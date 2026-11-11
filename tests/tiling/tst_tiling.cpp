// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <QColor>
#include <QStringList>
#include <QtTest>

#include "concurrency.h"
#include "ts/camera/camera.h"
#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"
#include "ts/projection/projection.h"
#include "ts/providers/testproviders.h"
#include "ts/tiling/lrucache.h"
#include "ts/tiling/tilekey.h"
#include "ts/tiling/tileloader.h"
#include "ts/tiling/tilemesh.h"
#include "ts/tiling/tileselector.h"

namespace
{

constexpr double widthPx = 800.0;
constexpr double heightPx = 600.0;

ts::Camera makeCamera(const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(widthPx, heightPx);
  camera.setPose(pose);
  return camera;
}

ts::Vec3 offsetOf(const ts::TileVertex& v)
{
  return ts::Vec3{static_cast<double>(v.offsetM[0]),
                  static_cast<double>(v.offsetM[1]),
                  static_cast<double>(v.offsetM[2])};
}

bool everythingRenderable(ts::TileKey /*key*/)
{
  return true;
}

QString joined(const std::vector<ts::TileKey>& keys)
{
  QStringList list;
  for (const ts::TileKey& key : keys) {
    list << key.toString();
  }
  return list.join(QLatin1Char(' '));
}

std::set<std::string> keyStrings(const std::vector<ts::TileKey>& keys)
{
  std::set<std::string> result;
  for (const ts::TileKey& key : keys) {
    result.insert(key.toString().toStdString());
  }
  return result;
}

/**
 * Checks that every pixel (on a grid) showing the globe is covered by
 * exactly one selected tile. Returns the number of failures.
 */
int coverageFailures(const ts::Camera& camera,
                     const std::vector<ts::TileKey>& tiles)
{
  int failures = 0;
  for (int row = 0; row < 30; ++row) {
    for (int column = 0; column < 40; ++column) {
      const std::optional<ts::Geodetic> hit = camera.pickGeodetic(
          (column + 0.5) * widthPx / 40.0, (row + 0.5) * heightPx / 30.0);
      if (!hit) {
        continue;
      }
      const double lat = hit->latDeg;
      const double lon = hit->lonDeg;
      const int covering = static_cast<int>(std::count_if(
          tiles.begin(), tiles.end(), [lat, lon](ts::TileKey key) {
            return key.bounds().contains(lat, lon);
          }));
      // Points exactly on a shared edge belong to two tiles.
      if (covering < 1 || covering > 4) {
        ++failures;
      }
    }
  }
  return failures;
}

/** A camera on the flat map of @p projection (by identifier). */
ts::Camera makeMapCamera(const char* projection, const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(widthPx, heightPx);
  camera.setMapProjection(ts::projectionById(QLatin1String(projection)));
  camera.setMorph(1.0);
  camera.setPose(pose);
  return camera;
}

bool overlaps(const std::vector<ts::TileKey>& tiles)
{
  for (const ts::TileKey& a : tiles) {
    for (const ts::TileKey& b : tiles) {
      if (a != b && a.contains(b)) {
        return true;
      }
    }
  }
  return false;
}

/** A provider that counts its loads and can be slowed down. */
class CountingProvider : public ts::ImageryProvider
{
public:
  QString id() const override { return QStringLiteral("test:counting"); }
  QString name() const override { return QStringLiteral("counting"); }
  int maximumLevel() const override { return ts::TileKey::maximumLevel; }
  int tileSizePx() const override { return 16; }
  QImage loadImage(ts::TileKey key, const std::atomic<bool>& cancelled) override
  {
    ++loads;
    for (int i = 0; i < delayLoops && !cancelled.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(QColor(key.level * 10, key.x % 256, key.y % 256));
    return image;
  }

  std::atomic<int> loads{0};
  int delayLoops = 0;
};

} // namespace

/** Unit tests for ts/tiling: keys, meshes, selection, cache and loader. */
class TestTiling : public QObject
{
  Q_OBJECT

private slots:
  // --- Keys -----------------------------------------------------------------

  void rootsAndSizes()
  {
    const std::array<ts::TileKey, 2> roots = ts::TileKey::roots();
    QCOMPARE(roots[0].bounds().westDeg, -180.0);
    QCOMPARE(roots[0].bounds().eastDeg, 0.0);
    QCOMPARE(roots[1].bounds().eastDeg, 180.0);
    QCOMPARE(roots[0].bounds().northDeg, 90.0);
    QCOMPARE(roots[0].bounds().southDeg, -90.0);
    QCOMPARE(ts::TileKey::columns(3), std::int64_t{16});
    QCOMPARE(ts::TileKey::rows(3), std::int64_t{8});
    QCOMPARE(ts::TileKey::sizeDeg(3), 22.5);
    QVERIFY((ts::TileKey{24, (1 << 25) - 1, (1 << 24) - 1}.isValid()));
    QVERIFY(!(ts::TileKey{3, 16, 0}.isValid()));
    QVERIFY(!(ts::TileKey{3, 0, 8}.isValid()));
    QVERIFY(!(ts::TileKey{25, 0, 0}.isValid()));
  }

