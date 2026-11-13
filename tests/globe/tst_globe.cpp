// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include <QColor>
#include <QDir>
#include <QImage>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#include "goldenimage.h"
#include "mockhttpserver.h"
#include "mockwms.h"
#include "offscreengl.h"
#include "ts/camera/camera.h"
#include "ts/elevation/heightgrid.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodetic.h"
#include "ts/globe/globerenderer.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/globe/tilerenderer.h"
#include "ts/globe/vectorrenderer.h"
#include "ts/layers/layerdefinition.h"
#include "ts/layers/layertreemodel.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/packs/packmanifest.h"
#include "ts/projection/projection.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/providerregistry.h"
#include "ts/providers/testproviders.h"
#include "ts/regions/regiondownload.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"

namespace
{

constexpr int imageWidthPx = 320;
constexpr int imageHeightPx = 240;

/**
 * Golden images are compared only on Mesa llvmpipe, whose output is the
 * same everywhere for a given version; other drivers differ in rounding.
 * Channel tolerance and differing-pixel fraction allow for small changes
 * between llvmpipe versions (Linux packages, the Windows Mesa build).
 */
constexpr ts::test::GoldenTolerance goldenTolerance{12, 0.005};

void appendUint32(QByteArray* wkb, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    wkb->append(
        static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xffU));
  }
}

void appendDouble(QByteArray* wkb, double value)
{
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  for (int i = 0; i < 8; ++i) {
    wkb->append(
        static_cast<char>((bits >> (8U * static_cast<unsigned>(i))) & 0xffU));
  }
}

/** Little-endian WKB of a polygon from rings of lon, lat pairs. */
QByteArray polygonWkb(const std::vector<std::vector<double>>& rings)
{
  QByteArray wkb;
  wkb.append(static_cast<char>(1));
  appendUint32(&wkb, 3);
  appendUint32(&wkb, static_cast<std::uint32_t>(rings.size()));
  for (const std::vector<double>& ring : rings) {
    appendUint32(&wkb, static_cast<std::uint32_t>(ring.size() / 2 + 1));
    for (const double value : ring) {
      appendDouble(&wkb, value);
    }
    appendDouble(&wkb, ring[0]);
    appendDouble(&wkb, ring[1]);
  }
  return wkb;
}

/**
 * Writes a GeoPackage with one table stored at level 0 for every level:
 * a polygon split at the antimeridian (both halves end on 180°, an
 * artificial edge not to be outlined), a polar cap down to the north pole
 * in each hemisphere tile, and an island with a lake.
 */
bool writeVectorFixture(const QString& path)
{
  ts::GeoPackageFeatureWriter writer(path);
  ts::FeatureTableSpec table;
  table.name = QStringLiteral("land");
  table.storageLevel = 0;
  table.minLevel = 0;
  table.maxLevel = -1;
  const ts::TileKey west{0, 0, 0};
  const ts::TileKey east{0, 1, 0};
  return writer.addTable(table) &&
         writer.putFeature(
             table.name, east,
             polygonWkb({{170, -25, 180, -25, 180, -5, 170, -5}})) &&
         writer.putFeature(
             table.name, west,
             polygonWkb({{-180, -25, -165, -25, -165, -10, -180, -10}})) &&
         writer.putFeature(table.name, west,
                           polygonWkb({{-180, 75, 0, 75, 0, 90, -180, 90}})) &&
         writer.putFeature(table.name, east,
                           polygonWkb({{0, 75, 180, 75, 180, 90, 0, 90}})) &&
         writer.putFeature(table.name, east,
                           polygonWkb({{20, 30, 40, 30, 40, 50, 20, 50},
                                       {25, 35, 32, 35, 32, 42, 25, 42}})) &&
         writer.finish();
}

ts::Camera makeCamera(const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(imageWidthPx, imageHeightPx);
  camera.setPose(pose);
  return camera;
}

/** Centroid of the pixels where red clearly exceeds blue (the markers). */
std::optional<QPointF> redCentroid(const QImage& image)
{
  double sum = 0.0;
  double sumX = 0.0;
  double sumY = 0.0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      const double weight = std::max(0, c.red() - c.blue());
      sum += weight;
      sumX += weight * (x + 0.5);
      sumY += weight * (y + 0.5);
    }
  }
  if (sum <= 0.0) {
    return std::nullopt;
  }
  return QPointF(sumX / sum, sumY / sum);
}

int to8Bit(float value)
{
  return qRound(value * 255.0F);
}

/** True if (x, y) shows the globe rather than the background colour. */
bool isGlobe(const QImage& image, int x, int y)
{
  const QColor c = image.pixelColor(x, y);
  const ts::GlobeStyle style;
  return std::abs(c.red() - to8Bit(style.backgroundRgb[0])) +
             std::abs(c.green() - to8Bit(style.backgroundRgb[1])) +
             std::abs(c.blue() - to8Bit(style.backgroundRgb[2])) >
         6;
}

/**
 * Steep synthetic mountains: 4 km ridges less than a degree apart, sea
 * level 30 m above the ellipsoid. Tiles of different levels sample them
 * differently, so neighbours at different levels disagree, which the
 * skirts must hide.
 */
class SineElevation : public ts::ElevationProvider
{
public:
  QString id() const override { return QStringLiteral("test:sine"); }
  QString name() const override { return QStringLiteral("Sine"); }
  int maximumLevel() const override { return 8; }

  static double heightM(double latDeg, double lonDeg)
  {
    return 30.0 + 2000.0 +
           2000.0 * std::sin(latDeg * 7.0) * std::cos(lonDeg * 9.0);
  }

  std::optional<ts::HeightTile>
  loadHeights(ts::TileKey key, const std::atomic<bool>& /*cancelled*/) override
  {
    // Beyond level 8, the level-8 ancestor's samples interpolated.
    ts::TileKey stored = key;
    while (stored.level > maximumLevel()) {
      stored = stored.parent();
    }
    ts::HeightTile tile;
    tile.key = stored;
    tile.samples = 17;
    for (int row = 0; row < 17; ++row) {
      for (int column = 0; column < 17; ++column) {
        const ts::GeoPoint point =
            ts::heightSamplePoint(stored, 17, row, column);
        tile.heightsM.push_back(
            static_cast<float>(heightM(point.latDeg, point.lonDeg)));
        tile.seaLevelM.push_back(30.0F);
      }
    }
    return stored == key ? tile : ts::resampleHeightTile(tile, key, 17);
  }
};

/**
 * SineElevation that fails the first time each tile is asked for (an
 * incomplete tile at sea level, like a web source that didn't answer).
 */
class FlakyElevation : public SineElevation
{
public:
  QString id() const override { return QStringLiteral("test:flaky"); }

