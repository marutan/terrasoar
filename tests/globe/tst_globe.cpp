// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <QColor>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "goldenimage.h"
#include "offscreengl.h"
#include "ts/camera/camera.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodetic.h"
#include "ts/globe/globerenderer.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/globe/tilerenderer.h"
#include "ts/globe/vectorrenderer.h"
#include "ts/layers/layerdefinition.h"
#include "ts/layers/layertreemodel.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/providerregistry.h"
#include "ts/providers/testproviders.h"

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

  void basePackGolden_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    QTest::newRow("base-uk") << 54.5 << -3.0 << 2.0e6 << 0.0;
    QTest::newRow("base-uk-close") << 51.5 << -0.5 << 1.5e5 << 30.0;
    QTest::newRow("base-pacific-antimeridian")
        << -17.5 << 179.5 << 1.5e6 << 0.0;
    QTest::newRow("base-pacific-wide") << 0.0 << 180.0 << 1.2e7 << 0.0;
    QTest::newRow("base-north-pole") << 90.0 << 0.0 << 7.0e6 << 0.0;
    QTest::newRow("base-north-pole-close") << 82.0 << -40.0 << 1.5e6 << 30.0;
    QTest::newRow("base-south-pole") << -90.0 << 0.0 << 9.0e6 << 0.0;
    QTest::newRow("base-south-pole-close") << -77.5 << 166.0 << 6.0e5 << 30.0;
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
    const QImage image = renderSettled(
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg}));
    const ts::VectorStatistics drawn = m_renderer->vectorStatistics();
    qInfo("vectors: %d tiles, %lld triangles, %lld segments, %d draw calls",
          drawn.tilesDrawn, drawn.triangles, drawn.segments, drawn.drawCalls);
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
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