  void containingRoundTrip_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::newRow("Greenwich") << 51.4769 << -0.0005;
    QTest::newRow("Sydney") << -33.8568 << 151.2153;
    QTest::newRow("antimeridian west") << 10.0 << -180.0;
    QTest::newRow("antimeridian east") << 10.0 << 180.0;
    QTest::newRow("beyond 180") << 10.0 << 190.0;
    QTest::newRow("north pole") << 90.0 << 45.0;
    QTest::newRow("south pole") << -90.0 << -45.0;
    QTest::newRow("equator, prime meridian") << 0.0 << 0.0;
  }

  void containingRoundTrip()
  {
    // The tile containing a point contains it, at every level, and its
    // parent is the tile containing it one level up.
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    double lon = ts::normaliseLonDeg(lonDeg);
    if (lonDeg >= 180.0 && lon == -180.0) {
      lon = 180.0;
    }
    for (int level = 0; level <= ts::TileKey::maximumLevel; ++level) {
      const ts::TileKey key =
          ts::TileKey::containing(level, ts::Geodetic{latDeg, lonDeg, 0.0});
      QVERIFY(key.isValid());
      QVERIFY2(key.bounds().contains(latDeg, lon), qPrintable(key.toString()));
      if (level > 0) {
        QCOMPARE(key.parent(),
                 ts::TileKey::containing(level - 1,
                                         ts::Geodetic{latDeg, lonDeg, 0.0}));
      }
    }
  }

  void parentsAndChildren()
  {
    const ts::TileKey key{5, 37, 11};
    for (int i = 0; i < 4; ++i) {
      const ts::TileKey child = key.child(i);
      QCOMPARE(child.parent(), key);
      QVERIFY(key.contains(child));
      QVERIFY(!child.contains(key));
      const ts::GeoRect outer = key.bounds();
      const ts::GeoRect inner = child.bounds();
      QVERIFY(inner.westDeg >= outer.westDeg && inner.eastDeg <= outer.eastDeg);
      QVERIFY(inner.southDeg >= outer.southDeg &&
              inner.northDeg <= outer.northDeg);
    }
    // Child 0 is north-west, 3 south-east.
    QCOMPARE(key.child(0).bounds().northDeg, key.bounds().northDeg);
    QCOMPARE(key.child(0).bounds().westDeg, key.bounds().westDeg);
    QCOMPARE(key.child(3).bounds().southDeg, key.bounds().southDeg);
    QCOMPARE(key.child(3).bounds().eastDeg, key.bounds().eastDeg);
    QCOMPARE(ts::TileKey::roots()[1].parent(), ts::TileKey::roots()[1]);
    QVERIFY(key.contains(key));
    QVERIFY(!key.contains(ts::TileKey{5, 38, 11}));
  }

  void polesAndUniqueness()
  {
    QVERIFY((ts::TileKey{4, 3, 0}.touchesPole()));
    QVERIFY((ts::TileKey{4, 3, 15}.touchesPole()));
    QVERIFY(!(ts::TileKey{4, 3, 7}.touchesPole()));
    std::unordered_set<std::uint64_t> packed;
    for (int level = 0; level < 5; ++level) {
      for (int y = 0; y < ts::TileKey::rows(level); ++y) {
        for (int x = 0; x < ts::TileKey::columns(level); ++x) {
          QVERIFY(packed.insert(ts::TileKey{level, x, y}.packed()).second);
        }
      }
    }
    QCOMPARE((ts::TileKey{12, 345, 67}.toString()),
             QStringLiteral("12/345/67"));
  }

  // --- Meshes
  // -----------------------------------------------------------------

  void meshGeometry_data()
  {
    QTest::addColumn<int>("level");
    QTest::addColumn<int>("x");
    QTest::addColumn<int>("y");
    QTest::newRow("root") << 0 << 1 << 0;
    QTest::newRow("north pole") << 3 << 5 << 0;
    QTest::newRow("antimeridian") << 4 << 31 << 6;
    QTest::newRow("deep") << 18 << 262140 << 50000;
  }

  void meshGeometry()
  {
    QFETCH(int, level);
    QFETCH(int, x);
    QFETCH(int, y);
    const ts::TileKey key{level, x, y};
    const int n = 8;
    const ts::TileMesh mesh = ts::buildTileMesh(key, n);
    QCOMPARE(static_cast<int>(mesh.vertices.size()), (n + 1) * (n + 1) + 4 * n);
    const ts::GeoRect rect = key.bounds();
    // Grid vertices lie on the ellipsoid (to float precision of the
    // offset) at their latitude and longitude.
    double worstM = 0.0;
    for (int row = 0; row <= n; ++row) {
      for (int column = 0; column <= n; ++column) {
        const ts::TileVertex& v = mesh.vertices.at(
            static_cast<std::size_t>(row) * static_cast<std::size_t>(n + 1) +
            static_cast<std::size_t>(column));
        const ts::Vec3 p = mesh.centreEcef + offsetOf(v);
        const ts::Vec3 expected = ts::geodeticToEcef(
            ts::Geodetic{rect.northDeg - rect.heightDeg() * row / n,
                         rect.westDeg + rect.widthDeg() * column / n, 0.0});
        worstM = std::max(worstM, ts::distance(p, expected));
        QCOMPARE(v.uv[0], static_cast<float>(column) / n);
        QCOMPARE(v.uv[1], static_cast<float>(row) / n);
        // Float offsets round by about 1e-7 of the tile size.
        QVERIFY(ts::length(p - mesh.centreEcef) <=
                mesh.radiusM * (1.0 + 1e-6) + 1e-3);
      }
    }
    // Float offsets: relative precision about 1e-7 of the tile size.
    QVERIFY2(worstM < 1e-6 * mesh.radiusM + 1e-3,
             qPrintable(QString::number(worstM)));
    // Skirt vertices are below the surface by the skirt depth.
    const double depthM = ts::tileSkirtDepthM(level, n);
    const int gridVertices = (n + 1) * (n + 1);
    for (std::size_t i = static_cast<std::size_t>(gridVertices);
         i < mesh.vertices.size(); ++i) {
      const ts::TileVertex& v = mesh.vertices[i];
      const ts::Vec3 p = mesh.centreEcef + offsetOf(v);
      QVERIFY(std::abs(ts::ecefToGeodetic(p).heightM + depthM) <
              1e-6 * mesh.radiusM + 1e-2);
    }
  }