  std::optional<ts::HeightTile>
  loadHeights(ts::TileKey key, const std::atomic<bool>& cancelled) override
  {
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      if (m_asked.insert(key).second) {
        ts::HeightTile tile;
        tile.key = key;
        tile.samples = 2;
        tile.heightsM.assign(4, 30.0F);
        tile.seaLevelM.assign(4, 30.0F);
        tile.incomplete = true;
        return tile;
      }
      ++m_retries;
    }
    return SineElevation::loadHeights(key, cancelled);
  }

  /** Tiles asked for again after an incomplete answer. */
  int retries() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_retries;
  }

private:
  mutable std::mutex m_mutex;
  std::unordered_set<ts::TileKey, ts::TileKeyHash> m_asked;
  int m_retries = 0;
};

} // namespace

/** GPU tests for ts/globe: golden images, precision near the surface. */
class TestGlobe : public QObject
{
  Q_OBJECT

private:
  /** Renders @p camera into an image (needs a valid m_gl). */
  /** Renders until nothing is loading any more, then once more. */
  QImage renderSettled(const ts::Camera& camera)
  {
    QImage image;
    const bool settled = QTest::qWaitFor(
        [&]() {
          image = render(camera);
          return !m_renderer->isBusy();
        },
        60000);
    if (!settled) {
      return QImage();
    }
    return render(camera);
  }

  QImage render(const ts::Camera& camera,
                const std::vector<ts::GlobeMarker>& markers = {})
  {
    m_gl->makeCurrent();
    m_renderer->setMarkers(markers);
    ts::PassDesc pass;
    pass.target = m_target.get();
    pass.viewportPx = QRect(0, 0, imageWidthPx, imageHeightPx);
    m_renderer->render(camera, pass, 1.0);
    return m_gl->device().readPixels(*m_target);
  }

  /** A WMS overlay from the mock server, in @p crs. */
  std::shared_ptr<ts::WmsImageryProvider> mockWms(const QString& crs)
  {
    if (!m_wmsServer) {
      m_wmsServer =
          std::make_unique<ts::test::MockHttpServer>(ts::test::mockWmsHandler(
              std::make_shared<ts::test::MockWmsBehaviour>()));
      ts::NetworkSettings settings;
      settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
      m_network = std::make_shared<ts::NetworkClient>(settings);
    }
    ts::WmsSource source;
    source.url = m_wmsServer->url(QStringLiteral("/wms"));
    source.layers = QStringList{QStringLiteral("pattern")};
    source.styles = QStringList{QString()};
    source.crs = crs;
    source.title = QStringLiteral("Mock overlay");
    return std::make_shared<ts::WmsImageryProvider>(source, m_network);
  }

  std::unique_ptr<ts::test::MockHttpServer> m_wmsServer;
  std::shared_ptr<ts::NetworkClient> m_network;
  std::unique_ptr<ts::test::OffscreenGl> m_gl;
  std::unique_ptr<ts::GlobeRenderer> m_renderer;
  std::unique_ptr<ts::RenderTarget> m_target;
  bool m_softwareRenderer = false;
  std::array<QImage, 3> m_compositingImages;

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void initTestCase()
  {
    m_gl = std::make_unique<ts::test::OffscreenGl>();
    if (!m_gl->isValid()) {
      return;
    }
    m_renderer = std::make_unique<ts::GlobeRenderer>(m_gl->device());
    QString error;
    QVERIFY2(m_renderer->initialise(&error), qPrintable(error));
    m_target = m_gl->device().createRenderTarget(imageWidthPx, imageHeightPx);
    QVERIFY(m_target);
    const ts::RenderDeviceInfo info = m_gl->device().info();
    m_softwareRenderer = info.renderer.contains(QLatin1String("llvmpipe"));
    qInfo("renderer: %s", qPrintable(info.renderer));
  }

  void cleanupTestCase()
  {
    if (m_gl && m_gl->isValid()) {
      m_gl->makeCurrent();
    }
    m_target.reset();
    m_renderer.reset();
    m_gl.reset();
  }

  void goldenImages_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("headingDeg");
    QTest::addColumn<double>("tiltDeg");
    QTest::addColumn<bool>("withMarkers");
    QTest::newRow("whole-globe") << 20.0 << 0.0 << 1.6e7 << 0.0 << 0.0 << false;
    QTest::newRow("europe-tilted")
        << 48.0 << 10.0 << 3.0e6 << 30.0 << 50.0 << false;
    QTest::newRow("north-pole") << 89.9 << 0.0 << 1.5e7 << 0.0 << 0.0 << false;
    QTest::newRow("antimeridian-markers")
        << -15.0 << 180.0 << 8.0e6 << 0.0 << 20.0 << true;
    QTest::newRow("low-horizon")
        << 45.0 << 7.0 << 3000.0 << 0.0 << 80.0 << false;
  }

  void goldenImages()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, headingDeg);
    QFETCH(double, tiltDeg);
    QFETCH(bool, withMarkers);
    std::vector<ts::GlobeMarker> markers;
    if (withMarkers) {
      for (const double lon : {170.0, 180.0, -170.0}) {
        ts::GlobeMarker marker;
        marker.positionEcef = ts::geodeticToEcef(ts::Geodetic{-15.0, lon, 0.0});
        marker.rgba = {230, 40, 40, 255};
        marker.radiusPx = 5.0;
        markers.push_back(marker);
      }
    }
    const QImage image = render(
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, headingDeg, tiltDeg}),
        markers);
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void checkerboardGolden_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    QTest::newRow("checkerboard-globe") << 20.0 << 10.0 << 1.6e7 << 0.0;
    QTest::newRow("checkerboard-tilted") << 48.0 << 10.0 << 2.0e6 << 50.0;
    QTest::newRow("checkerboard-pole") << 89.0 << 0.0 << 6.0e6 << 20.0;
  }

  void checkerboardGolden()
  {
    // The globe drawn from the tile quadtree with the checkerboard test
    // imagery, once every wanted tile has loaded.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg});
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera);
          return !m_renderer->isBusy();
        },
        30000));
    // Once more, now that every tile is uploaded.
    image = render(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
    qInfo("%d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    QVERIFY(tiles.rendered > 0);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void layerCompositing_data()
  {
    QTest::addColumn<int>("scene");
    QTest::newRow("two-layers-50") << 0;
    QTest::newRow("group-composite") << 1;
    QTest::newRow("group-multiply") << 2;
  }

  void layerCompositing()
  {
    // 0: the checkerboard with red at 50 % over it. 1 and 2: a collection
    //    at 50 % holding the checkerboard and opaque blue over it; drawn as
    //    one composite image (1) the checkerboard doesn't show through, with
    //    multiplied opacity (2) it does.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(int, scene);
    const std::shared_ptr<ts::ImageryProvider> checkerboard =
        std::make_shared<ts::CheckerboardImageryProvider>();
    const std::shared_ptr<ts::ImageryProvider> red =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(255, 0, 0), 64);
    const std::shared_ptr<ts::ImageryProvider> blue =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(0, 0, 255), 64);
    std::vector<ts::ImageryLayerDraw> layers;
    std::vector<double> groups;
    if (scene == 0) {
      layers = {{checkerboard, 1.0, -1}, {red, 0.5, -1}};
    } else {
      layers = {{checkerboard, 1.0, 0}, {blue, 1.0, 0}};
      groups = {0.5};
    }
    m_gl->makeCurrent();
    m_renderer->setGroupCompositing(scene != 2);
    m_renderer->setImageryLayers(layers, groups);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{48.0, 10.0, 2.0e6, 0.0, 30.0});
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera);
          return !m_renderer->isBusy();
        },
        30000));
    image = render(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setGroupCompositing(true);
    QCOMPARE(tiles.groupsComposited, scene == 1 ? 1 : 0);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // In the middle of the view: red over the checkerboard keeps half its
    // pattern; the composite group is plain blue over the globe; the
    // multiplied one lets the checkerboard through.
    const QColor centre = image.pixelColor(imageWidthPx / 2, imageHeightPx / 2);
    if (scene == 0) {
      QVERIFY2(centre.red() > 140 && centre.blue() < 120,
               qPrintable(centre.name()));
    } else {
      QVERIFY2(centre.blue() > 110 && centre.red() < 100,
               qPrintable(centre.name()));
    }
    m_compositingImages.at(static_cast<std::size_t>(scene)) = image;
    if (scene == 2) {
      QVERIFY(m_compositingImages[1] != m_compositingImages[2]);
    }
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void layersShareSources()
  {
    // Two layers with the same provider share one loader and set of
    // textures; a third provider adds a second source.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::TileRenderer tiles(m_gl->device());
    QString error;
    QVERIFY2(tiles.initialise(&error), qPrintable(error));
    const std::shared_ptr<ts::ImageryProvider> shared =
        std::make_shared<ts::CheckerboardImageryProvider>(64);
    const std::shared_ptr<ts::ImageryProvider> other =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(0, 255, 0));
    tiles.setLayers({{shared, 1.0, -1}, {shared, 0.5, -1}}, {});
    QCOMPARE(tiles.sourceCount(), 1);
    tiles.setLayers({{shared, 1.0, -1}, {other, 0.5, -1}, {shared, 0.2, -1}},
                    {});
    QCOMPARE(tiles.sourceCount(), 2);
    tiles.setLayers({}, {});
    QCOMPARE(tiles.sourceCount(), 0);
  }

  void vectorGolden_data()
  {
    QTest::addColumn<int>("scene");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    // Fill and outline at the antimeridian (no line along 180°), at the
    // north pole (no line along the pole or 0°/180°), an island with a
    // lake; and the graticule layer over imagery.
    QTest::newRow("vector-antimeridian") << 0 << -15.0 << 180.0 << 6.0e6 << 0.0;
    QTest::newRow("vector-pole") << 0 << 80.0 << 30.0 << 6.0e6 << 30.0;
    QTest::newRow("vector-lake") << 0 << 40.0 << 30.0 << 4.0e6 << 0.0;
    QTest::newRow("graticule-over-imagery")
        << 1 << 20.0 << 10.0 << 1.6e7 << 0.0;
  }

  void vectorGolden()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(int, scene);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    const QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("v.gpkg"));
    QVERIFY(writeVectorFixture(path));
    const std::shared_ptr<ts::GeoPackageFeatureProvider> features =
        std::make_shared<ts::GeoPackageFeatureProvider>(path);
    QVERIFY(features->isValid());

    ts::VectorLayerDraw graticule;
    graticule.kind = ts::VectorLayerDraw::Kind::Graticule;
    graticule.style.outline = QColor(255, 255, 255, 200);
    graticule.style.outlineWidthPx = 1.0;
    ts::VectorLayerDraw land;
    land.provider = features;
    land.style.fill = QColor(200, 185, 140, 200);
    land.style.outline = QColor(255, 220, 40);
    land.style.outlineWidthPx = 2.0;

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    if (scene == 1) {
      // Red, to stand out on the pale checkerboard.
      graticule.style.outline = QColor(220, 30, 30);
      graticule.style.outlineWidthPx = 1.5;
      m_renderer->setImageryProvider(
          std::make_shared<ts::CheckerboardImageryProvider>());
      m_renderer->setVectorLayers({graticule});
    } else {
      // No graticule here: at close range llvmpipe versions place its 1°
      // parallels a pixel apart (their asin differs); it has its own
      // golden images at whole-globe scale.
      m_renderer->setVectorLayers({land});
    }
    const QImage image = renderSettled(
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg}));
    const ts::VectorStatistics vectors = m_renderer->vectorStatistics();
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (scene == 0) {
      QVERIFY(vectors.tilesDrawn > 0);
      QVERIFY(vectors.triangles > 0);
      QVERIFY(vectors.segments > 0);
    }
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void morphGolden_data()
  {
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("headingDeg");
    QTest::addColumn<double>("tiltDeg");
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    const QString web = QStringLiteral("web-mercator");
    // The change from globe to map: t = 0, 0.5 and 1 with the same centre.
    QTest::newRow("morph-0")
        << mercator << 0.0 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    QTest::newRow("morph-half")
        << mercator << 0.5 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    QTest::newRow("morph-1")
        << mercator << 1.0 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    // Part of the way, the camera still turned and tilted.
    QTest::newRow("morph-quarter-tilted")
        << miller << 0.25 << 35.0 << 30.0 << 1.2e7 << 30.0 << 40.0;
    // The other projections, the whole world.
    QTest::newRow("map-miller")
        << miller << 1.0 << 0.0 << 0.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-web-mercator")
        << web << 1.0 << 0.0 << -60.0 << 5.0e7 << 0.0 << 0.0;
    // Centred on the antimeridian: polygons that cross it, markers either
    // side, no streaks across the map.
    QTest::newRow("map-antimeridian")
        << mercator << 1.0 << -15.0 << 180.0 << 8.0e6 << 0.0 << 0.0;
    QTest::newRow("map-antimeridian-wide")
        << miller << 1.0 << 10.0 << 179.0 << 5.0e7 << 0.0 << 0.0;
    // The top edge of the Mercator map and Miller's pole.
    QTest::newRow("map-mercator-north")
        << mercator << 1.0 << 78.0 << 30.0 << 4.0e6 << 0.0 << 0.0;
    QTest::newRow("map-miller-north")
        << miller << 1.0 << 80.0 << 30.0 << 2.0e6 << 0.0 << 0.0;
    // Close in: the island with its lake.
    QTest::newRow("map-lake")
        << mercator << 1.0 << 40.0 << 30.0 << 4.0e6 << 0.0 << 0.0;
  }

  void morphGolden()
  {
    // Every kind of layer on the flat map and part of the way to it:
    // imagery, vector fills and outlines, the graticule layer, markers.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, headingDeg);
    QFETCH(double, tiltDeg);
    const QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("v.gpkg"));
    QVERIFY(writeVectorFixture(path));
    const std::shared_ptr<ts::GeoPackageFeatureProvider> features =
        std::make_shared<ts::GeoPackageFeatureProvider>(path);
    QVERIFY(features->isValid());

    ts::VectorLayerDraw graticule;
    graticule.kind = ts::VectorLayerDraw::Kind::Graticule;
    graticule.style.outline = QColor(220, 30, 30);
    graticule.style.outlineWidthPx = 1.5;
    ts::VectorLayerDraw land;
    land.provider = features;
    land.style.fill = QColor(200, 185, 140, 200);
    land.style.outline = QColor(255, 220, 40);
    land.style.outlineWidthPx = 2.0;
    std::vector<ts::GlobeMarker> markers;
    for (const double lon : {170.0, 180.0, -170.0}) {
      ts::GlobeMarker marker;
      marker.positionEcef = ts::geodeticToEcef(ts::Geodetic{-15.0, lon, 0.0});
      marker.rgba = {230, 40, 40, 255};
      marker.radiusPx = 5.0;
      markers.push_back(marker);
    }

    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setMapProjection(ts::projectionById(projection));
    camera.setPose(ts::CameraPose{latDeg, lonDeg, rangeM, headingDeg, tiltDeg});
    camera.setMorph(morph);

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    m_renderer->setVectorLayers({land, graticule});
    QImage image;
    const bool settled = QTest::qWaitFor(
        [&]() {
          image = render(camera, markers);
          return !m_renderer->isBusy();
        },
        60000);
    image = render(camera, markers);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setMarkers({});
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(settled);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("%d tiles drawn, deepest level %d, %d draw calls", tiles.rendered,
          tiles.deepestLevel, tiles.drawCalls);
    QVERIFY(tiles.rendered > 0);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void centreStaysFixedThroughTheMorph()
  {
    // A marker at the centre of the view, and one a little way off, drawn
    // at steps from globe to map: the first never leaves the middle of
    // the image, and neither jumps from one step to the next.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::CameraPose pose{35.0, 139.0, 4.0e6, 0.0, 0.0};
    ts::GlobeMarker centre;
    centre.positionEcef = ts::geodeticToEcef(ts::Geodetic{35.0, 139.0, 0.0});
    centre.rgba = {255, 0, 0, 255};
    centre.radiusPx = 4.0;
    ts::GlobeMarker other = centre;
    other.positionEcef = ts::geodeticToEcef(ts::Geodetic{43.0, 131.0, 0.0});
    for (const char* id : {"mercator", "miller"}) {
      ts::Camera camera = makeCamera(pose);
      camera.setMapProjection(ts::projectionById(QLatin1String(id)));
      std::optional<QPointF> previous;
      for (int step = 0; step <= 10; ++step) {
        camera.setMorph(0.1 * step);
        const std::optional<QPointF> atCentre =
            redCentroid(render(camera, {centre}));
        if (!atCentre) {
          QFAIL("centre marker not drawn");
        }
        QVERIFY2(std::abs(atCentre->x() - imageWidthPx / 2.0) < 0.6 &&
                     std::abs(atCentre->y() - imageHeightPx / 2.0) < 0.6,
                 qPrintable(QStringLiteral("%1, t = %2: (%3, %4)")
                                .arg(QLatin1String(id))
                                .arg(0.1 * step)
                                .arg(atCentre->x())
                                .arg(atCentre->y())));
        const std::optional<QPointF> atOther =
            redCentroid(render(camera, {other}));
        if (!atOther) {
          QFAIL("second marker not drawn");
        }
        // Where the camera says it is, at both ends of the change.
        if (step == 0 || step == 10) {
          const ts::ScreenPoint expected =
              camera.projectGeodetic(43.0, 131.0)
                  .value_or(ts::ScreenPoint{-1e9, -1e9, 0.0});
          QVERIFY(std::abs(atOther->x() - expected.xPx) < 0.6 &&
                  std::abs(atOther->y() - expected.yPx) < 0.6);
        }
        if (previous) {
          QVERIFY(std::hypot(atOther->x() - previous->x(),
                             atOther->y() - previous->y()) < 4.0);
        }
        previous = atOther;
      }
    }
    m_gl->makeCurrent();
    m_renderer->setMarkers({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void mapWrapsAroundTheWorld()
  {
    // Zoomed right out on the antimeridian the map fills the view from
    // edge to edge: the tiles that straddle the wrap are drawn on both
    // sides, and no column of pixels is left as background.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setMapProjection(ts::projectionById(QStringLiteral("miller")));
    camera.setMorph(1.0);
    camera.setPose(ts::CameraPose{0.0, 170.0, 5.0e7, 0.0, 0.0});
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    const QImage image = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // 8 × 4 tiles of level 2, and one column of 4 twice.
    QCOMPARE(tiles.deepestLevel, 2);
    QCOMPARE(tiles.rendered, 36);
    const QColor background(image.pixel(2, 2));
    const QColor sea(image.pixel(imageWidthPx / 2, imageHeightPx / 2));
    QVERIFY(sea.blue() > background.blue() + 40);
    for (int x = 0; x < imageWidthPx; ++x) {
      const QColor pixel(image.pixel(x, imageHeightPx / 2));
      QVERIFY2(std::abs(pixel.blue() - sea.blue()) <= 2,
               qPrintable(QStringLiteral("column %1").arg(x)));
    }
    // The flat map is not shaded: the surface colour itself.
    const ts::GlobeStyle defaults;
    QVERIFY(std::abs(sea.red() - to8Bit(defaults.surfaceRgb[0])) <= 2);
    QVERIFY(std::abs(sea.green() - to8Bit(defaults.surfaceRgb[1])) <= 2);
    QVERIFY(std::abs(sea.blue() - to8Bit(defaults.surfaceRgb[2])) <= 2);
  }

  void morphHoldsFrameRate()
  {
    // The change from globe to map with every kind of layer on screen,
    // frame by frame as the application draws it. The budget on software
    // rendering is 10 frames per second (20 on the reference hardware, a
    // GPU several times faster than this); the limit here leaves room for
    // a loaded CI machine.
    TS_TEST_REQUIRE_GL(*m_gl);
    const QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("v.gpkg"));
    QVERIFY(writeVectorFixture(path));
    ts::VectorLayerDraw graticule;
    graticule.kind = ts::VectorLayerDraw::Kind::Graticule;
    ts::VectorLayerDraw land;
    land.provider = std::make_shared<ts::GeoPackageFeatureProvider>(path);
    land.style.fill = QColor(200, 185, 140, 200);
    land.style.outline = QColor(255, 220, 40);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    m_renderer->setVectorLayers({land, graticule});
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
    // Everything loaded at both ends first, as after a moment's viewing.
    for (const double morph : {1.0, 0.5, 0.0}) {
      camera.setMorph(morph);
      QVERIFY(!renderSettled(camera).isNull());
    }
    constexpr int frames = 30;
    std::vector<double> timesS;
    for (int frame = 0; frame <= frames; ++frame) {
      const double u = static_cast<double>(frame) / frames;
      camera.setMorph(u * u * (3.0 - 2.0 * u));
      QElapsedTimer one;
      one.start();
      render(camera);
      timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
    }
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    std::sort(timesS.begin(), timesS.end());
    double sumS = 0.0;
    for (const double timeS : timesS) {
      sumS += timeS;
    }
    const double meanS = sumS / static_cast<double>(timesS.size());
    // The quarter of the frames least disturbed by whatever else the
    // machine is doing: a busy CI runner must not fail this.
    const double quartileS = timesS.at(timesS.size() / 4);
    qInfo("morph: %.1f ms per frame (%.0f fps), fastest quarter within "
          "%.1f ms, slowest %.1f ms",
          meanS * 1000.0, 1.0 / meanS, quartileS * 1000.0,
          timesS.back() * 1000.0);
#if defined(TS_INSTRUMENTED_BUILD) || !defined(QT_NO_DEBUG)
    // Sanitizer, coverage and debug builds are several times slower: the
    // change is drawn (and checked for errors) but not timed.
    Q_UNUSED(quartileS)
#else
    QVERIFY2(quartileS < 0.1, "the morph is below 10 frames per second");
#endif
  }

  void wmsGolden_data()
  {
    QTest::addColumn<QString>("crs");
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    const QString geographic = QStringLiteral("EPSG:4326");
    const QString webMercator = QStringLiteral("EPSG:3857");
    const QString none;
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    // The mock WMS's test pattern at 50 % over plain imagery: red discs at
    // every 30° intersection, yellow bands at 45° N and 60° E.
    QTest::newRow("wms-overlay-globe")
        << geographic << none << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("wms-overlay-close")
        << geographic << none << 0.0 << 45.0 << 60.0 << 1.5e6;
    QTest::newRow("wms-overlay-mercator")
        << geographic << mercator << 1.0 << 30.0 << 20.0 << 2.6e7;
    QTest::newRow("wms-overlay-miller")
        << geographic << miller << 1.0 << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("wms-overlay-morph-half")
        << geographic << mercator << 0.5 << 30.0 << 20.0 << 2.6e7;
    // A server with Web Mercator only: reprojected, the same picture.
    QTest::newRow("wms-web-mercator-globe")
        << webMercator << none << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("wms-web-mercator-close")
        << webMercator << none << 0.0 << 60.0 << 30.0 << 1.2e6;
    QTest::newRow("wms-web-mercator-miller")
        << webMercator << miller << 1.0 << 0.0 << 0.0 << 5.0e7;
  }

  void wmsGolden()
  {
    // A WMS layer at 50 % over plain imagery, through the network client
    // and the provider, from a local mock server.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, crs);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(morph);
    }
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(
        {ts::ImageryLayerDraw{std::make_shared<ts::SolidColorImageryProvider>(
                                  QColor(40, 80, 130), 64, 12),
                              1.0, -1},
         ts::ImageryLayerDraw{mockWms(crs), 0.5, -1}},
        {});
    const QImage image = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("%d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void wmsWebMercatorMatchesGeographic_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("morph");
    QTest::newRow("whole globe") << 30.0 << 20.0 << 1.6e7 << 0.0;
    QTest::newRow("60 N close") << 60.0 << 30.0 << 1.2e6 << 0.0;
    // Up to 86° N, near the Mercator limit (the pole itself has no
    // Mercator picture, so it is left out of view).
    QTest::newRow("near the limit") << 72.0 << -30.0 << 4.0e6 << 0.0;
    QTest::newRow("deep") << 45.0 << 60.0 << 2.0e4 << 0.0;
    QTest::newRow("Mercator map") << 30.0 << 0.0 << 2.0e7 << 1.0;
  }

  void wmsWebMercatorMatchesGeographic()
  {
    // The same test pattern from a server in EPSG:4326 and from one in
    // Web Mercator, reprojected in the shader: the two pictures agree
    // except along the edges of the discs and lines, where the two images'
    // pixels fall differently.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, morph);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    camera.setMorph(morph);
    std::array<QImage, 2> images;
    for (std::size_t i = 0; i < images.size(); ++i) {
      m_gl->makeCurrent();
      ts::GlobeStyle style;
      style.graticule = false;
      m_renderer->setStyle(style);
      m_renderer->setImageryLayers(
          {ts::ImageryLayerDraw{mockWms(i == 0 ? QStringLiteral("EPSG:4326")
                                               : QStringLiteral("EPSG:3857")),
                                1.0, -1}},
          {});
      images.at(i) = renderSettled(camera);
      QVERIFY(!images.at(i).isNull());
    }
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    const ts::test::ImageDifference difference =
        ts::test::compareImages(images[1], images[0], 40);
    qInfo("%.2f %% of pixels differ by more than 40",
          100.0 * difference.differingFraction);
    // Something is drawn at all: the pattern covers part of the view.
    int coloured = 0;
    for (int y = 0; y < images[0].height(); y += 4) {
      for (int x = 0; x < images[0].width(); x += 4) {
        const QColor c(images[0].pixel(x, y));
        coloured += c.red() > c.blue() + 60 ? 1 : 0;
      }
    }
    QVERIFY(coloured > 20);
    QVERIFY2(difference.differingFraction < 0.01,
             qPrintable(QString::number(difference.differingFraction)));
  }

  void offlineRegionGolden()
  {
    // A region over the British Isles downloaded from the mock WMS, then
    // drawn with no network at all (every request goes to a proxy on a
    // closed port): the same picture as online, from the pinned tiles.
    TS_TEST_REQUIRE_GL(*m_gl);
    QTemporaryDir cacheDir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = cacheDir.path();
    ts::WmsSource source;
    source.layers = QStringList{QStringLiteral("pattern")};
    source.styles = QStringList{QString()};
    source.title = QStringLiteral("Mock overlay");
    const ts::RegionGeometry region =
        ts::RegionGeometry::rectangle(-12.0, 49.0, 8.0, 62.0);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{55.0, -2.0, 7.0e5, 0.0, 0.0});
    const std::shared_ptr<ts::ImageryProvider> background =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(40, 80, 130), 64,
                                                        12);
    std::array<QImage, 2> images;
    {
      ts::test::MockHttpServer server(ts::test::mockWmsHandler(
          std::make_shared<ts::test::MockWmsBehaviour>()));
      source.url = server.url(QStringLiteral("/wms"));
      ts::NetworkSettings online;
      online.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
      std::shared_ptr<ts::WmsImageryProvider> provider =
          std::make_shared<ts::WmsImageryProvider>(
              source, std::make_shared<ts::NetworkClient>(online), settings);
      ts::RegionDownload download(provider, 1, region, 0, 8);
      QString error;
      QVERIFY(download.start({}, &error));
      QVERIFY(download.waitUntilSettled(std::chrono::seconds(120)));
      QCOMPARE(download.progress().state,
               ts::RegionDownloadProgress::State::Finished);
      QCOMPARE(download.progress().failedTiles, 0LL);
      m_gl->makeCurrent();
      ts::GlobeStyle style;
      style.graticule = false;
      m_renderer->setStyle(style);
      m_renderer->setImageryLayers({ts::ImageryLayerDraw{background, 1.0, -1},
                                    ts::ImageryLayerDraw{provider, 0.5, -1}},
                                   {});
      images[0] = renderSettled(camera);
      qInfo("online: deepest level %d",
            m_renderer->tileStatistics().deepestLevel);
      m_gl->makeCurrent();
      m_renderer->setImageryLayers({}, {});
    }
    // The server is gone, and the network is unusable anyway.
    ts::test::MockHttpServer closed;
    ts::NetworkSettings offline;
    offline.proxy = QNetworkProxy(QNetworkProxy::HttpProxy,
                                  QStringLiteral("127.0.0.1"), closed.port());
    offline.attempts = 1;
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(offline);
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(source, network, settings);
    QCOMPARE(provider->diskCache()->regionUsage(1).tiles,
             ts::countRegionTiles(region, 0, 8, std::nullopt).total());
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({ts::ImageryLayerDraw{background, 1.0, -1},
                                  ts::ImageryLayerDraw{provider, 0.5, -1}},
                                 {});
    images[1] = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!images[0].isNull() && !images[1].isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("offline: %d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    QVERIFY(tiles.deepestLevel >= 5);
    // Not one request, and every tile from the disk.
    QCOMPARE(network->attemptsSent(), 0);
    QCOMPARE(provider->statistics().requests, 0);
    QVERIFY(provider->statistics().cacheHits > 0);
    QCOMPARE(images[1], images[0]);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        images[1], QStringLiteral("offline-region-uk"), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void basePackWmsGolden_data()
  {
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::newRow("base-wms-globe")
        << QString() << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("base-wms-mercator")
        << QStringLiteral("mercator") << 1.0 << 30.0 << 20.0 << 2.6e7;
    QTest::newRow("base-wms-miller")
        << QStringLiteral("miller") << 1.0 << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("base-wms-morph-half")
        << QStringLiteral("mercator") << 0.5 << 30.0 << 20.0 << 2.6e7;
  }

  void basePackWmsGolden()
  {
    // The mock WMS overlay at 50 % over Blue Marble, set up through the
    // layer tree and the provider registry like the application.
    TS_TEST_REQUIRE_GL(*m_gl);
    const QString directory = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (directory.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QString error;
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(directory, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    const std::shared_ptr<ts::WmsImageryProvider> mock =
        mockWms(QStringLiteral("EPSG:4326"));
    ts::ProviderRegistry registry;
    registry.setBasePack(directory, *manifest);
    registry.setWmsNetwork(m_network, ts::WmsProviderSettings());
    ts::LayerTreeModel model;
    for (const ts::LayerDefinition& definition :
         ts::builtinLayerDefinitions()) {
      if (definition.id == QLatin1String("builtin:bluemarble")) {
        model.addLayer(definition);
      }
    }
    ts::LayerDefinition wms;
    wms.id = QStringLiteral("user:mock-wms");
    wms.origin = ts::LayerOrigin::User;
    wms.type = ts::LayerType::Imagery;
    wms.name = QStringLiteral("Mock overlay");
    wms.source = mock->source().toJson();
    const QModelIndex overlay = model.addLayer(wms);
    model.setData(overlay, 0.5, ts::LayerTreeModel::OpacityRole);
    const ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const ts::ImageryDraws imagery =
        ts::imageryDrawsForStack(stack, registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QCOMPARE(imagery.layers.size(), std::size_t{2});
    QCOMPARE(imagery.layers.back().alpha, 0.5);

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(imagery.layers, imagery.groupAlphas);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(morph);
    }
    const QImage image = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void basePackGolden_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    // The terrain's exaggeration; 0 for none (no elevation source).
    QTest::addColumn<double>("terrain");
    const QString none;
    QTest::newRow("base-uk")
        << 54.5 << -3.0 << 2.0e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-uk-close")
        << 51.5 << -0.5 << 1.5e5 << 30.0 << none << 0.0 << 0.0;
    QTest::newRow("base-pacific-antimeridian")
        << -17.5 << 179.5 << 1.5e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-pacific-wide")
        << 0.0 << 180.0 << 1.2e7 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-north-pole")
        << 90.0 << 0.0 << 7.0e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-north-pole-close")
        << 82.0 << -40.0 << 1.5e6 << 30.0 << none << 0.0 << 0.0;
    QTest::newRow("base-south-pole")
        << -90.0 << 0.0 << 9.0e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-south-pole-close")
        << -77.5 << 166.0 << 6.0e5 << 30.0 << none << 0.0 << 0.0;
    // The same data on the flat maps and half-way there.
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    QTest::newRow("base-map-mercator-world")
        << 0.0 << 0.0 << 5.0e7 << 0.0 << mercator << 1.0 << 0.0;
    QTest::newRow("base-map-miller-world")
        << 0.0 << 0.0 << 5.0e7 << 0.0 << miller << 1.0 << 0.0;
    QTest::newRow("base-map-uk")
        << 54.5 << -3.0 << 2.0e6 << 0.0 << mercator << 1.0 << 0.0;
    QTest::newRow("base-map-pacific-antimeridian")
        << -17.5 << 179.5 << 1.5e6 << 0.0 << mercator << 1.0 << 0.0;
    QTest::newRow("base-map-pacific-wide")
        << 0.0 << 180.0 << 1.2e7 << 0.0 << miller << 1.0 << 0.0;
    QTest::newRow("base-map-antarctica")
        << -75.0 << 0.0 << 6.0e6 << 0.0 << miller << 1.0 << 0.0;
    QTest::newRow("base-morph-half")
        << 30.0 << 20.0 << 1.6e7 << 0.0 << mercator << 0.5 << 0.0;
    // Terrain from the base pack's DEM, looking across mountains.
    QTest::newRow("base-terrain-alps")
        << 46.0 << 8.0 << 2.0e5 << 65.0 << none << 0.0 << 3.0;
    QTest::newRow("base-terrain-everest")
        << 27.0 << 86.9 << 3.0e5 << 65.0 << none << 0.0 << 2.0;
    QTest::newRow("base-terrain-erebus-antimeridian")
        << -77.0 << 167.0 << 2.0e5 << 65.0 << none << 0.0 << 3.0;
    // Half-way to the map the terrain is half flattened.
    QTest::newRow("base-terrain-morph-half")
        << 46.0 << 8.0 << 3.0e5 << 45.0 << mercator << 0.5 << 3.0;
    // Loaded but switched off: the same picture as without terrain (its
    // golden image is base-uk-close's).
    QTest::newRow("base-uk-close-terrain-off")
        << 51.5 << -0.5 << 1.5e5 << 30.0 << none << 0.0 << -1.0;
  }

  void basePackGolden()
  {
    // Blue Marble and the shoreline from the real base pack, set up through
    // the layer tree like the application. (Not the graticule: at close
    // range llvmpipe versions place its 1° parallels a pixel apart; it has
    // its own golden images at whole-globe scale.)
    TS_TEST_REQUIRE_GL(*m_gl);
    const QString directory = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (directory.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, terrain);
    QString error;
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(directory, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    ts::ProviderRegistry registry;
    registry.setBasePack(directory, *manifest);
    ts::LayerTreeModel model;
    for (const QString& id : {QStringLiteral("builtin:bluemarble"),
                              QStringLiteral("builtin:shoreline")}) {
      for (const ts::LayerDefinition& definition :
           ts::builtinLayerDefinitions()) {
        if (definition.id == id) {
          model.addLayer(definition);
        }
      }
    }
    const ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const ts::ImageryDraws imagery =
        ts::imageryDrawsForStack(stack, registry, &errors);
    const std::vector<ts::VectorLayerDraw> vectors =
        ts::vectorDrawsForStack(stack, registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QCOMPARE(ts::attributionsForStack(stack, registry).size(), 2);

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(imagery.layers, imagery.groupAlphas);
    m_renderer->setVectorLayers(vectors);
    if (terrain != 0.0) {
      const std::shared_ptr<ts::ElevationProvider> elevation =
          registry.elevation(
              QJsonObject{
                  {QStringLiteral("kind"), QStringLiteral("basepack")},
                  {QStringLiteral("dataset"), QStringLiteral("elevation")}},
              &error);
      QVERIFY2(elevation, qPrintable(error));
      m_renderer->setElevationProvider(elevation);
      m_renderer->setTerrain(terrain > 0.0, std::max(terrain, 1.0));
    }
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(morph);
    }
    const QImage image = renderSettled(camera);
    const ts::VectorStatistics drawn = m_renderer->vectorStatistics();
    qInfo("vectors: %d tiles, %lld triangles, %lld segments, %d draw calls",
          drawn.tilesDrawn, drawn.triangles, drawn.segments, drawn.drawCalls);
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryLayers({}, {});
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    QString golden = QString::fromLatin1(QTest::currentDataTag());
    golden.remove(QStringLiteral("-terrain-off"));
    const QString failure =
        ts::test::checkGolden(image, golden, goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void terrainHasNoCracks()
  {
    // Steep exaggerated terrain seen at a grazing angle, against a
    // magenta sky: below the first ground pixel of each column there must
    // be no sky at all, so no gap between tiles of different levels.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    style.backgroundRgb = {1.0F, 0.0F, 1.0F};
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    m_renderer->setTerrain(true, 5.0);
    const QImage image = renderSettled(
        makeCamera(ts::CameraPose{20.0, 30.0, 4.0e5, 30.0, 70.0}));
    m_gl->makeCurrent();

    m_renderer->setElevationProvider(nullptr);
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    const QString output = qEnvironmentVariable("TS_GOLDEN_OUTPUT");
    if (!output.isEmpty()) {
      image.save(QDir(output).filePath(QStringLiteral("terrain-cracks.png")));
    }
    const std::function<bool(int, int)> sky = [&image](int x, int y) {
      const QColor c = image.pixelColor(x, y);
      return c.red() > 240 && c.green() < 15 && c.blue() > 240;
    };
    int ground = 0;
    int cracks = 0;
    for (int x = 0; x < image.width(); ++x) {
      int y = 0;
      while (y < image.height() && sky(x, y)) {
        ++y;
      }
      for (; y < image.height(); ++y) {
        ++ground;
        cracks += sky(x, y) ? 1 : 0;
      }
    }
    QVERIFY(ground > image.width() * image.height() / 3);
    QCOMPARE(cracks, 0);
  }

  void terrainOffDrawsTheEllipsoid()
  {
    // Heights loaded but the terrain off: the same picture as without
    // them (the vertex shader adds nothing).
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    const ts::Camera camera =
        makeCamera(ts::CameraPose{20.0, 30.0, 2.0e6, 10.0, 40.0});
    const QImage plain = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    m_renderer->setTerrain(false, 5.0);
    const QImage off = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setTerrain(true, 5.0);
    const QImage on = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setImageryProvider(nullptr);
    QVERIFY(!plain.isNull() && !off.isNull() && !on.isNull());
    int differing = 0;
    int changed = 0;
    for (int y = 0; y < plain.height(); ++y) {
      for (int x = 0; x < plain.width(); ++x) {
        const QColor a = plain.pixelColor(x, y);
        const QColor b = off.pixelColor(x, y);
        const QColor c = on.pixelColor(x, y);
        differing += std::abs(a.red() - b.red()) +
                                 std::abs(a.green() - b.green()) +
                                 std::abs(a.blue() - b.blue()) >
                             6
                         ? 1
                         : 0;
        changed += a != c ? 1 : 0;
      }
    }
    QCOMPARE(differing, 0);
    // And switched on, it does change the picture.
    QVERIFY(changed > plain.width() * plain.height() / 10);
  }

  void incompleteHeightsAreAskedForAgain()
  {
    // A source that couldn't deliver a tile gets asked again, and the
    // terrain then takes the full heights (the mesh is rebuilt).
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::TileRendererSettings settings;
    settings.heightsRetryDelay = std::chrono::milliseconds(0);
    ts::GlobeRenderer renderer(m_gl->device(), settings);
    std::shared_ptr<FlakyElevation> flaky = std::make_shared<FlakyElevation>();
    renderer.setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    renderer.setElevationProvider(flaky);
    renderer.setTerrain(true, 1.0);
    const ts::CameraPose pose{46.0, 8.0, 3.0e5, 0.0, 0.0};
    const ts::Camera camera = makeCamera(pose);
    const std::function<void()> frame = [&]() {
      m_gl->makeCurrent();
      ts::PassDesc pass;
      pass.target = m_target.get();
      pass.viewportPx = QRect(0, 0, imageWidthPx, imageHeightPx);
      renderer.render(camera, pass, 1.0);
    };
    const double expected = SineElevation::heightM(46.0, 8.0);
    const bool settled = QTest::qWaitFor(
        [&]() {
          frame();
          const std::optional<double> height =
              renderer.terrainHeightM(46.0, 8.0);
          return !renderer.isBusy() && height &&
                 std::abs(*height - expected) < 50.0;
        },
        60000);
    m_gl->makeCurrent();
    renderer.setElevationProvider(nullptr);
    renderer.setImageryProvider(nullptr);
    QVERIFY2(settled, "the full heights never arrived");
    QVERIFY(flaky->retries() > 0);
  }

  void silhouetteMatchesPicking()
  {
    // Every pixel the CPU picking hits shows the globe and every pixel it
    // misses shows the background, except along the antialiased edge.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{10.0, 20.0, 1.6e7, 0.0, 0.0});
    const QImage image = render(camera);
    int mismatches = 0;
    for (int y = 0; y < imageHeightPx; ++y) {
      for (int x = 0; x < imageWidthPx; ++x) {
        // A pixel is decided if its centre and all four neighbours'
        // centres agree.
        const bool hit = camera.pickGeodetic(x + 0.5, y + 0.5).has_value();
        bool decided = true;
        for (const QPointF& offset :
             {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) {
          decided = decided && camera.pickGeodetic(x + 0.5 + offset.x(),
                                                   y + 0.5 + offset.y())
                                       .has_value() == hit;
        }
        if (decided && isGlobe(image, x, y) != hit) {
          ++mismatches;
        }
      }
    }
    QCOMPARE(mismatches, 0);
  }

  void horizonOneMetreAboveSurface()
  {
    // Looking towards the horizon from 1 m up. The horizon row from the
    // GPU must match CPU picking (double precision) to within a pixel; the
    // ray-cast's constant term is 3e-7 here and would vanish in float.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setPose(ts::CameraPose{51.4769, -0.0005, 1.0, 0.0, 85.0});
    QVERIFY(std::abs(camera.eyeGeodetic().heightM - 1.0) < 0.01);
    const QImage image = render(camera);
    const int x = imageWidthPx / 2;
    int cpuRow = -1;
    int gpuRow = -1;
    for (int y = 0; y < imageHeightPx; ++y) {
      if (cpuRow < 0 && camera.pickGeodetic(x + 0.5, y + 0.5)) {
        cpuRow = y;
      }
      if (gpuRow < 0 && isGlobe(image, x, y)) {
        gpuRow = y;
      }
    }
    qInfo("horizon row: CPU %d, GPU %d", cpuRow, gpuRow);
    QVERIFY(cpuRow > 0 && cpuRow < imageHeightPx - 1);
    QVERIFY(std::abs(cpuRow - gpuRow) <= 1);
  }

  void noJitterOneMetreAboveSurface()
  {
    // The camera is 1 m above the ground and moves east in 1 mm steps; a
    // marker 2 m away must land where the double-precision projection puts
    // it, to well under a pixel, in every frame. In plain float, ECEF
    // positions round to 0.5 m and the marker would jump by tens of pixels.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::CameraPose start{51.4769, -0.0005, 2.0, 0.0, 60.0};
    ts::Camera camera = makeCamera(start);
    QVERIFY(std::abs(camera.eyeGeodetic().heightM - 1.0) < 0.01);
    ts::GlobeMarker marker;
    marker.positionEcef = camera.targetEcef();
    marker.rgba = {255, 0, 0, 255};
    marker.radiusPx = 6.0;
    const std::vector<ts::GlobeMarker> markers = {marker};

    const ts::LocalFrame frame =
        ts::localFrame(start.targetLatDeg, start.targetLonDeg);
    double worstPx = 0.0;
    for (int step = 0; step <= 40; ++step) {
      // Move the camera, not the marker: shift the target east.
      const ts::Geodetic moved = ts::ecefToGeodetic(
          marker.positionEcef + frame.eastEcef * (0.001 * step));
      ts::CameraPose pose = start;
      pose.targetLatDeg = moved.latDeg;
      pose.targetLonDeg = moved.lonDeg;
      camera = makeCamera(pose);
      const std::optional<ts::ScreenPoint> expected =
          camera.project(marker.positionEcef);
      if (!expected) {
        QFAIL("expected: no value");
      }
      const std::optional<QPointF> drawn = redCentroid(render(camera, markers));
      if (!drawn) {
        QFAIL("drawn: no value");
      }
      const double errorPx =
          std::hypot(drawn->x() - expected->xPx, drawn->y() - expected->yPx);
      worstPx = std::max(worstPx, errorPx);
    }
    qInfo("worst marker position error: %.3f px", worstPx);
    QVERIFY(worstPx < 0.25);
  }

  void highDpiScalesMarkers()
  {
    // The same camera at device pixel ratio 2 covers twice the pixels in
    // each direction; markers keep their logical size.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 2.0e7, 0.0, 0.0});
    ts::GlobeMarker marker;
    marker.positionEcef = camera.targetEcef();
    marker.rgba = {255, 0, 0, 255};
    marker.radiusPx = 5.0;
    m_gl->makeCurrent();
    std::unique_ptr<ts::RenderTarget> large =
        m_gl->device().createRenderTarget(2 * imageWidthPx, 2 * imageHeightPx);
    QVERIFY(large);
    m_renderer->setMarkers({marker});
    ts::PassDesc pass;
    pass.target = large.get();
    pass.viewportPx = QRect(0, 0, 2 * imageWidthPx, 2 * imageHeightPx);
    m_renderer->render(camera, pass, 2.0);
    const QImage image = m_gl->device().readPixels(*large);
    int redPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        const QColor c = image.pixelColor(x, y);
        redPixels += c.red() > 200 && c.blue() < 100 ? 1 : 0;
      }
    }
    // A disc of radius 2 × 5 − 1 (inside the rim) device pixels.
    const double expected = 3.14159265358979 * 9.0 * 9.0;
    qInfo("red pixels at 2x: %d (disc %.0f)", redPixels, expected);
    QVERIFY(std::abs(redPixels - expected) < 0.15 * expected);
    const std::optional<QPointF> centre = redCentroid(image);
    if (!centre) {
      QFAIL("centre: no value");
    }
    QVERIFY(std::abs(centre->x() - imageWidthPx) < 0.5);
    QVERIFY(std::abs(centre->y() - imageHeightPx) < 0.5);
  }

  void benchmarkTiledFrame()
  {
    // A tilted view over Europe with every checkerboard tile loaded.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{48.0, 10.0, 2.0e5, 30.0, 70.0});
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QVERIFY(QTest::qWaitFor(
        [&]() {
          render(camera);
          return !m_renderer->isBusy();
        },
        30000));
    QVERIFY(m_renderer->tileStatistics().rendered > 20);
    QBENCHMARK
    {
      render(camera);
    }
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
  }

  void benchmarkMapFrame()
  {
    // The flat map with imagery, on whatever renderer is there.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
    camera.setMorph(1.0);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QVERIFY(!renderSettled(camera).isNull());
    QBENCHMARK
    {
      render(camera);
    }
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
  }

  void benchmarkMorphFrame()
  {
    // Half-way between globe and map.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
    camera.setMorph(0.5);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QVERIFY(!renderSettled(camera).isNull());
    QBENCHMARK
    {
      render(camera);
    }
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
  }

  void benchmarkFrame()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{20.0, 0.0, 2.2e7, 0.0, 0.0});
    QBENCHMARK
    {
      render(camera);
    }
  }
};

QTEST_MAIN(TestGlobe)
#include "tst_globe.moc"