  void meshIndicesFaceOutwards()
  {
    // Every grid triangle's normal points away from the globe's centre.
    const ts::TileKey key{3, 6, 2};
    const int n = 8;
    const ts::TileMesh mesh = ts::buildTileMesh(key, n);
    const std::vector<std::uint16_t> indices = ts::tileMeshIndices(n);
    QCOMPARE(static_cast<int>(indices.size()), 6 * n * n + 24 * n);
    const int gridTriangleCount = 2 * n * n;
    const std::size_t gridTriangles =
        static_cast<std::size_t>(gridTriangleCount);
    for (std::size_t t = 0; t < indices.size() / 3; ++t) {
      std::array<ts::Vec3, 3> p;
      for (std::size_t k = 0; k < 3; ++k) {
        const std::uint16_t index = indices[3 * t + k];
        QVERIFY(index < mesh.vertices.size());
        const ts::TileVertex& v = mesh.vertices[index];
        p.at(k) = mesh.centreEcef + offsetOf(v);
      }
      const ts::Vec3 normal = ts::cross(p[1] - p[0], p[2] - p[0]);
      if (t < gridTriangles) {
        QVERIFY(ts::dot(normal, p[0]) > 0.0);
      } else {
        // Skirt triangles face away from the tile's middle.
        const ts::Vec3 middle = (p[0] + p[1] + p[2]) / 3.0;
        QVERIFY(ts::dot(normal, middle - mesh.centreEcef) > 0.0);
      }
    }
  }

  // --- Selection
  // --------------------------------------------------------------

  void selectionCoversViewWithoutOverlap_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    QTest::newRow("whole globe") << 20.0 << 10.0 << 2.0e7 << 0.0;
    QTest::newRow("Europe tilted") << 48.0 << 10.0 << 2.0e6 << 50.0;
    QTest::newRow("north pole") << 89.0 << 0.0 << 5.0e6 << 0.0;
    QTest::newRow("antimeridian") << -15.0 << 180.0 << 3.0e6 << 30.0;
    QTest::newRow("city") << 51.5 << -0.1 << 5.0e3 << 60.0;
  }

  void selectionCoversViewWithoutOverlap()
  {
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 30.0, tiltDeg});
    const ts::TileSelector selector;
    const ts::TileSelection selection =
        selector.select(camera, everythingRenderable);
    qInfo("%d tiles drawn, %d visited",
          static_cast<int>(selection.render.size()), selection.visited);
    QVERIFY(!selection.render.empty());
    QVERIFY(selection.load.empty());
    QVERIFY(!overlaps(selection.render));
    QCOMPARE(coverageFailures(camera, selection.render), 0);
    // Detail is where it is needed: no drawn tile has a texel bigger than
    // the limit on screen, unless it is at the deepest level.
    for (const ts::TileKey& key : selection.render) {
      QVERIFY(selector.screenSpaceErrorPx(camera, key) <=
                  selector.settings().maximumScreenSpaceErrorPx ||
              key.level == selector.settings().maximumLevel);
    }
  }

  void expectedTileSets()
  {
    // Fixed cameras select these tile sets (regression).
    const ts::TileSelector selector;
    const ts::TileSelection globe =
        selector.select(makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0}),
                        everythingRenderable);
    // Level 2 in the middle, level 1 towards the limb, where tiles are
    // farther away and seen edge-on.
    const std::set<std::string> expectedGlobe = {
        "1/0/0", "1/0/1", "1/1/1", "1/3/0", "1/3/1", "2/2/0",
        "2/3/0", "2/2/1", "2/3/1", "2/4/0", "2/5/0", "2/4/1",
        "2/5/1", "2/4/2", "2/5/2", "2/4/3", "2/5/3"};
    qInfo("whole globe: %s", qPrintable(joined(globe.render)));
    QCOMPARE(keyStrings(globe.render), expectedGlobe);

    // Straight down at 1000 km over (0, 0): the four tiles meeting there,
    // at one level, and nothing behind the horizon.
    const ts::TileSelection close =
        selector.select(makeCamera(ts::CameraPose{0.0, 0.0, 1.0e6, 0.0, 0.0}),
                        everythingRenderable);
    for (const ts::TileKey& key : close.render) {
      const ts::GeoRect r = key.bounds();
      QVERIFY2(r.eastDeg > -40.0 && r.westDeg < 40.0 && r.northDeg > -40.0 &&
                   r.southDeg < 40.0,
               qPrintable(key.toString()));
    }
    const std::set<std::string> nearOrigin = {"6/63/31", "6/64/31", "6/63/32",
                                              "6/64/32"};
    for (const std::string& key : nearOrigin) {
      QVERIFY2(keyStrings(close.render).count(key) == 1, key.c_str());
    }
  }

  void culling()
  {
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 1.0e6, 0.0, 0.0});
    // The antipode is behind the horizon; a tile on the visible side but
    // off screen is outside the frustum.
    const ts::TileKey antipode =
        ts::TileKey::containing(4, ts::Geodetic{0.0, 180.0, 0.0});
    QVERIFY(ts::TileSelector::isBehindHorizon(camera, antipode));
    QVERIFY(!ts::TileSelector::isVisible(camera, antipode));
    const ts::TileKey side =
        ts::TileKey::containing(6, ts::Geodetic{0.0, 25.0, 0.0});
    QVERIFY(!ts::TileSelector::isBehindHorizon(camera, side));
    QVERIFY(!ts::TileSelector::isInFrustum(camera, side));
    const ts::TileKey below =
        ts::TileKey::containing(6, ts::Geodetic{0.0, 0.0, 0.0});
    QVERIFY(ts::TileSelector::isVisible(camera, below));
  }

  // --- Selection on the flat map
  // ----------------------------------------------

  void mapSelectionCoversViewWithoutOverlap_data()
  {
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    QTest::newRow("Mercator world") << mercator << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("Miller world") << miller << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("Mercator Europe") << mercator << 48.0 << 10.0 << 2.0e6;
    QTest::newRow("Mercator antimeridian")
        << mercator << -15.0 << 180.0 << 3.0e6;
    QTest::newRow("Mercator top edge") << mercator << 84.0 << 30.0 << 1.0e6;
    QTest::newRow("Miller pole") << miller << 89.0 << -120.0 << 1.0e6;
    QTest::newRow("Web Mercator city")
        << QStringLiteral("web-mercator") << 51.5 << -0.1 << 5.0e3;
  }

  void mapSelectionCoversViewWithoutOverlap()
  {
    // On the flat map every point in view is covered by one drawn tile, no
    // texel is larger than the limit, and nothing out of view is drawn.
    QFETCH(QString, projection);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    const ts::Camera camera =
        makeMapCamera(projection.toLatin1().constData(),
                      ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    const ts::TileSelector selector;
    const ts::TileSelection selection =
        selector.select(camera, everythingRenderable);
    qInfo("%d tiles drawn, %d visited",
          static_cast<int>(selection.render.size()), selection.visited);
    QVERIFY(!selection.render.empty());
    QVERIFY(selection.load.empty());
    QVERIFY(!overlaps(selection.render));
    QCOMPARE(coverageFailures(camera, selection.render), 0);
    for (const ts::TileKey& key : selection.render) {
      QVERIFY(selector.screenSpaceErrorPx(camera, key) <=
                  selector.settings().maximumScreenSpaceErrorPx ||
              key.level == selector.settings().maximumLevel);
      QVERIFY(ts::TileSelector::isVisibleOnMap(camera, key));
      QVERIFY(ts::TileSelector::isVisible(camera, key));
    }
    // Far fewer tiles than the whole world at that level.
    QVERIFY(selection.render.size() < 400);
  }

  void mapCulling()
  {
    // 1000 km above (0°, 0°) on the Mercator map: the view is about
    // ±5° by ±3.7°.
    const ts::Camera camera =
        makeMapCamera("mercator", ts::CameraPose{0.0, 0.0, 1.0e6, 0.0, 0.0});
    const std::function<ts::TileKey(int, double, double)> tileAt =
        [](int level, double lat, double lon) {
          return ts::TileKey::containing(level, ts::Geodetic{lat, lon, 0.0});
        };
    QVERIFY(ts::TileSelector::isVisibleOnMap(camera, tileAt(6, 1.0, 1.0)));
    QVERIFY(!ts::TileSelector::isVisibleOnMap(camera, tileAt(6, 1.0, 25.0)));
    QVERIFY(!ts::TileSelector::isVisibleOnMap(camera, tileAt(6, 20.0, 1.0)));
    // There is no horizon on a map: the antipode is merely out of view.
    QVERIFY(!ts::TileSelector::isVisibleOnMap(camera, tileAt(4, 0.0, 180.0)));
    // The same answers from isVisible(), which follows the camera's view.
    QVERIFY(ts::TileSelector::isVisible(camera, tileAt(6, 1.0, 1.0)));
    QVERIFY(!ts::TileSelector::isVisible(camera, tileAt(6, 1.0, 25.0)));
    // isVisibleOnMap() answers for the map whatever the camera shows.
    ts::Camera globe = makeCamera(ts::CameraPose{0.0, 0.0, 1.0e6, 0.0, 0.0});
    QVERIFY(ts::TileSelector::isVisibleOnMap(globe, tileAt(6, 1.0, 1.0)));
    QVERIFY(!ts::TileSelector::isVisibleOnMap(globe, tileAt(6, 1.0, 25.0)));

    // Across the edge where the map wraps: centred on 179° E, tiles just
    // east of the antimeridian are in view, through the next copy.
    const ts::Camera pacific =
        makeMapCamera("mercator", ts::CameraPose{0.0, 179.0, 1.0e6, 0.0, 0.0});
    QVERIFY(ts::TileSelector::isVisibleOnMap(pacific, tileAt(6, 1.0, -179.0)));
    QVERIFY(ts::TileSelector::isVisibleOnMap(pacific, tileAt(6, 1.0, 178.0)));
    QVERIFY(!ts::TileSelector::isVisibleOnMap(pacific, tileAt(6, 1.0, -150.0)));

    // Beyond the projection's latitude limit nothing is drawn: Mercator
    // stops near 85°, Miller goes to the pole.
    const ts::TileKey polar = tileAt(6, 88.0, 0.0);
    QVERIFY(polar.bounds().southDeg > 85.1);
    const ts::CameraPose north{84.5, 0.0, 2.0e6, 0.0, 0.0};
    QVERIFY(!ts::TileSelector::isVisibleOnMap(makeMapCamera("mercator", north),
                                              polar));
    QVERIFY(ts::TileSelector::isVisibleOnMap(makeMapCamera("miller", north),
                                             polar));
  }

  void mapErrorFollowsTheProjection()
  {
    // On the map a texel's size depends on the latitude, not on how far
    // the tile is from the centre: Mercator stretches towards the poles.
    const ts::TileSelector selector;
    const ts::Camera camera =
        makeMapCamera("mercator", ts::CameraPose{0.0, 0.0, 2.0e7, 0.0, 0.0});
    const std::function<double(double, double)> errorAt = [&](double lat,
                                                              double lon) {
      return selector.screenSpaceErrorPx(
          camera, ts::TileKey::containing(5, ts::Geodetic{lat, lon, 0.0}));
    };
    QVERIFY(std::abs(errorAt(1.0, 1.0) / errorAt(1.0, 120.0) - 1.0) < 1e-9);
    QVERIFY(errorAt(61.0, 1.0) > 1.9 * errorAt(1.0, 1.0));
    QVERIFY(errorAt(80.0, 1.0) > 4.0 * errorAt(1.0, 1.0));
    // Half the range, twice the size.
    const ts::Camera closer =
        makeMapCamera("mercator", ts::CameraPose{0.0, 0.0, 1.0e7, 0.0, 0.0});
    const ts::TileKey key =
        ts::TileKey::containing(5, ts::Geodetic{1.0, 1.0, 0.0});
    QVERIFY(std::abs(selector.screenSpaceErrorPx(closer, key) /
                         selector.screenSpaceErrorPx(camera, key) -
                     2.0) < 1e-9);
    // A level 5 tile is 5.625° of 256 texels; at the equator a degree is
    // 111.3 km and a pixel of this view 2 × 2e7 × tan(22.5°) / 600 m.
    const double expected = 5.625 * 111319.49 / 256.0 /
                            (2.0 * 2.0e7 * std::tan(ts::pi / 8.0) / 600.0);
    QVERIFY(std::abs(errorAt(1.0, 1.0) / expected - 1.0) < 0.01);
  }

  void mapMinimumLevel()
  {
    // The map's geometry is never coarser than minimumMapLevel, even where
    // the data ends above it; the globe is unaffected.
    ts::TileSelectionSettings settings;
    settings.minimumMapLevel = 3;
    settings.maximumLevel = 1;
    const ts::TileSelector selector(settings);
    const ts::CameraPose pose{0.0, 0.0, 5.0e7, 0.0, 0.0};
    const ts::TileSelection map =
        selector.select(makeMapCamera("miller", pose), everythingRenderable);
    QCOMPARE(static_cast<int>(map.render.size()), 128);
    for (const ts::TileKey& key : map.render) {
      QCOMPARE(key.level, 3);
    }
    const ts::TileSelection globe =
        selector.select(makeCamera(pose), everythingRenderable);
    for (const ts::TileKey& key : globe.render) {
      QVERIFY(key.level <= 1);
    }
    // Until the finer meshes exist the coarse ones are drawn and the
    // finer requested.
    const ts::TileSelection waiting =
        selector.select(makeMapCamera("miller", pose),
                        [](ts::TileKey key) { return key.level == 0; });
    QCOMPARE(static_cast<int>(waiting.render.size()), 2);
    QCOMPARE(static_cast<int>(waiting.load.size()), 8);
  }

  void mapGeometryError()
  {
    // A mesh is straight between its grid rows; Mercator is not. The
    // error grows towards the map's edge and with the size of the tile,
    // and such tiles are split on the finished map.
    const ts::TileSelector selector;
    const ts::CameraPose world{0.0, 0.0, 5.0e7, 0.0, 0.0};
    const ts::Camera mercator = makeMapCamera("mercator", world);
    const std::function<double(const ts::Camera&, int, double)> errorAt =
        [&](const ts::Camera& camera, int level, double lat) {
          return selector.mapGeometryErrorPx(
              camera,
              ts::TileKey::containing(level, ts::Geodetic{lat, 5.0, 0.0}));
        };
    QVERIFY(errorAt(mercator, 2, 10.0) < 0.1);
    QVERIFY(errorAt(mercator, 2, 80.0) > 10.0 * errorAt(mercator, 2, 10.0));
    QVERIFY(errorAt(mercator, 2, 80.0) > 0.5);
    QVERIFY(errorAt(mercator, 2, -80.0) > 0.5);
    QVERIFY(errorAt(mercator, 5, 80.0) < errorAt(mercator, 2, 80.0) / 30.0);
    // Miller curves far less.
    const ts::Camera miller = makeMapCamera("miller", world);
    QVERIFY(errorAt(miller, 2, 80.0) < 0.2 * errorAt(mercator, 2, 80.0));
    // Entirely beyond the map's edge there is nothing to be wrong.
    QCOMPARE(errorAt(mercator, 6, 88.0), 0.0);
    // The value itself: the middle of the top grid row of 2/x/0, between
    // its straight ends, in pixels of this view.
    const ts::Projection& projection = *mercator.mapProjection();
    const double top = projection.maximumLatDeg();
    const double below = top - 45.0 / 16.0;
    const double expectedM =
        std::abs(projection.forward(0.5 * (top + below), 0.0).yM -
                 0.5 * (projection.forward(top, 0.0).yM +
                        projection.forward(below, 0.0).yM));
    const double pixelM = 2.0 * mercator.mapFrame().halfHeightM / heightPx;
    QVERIFY(std::abs(errorAt(mercator, 2, 80.0) / (expectedM / pixelM) - 1.0) <
            1e-9);

    // The selection keeps every drawn tile within the limit: finer tiles
    // towards the top and bottom of the Mercator map than in the middle.
    ts::TileSelectionSettings settings;
    settings.maximumLevel = 1; // the data ends here; the geometry doesn't
    const ts::TileSelector coarse(settings);
    const ts::TileSelection selection =
        coarse.select(mercator, everythingRenderable);
    int deepest = 0;
    for (const ts::TileKey& key : selection.render) {
      QVERIFY(coarse.mapGeometryErrorPx(mercator, key) <=
              settings.maximumMapGeometryErrorPx);
      deepest = std::max(deepest, key.level);
      if (std::abs(key.bounds().centreLatDeg()) < 40.0) {
        // The level 1 tiles reach the map's edge; their equatorial
        // halves need no more.
        QCOMPARE(key.level, 2);
      }
    }
    QVERIFY(deepest >= 3);
    QVERIFY(!overlaps(selection.render));
    // Not while the globe is still unrolling: the change takes a second,
    // and the extra tiles would cost more than they show.
    ts::Camera morphing = mercator;
    morphing.setMorph(0.9);
    for (const ts::TileKey& key :
         coarse.select(morphing, everythingRenderable).render) {
      QVERIFY(key.level <= 1);
    }
  }

  void stripIndices()
  {
    // One column of quads per mesh: each grid row from its west end to
    // its east end, facing the same way as the full grid.
    const int n = 16;
    const std::vector<std::uint16_t> strip = ts::tileMeshStripIndices(n);
    QCOMPARE(static_cast<int>(strip.size()), 6 * n);
    const ts::TileMesh mesh = ts::buildTileMesh(ts::TileKey{3, 9, 2}, n);
    for (std::size_t i = 0; i + 2 < strip.size(); i += 3) {
      std::array<ts::Vec3, 3> corners;
      for (std::size_t k = 0; k < 3; ++k) {
        const int index = strip[i + k];
        QVERIFY(index < (n + 1) * (n + 1)); // no skirt
        const int column = index % (n + 1);
        QVERIFY(column == 0 || column == n);
        corners.at(k) =
            mesh.centreEcef +
            offsetOf(mesh.vertices.at(static_cast<std::size_t>(index)));
      }
      // Counter-clockwise seen from outside, like tileMeshIndices().
      const ts::Vec3 normal =
          ts::cross(corners[1] - corners[0], corners[2] - corners[0]);
      QVERIFY(ts::dot(normal, corners[0]) > 0.0);
    }
    QCOMPARE(static_cast<int>(ts::tileMeshStripIndices(1).size()), 12);
  }

  void morphSelectionShowsBothViews()
  {
    // Part of the way between globe and map a tile is kept if either
    // view shows it, and the far side of the globe is not culled: it
    // unrolls into view.
    ts::Camera camera = makeCamera(ts::CameraPose{0.0, 0.0, 2.0e7, 0.0, 0.0});
    const ts::TileKey antipode =
        ts::TileKey::containing(3, ts::Geodetic{10.0, 175.0, 0.0});
    QVERIFY(!ts::TileSelector::isVisible(camera, antipode));
    camera.setMorph(0.5);
    QVERIFY(ts::TileSelector::isVisible(camera, antipode));
    const ts::TileSelector selector;
    const ts::TileSelection selection =
        selector.select(camera, everythingRenderable);
    QVERIFY(!overlaps(selection.render));
    // The whole world is there.
    double areaDeg2 = 0.0;
    for (const ts::TileKey& key : selection.render) {
      areaDeg2 += key.bounds().widthDeg() * key.bounds().heightDeg();
    }
    QCOMPARE(areaDeg2, 360.0 * 180.0);
    // The error is the larger of the two views'.
    const ts::TileKey near =
        ts::TileKey::containing(4, ts::Geodetic{60.0, 1.0, 0.0});
    const double morphing = selector.screenSpaceErrorPx(camera, near);
    camera.setMorph(0.0);
    const double onGlobe = selector.screenSpaceErrorPx(camera, near);
    camera.setMorph(1.0);
    QVERIFY(morphing >= onGlobe);
    QVERIFY(morphing >= selector.screenSpaceErrorPx(camera, near) * 0.99);
  }

  void refinementWaitsForChildren()
  {
    // Only levels up to 3 are on the GPU: deeper tiles are requested,
    // coarsest first, and level 3 is drawn meanwhile, with no holes.
    const ts::Camera camera =
        makeCamera(ts::CameraPose{45.0, 7.0, 2.0e5, 0.0, 30.0});
    const ts::TileSelector selector;
    const ts::TileSelection selection =
        selector.select(camera, [](ts::TileKey key) { return key.level <= 3; });
    QVERIFY(!selection.render.empty());
    for (const ts::TileKey& key : selection.render) {
      QVERIFY(key.level <= 3);
    }
    QCOMPARE(coverageFailures(camera, selection.render), 0);
    QVERIFY(!selection.load.empty());
    for (const ts::TileLoadRequest& request : selection.load) {
      QCOMPARE(request.key.level, 4);
    }
    // Nothing loaded yet: the roots are requested.
    const ts::TileSelection empty =
        selector.select(camera, [](ts::TileKey /*key*/) { return false; });
    QVERIFY(empty.render.empty());
    QCOMPARE(static_cast<int>(empty.load.size()), 2);
    QCOMPARE(empty.load[0].key.level, 0);
  }

  void errorShrinksWithDistance()
  {
    const ts::TileSelector selector;
    const ts::TileKey key =
        ts::TileKey::containing(8, ts::Geodetic{45.0, 7.0, 0.0});
    double previous = 1e300;
    for (const double rangeM : {1.0e4, 1.0e5, 1.0e6, 1.0e7}) {
      const double error = selector.screenSpaceErrorPx(
          makeCamera(ts::CameraPose{45.0, 7.0, rangeM, 0.0, 0.0}), key);
      QVERIFY(error < previous);
      previous = error;
    }
  }

  // --- Cache
  // ------------------------------------------------------------------

  void lruCache()
  {
    ts::LruCache<int, std::string> cache(10);
    cache.insert(1, "one", 4);
    cache.insert(2, "two", 4);
    QCOMPARE(cache.size(), std::size_t{2});
    QVERIFY(cache.find(1).has_value()); // 1 is now most recent
    cache.insert(3, "three", 4);        // over budget: 2 goes
    QVERIFY(!cache.find(2).has_value());
    QCOMPARE(cache.find(1).value_or(std::string()), std::string("one"));
    QCOMPARE(cache.find(3).value_or(std::string()), std::string("three"));
    QCOMPARE(cache.totalCost(), std::size_t{8});
    cache.insert(1, "uno", 2); // replace changes the cost
    QCOMPARE(cache.totalCost(), std::size_t{6});
    cache.insert(9, "huge", 11); // bigger than the budget: not kept
    QVERIFY(!cache.find(9).has_value());
    cache.setMaximumCost(3);
    QCOMPARE(cache.size(), std::size_t{1});
    cache.remove(1);
    cache.clear();
    QCOMPARE(cache.size(), std::size_t{0});
    QVERIFY(cache.hits() > 0 && cache.misses() > 0);
  }

  void lruCacheShared()
  {
    // Thread-safe: many threads insert and look up in one cache.
    ts::LruCache<int, int> cache(500);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&cache](int thread) {
          for (int i = 0; i < 2000; ++i) {
            const int key = (thread * 7919 + i) % 1000;
            cache.insert(key, key * 2, 1);
            const std::optional<int> value = cache.find((key + 13) % 1000);
            if (value && *value != ((key + 13) % 1000) * 2) {
              return false;
            }
          }
          return cache.totalCost() <= 500;
        });
    QCOMPARE(failures, 0);
  }

  // --- Loader
  // -----------------------------------------------------------------

  void loaderLoadsRequestedTiles()
  {
    const std::shared_ptr<CountingProvider> provider =
        std::make_shared<CountingProvider>();
    ts::TileLoader loader(provider);
    std::atomic<int> ready{0};
    loader.setTilesReadyCallback([&ready]() { ++ready; });
    std::vector<ts::TileLoadRequest> requests;
    requests.reserve(8);
    for (int x = 0; x < 8; ++x) {
      requests.push_back(ts::TileLoadRequest{ts::TileKey{2, x, 1}, 0.0});
    }
    loader.setRequests(requests);
    std::vector<ts::LoadedTile> loaded;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          std::vector<ts::LoadedTile> more = loader.takeLoaded(3);
          loaded.insert(loaded.end(), more.begin(), more.end());
          return loaded.size() == requests.size();
        },
        10000));
    QCOMPARE(ready.load(), 8);
    for (const ts::LoadedTile& tile : loaded) {
      QVERIFY(!tile.image.isNull());
      QVERIFY(!tile.mesh.vertices.empty());
      QCOMPARE(tile.mesh.key, tile.key);
    }
    QCOMPARE(loader.outstanding(), 0);
    // Asking again comes from the cache, not the provider.
    loader.setRequests(requests);
    std::size_t again = 0;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          again += loader.takeLoaded(8).size();
          return again == requests.size();
        },
        10000));
    QCOMPARE(provider->loads.load(), 8);
  }

  void loaderCancelsUnwantedTiles()
  {
    const std::shared_ptr<CountingProvider> provider =
        std::make_shared<CountingProvider>();
    provider->delayLoops = 200; // 20 ms per tile unless cancelled
    ts::TileLoaderSettings settings;
    settings.threads = 2;
    ts::TileLoader loader(provider, settings);
    std::vector<ts::TileLoadRequest> requests;
    requests.reserve(40);
    for (int x = 0; x < 40; ++x) {
      requests.push_back(ts::TileLoadRequest{ts::TileKey{5, x, 3}, 0.0});
    }
    loader.setRequests(requests);
    QTest::qWait(30);
    // The camera moved on: none of those are wanted any more.
    loader.setRequests({});
    QVERIFY(QTest::qWaitFor(
        [&]() {
          loader.takeLoaded(100);
          return loader.outstanding() == 0;
        },
        10000));
    QVERIFY2(provider->loads.load() < 10,
             qPrintable(QString::number(provider->loads.load())));
  }

  void loaderStressWhileCameraMoves()
  {
    // Thread safety: thousands of requests, cancellations and loads while
    // the camera flies around (run under ThreadSanitizer in CI); then the
    // final view must load completely. No assertion depends on how fast
    // the workers are.
    const std::shared_ptr<ts::CheckerboardImageryProvider> provider =
        std::make_shared<ts::CheckerboardImageryProvider>(32);
    ts::TileLoaderSettings settings;
    settings.threads = 4;
    settings.cacheBytes = std::size_t{2} * 1024 * 1024;
    settings.meshSegments = 4;
    ts::TileLoader loader(provider, settings);
    std::atomic<int> callbacks{0};
    loader.setTilesReadyCallback([&callbacks]() { ++callbacks; });
    const ts::TileSelector selector;
    std::unordered_set<ts::TileKey, ts::TileKeyHash> resident;
    std::unordered_set<ts::TileKey, ts::TileKeyHash> requested;
    int taken = 0;
    bool unrequestedTile = false;
    ts::Camera camera;
    const std::function<void()> step = [&]() {
      for (const ts::LoadedTile& tile : loader.takeLoaded(16)) {
        unrequestedTile = unrequestedTile || requested.count(tile.key) == 0;
        resident.insert(tile.key);
        ++taken;
      }
    };
    for (int frame = 0; frame < 400; ++frame) {
      const double t = frame / 400.0;
      camera = makeCamera(ts::CameraPose{
          60.0 * std::sin(6.0 * t), 360.0 * t - 180.0,
          2.0e7 * std::pow(1.0e-3, std::sin(3.14159 * t)), 0.0, 40.0 * t});
      step();
      const ts::TileSelection selection =
          selector.select(camera, [&resident](ts::TileKey key) {
            return resident.count(key) != 0;
          });
      for (const ts::TileLoadRequest& request : selection.load) {
        requested.insert(request.key);
      }
      loader.setRequests(selection.load);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Stay at the last camera until everything it wants is loaded.
    QVERIFY(QTest::qWaitFor(
        [&]() {
          step();
          const ts::TileSelection selection =
              selector.select(camera, [&resident](ts::TileKey key) {
                return resident.count(key) != 0;
              });
          for (const ts::TileLoadRequest& request : selection.load) {
            requested.insert(request.key);
          }
          loader.setRequests(selection.load);
          return selection.load.empty();
        },
        30000));
    QVERIFY(QTest::qWaitFor(
        [&]() { return callbacks.load() == loader.loadedCount(); }, 5000));
    qInfo("%d tiles taken, %d loaded, %d cancelled, %d callbacks", taken,
          loader.loadedCount(), loader.cancelledCount(), callbacks.load());
    QVERIFY(taken > 0);
    QVERIFY(!unrequestedTile);
    QVERIFY(loader.cache().totalCost() <= settings.cacheBytes);
  }

  void concurrentOwnSelectors()
  {
    // Reentrant: selectors on many threads give the single-threaded result.
    const ts::Camera camera =
        makeCamera(ts::CameraPose{48.0, 10.0, 2.0e6, 30.0, 50.0});
    const std::set<std::string> expected = keyStrings(
        ts::TileSelector().select(camera, everythingRenderable).render);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&camera, &expected](int) {
          const ts::TileSelector selector;
          return keyStrings(
                     selector.select(camera, everythingRenderable).render) ==
                 expected;
        });
    QCOMPARE(failures, 0);
  }

  // --- Benchmarks
  // ---------------------------------------------------------------

  void benchmarkSelection()
  {
    const ts::Camera camera =
        makeCamera(ts::CameraPose{48.0, 10.0, 2.0e4, 30.0, 70.0});
    const ts::TileSelector selector;
    QBENCHMARK
    {
      selector.select(camera, everythingRenderable);
    }
  }

  void benchmarkMapSelection()
  {
    const ts::Camera camera =
        makeMapCamera("mercator", ts::CameraPose{48.0, 10.0, 2.0e5, 0.0, 0.0});
    const ts::TileSelector selector;
    std::size_t count = 0;
    QBENCHMARK
    {
      count += selector.select(camera, everythingRenderable).render.size();
    }
    QVERIFY(count > 0);
  }

  void benchmarkMesh()
  {
    QBENCHMARK
    {
      ts::buildTileMesh(ts::TileKey{12, 2100, 700});
    }
  }
};

QTEST_GUILESS_MAIN(TestTiling)
#include "tst_tiling.moc"
