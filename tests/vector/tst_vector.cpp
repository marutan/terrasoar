// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include <QtTest>

#include "concurrency.h"
#include "ts/geo/geodetic.h"
#include "ts/providers/tileprovider.h"
#include "ts/vector/vectormesh.h"
#include "ts/vector/vectortileloader.h"

namespace
{

const std::atomic<bool> notCancelled{false};

std::vector<ts::GeoPoint> ring(const std::vector<double>& lonLat)
{
  std::vector<ts::GeoPoint> points;
  for (std::size_t i = 0; i + 1 < lonLat.size(); i += 2) {
    points.push_back(ts::GeoPoint{lonLat[i + 1], lonLat[i]});
  }
  points.push_back(points.front());
  return points;
}

ts::Feature polygon(const std::vector<std::vector<double>>& rings)
{
  ts::Feature feature;
  feature.type = ts::Feature::Type::Polygon;
  for (const std::vector<double>& r : rings) {
    feature.parts.push_back(ring(r));
  }
  return feature;
}

ts::Feature lineString(const std::vector<double>& lonLat)
{
  ts::Feature feature;
  feature.type = ts::Feature::Type::LineStrings;
  std::vector<ts::GeoPoint> points;
  for (std::size_t i = 0; i + 1 < lonLat.size(); i += 2) {
    points.push_back(ts::GeoPoint{lonLat[i + 1], lonLat[i]});
  }
  feature.parts.push_back(points);
  return feature;
}

/** Longitude and latitude of @p vertex of @p mesh, degrees. */
std::array<float, 2> lonLatDeg(const ts::VectorTileMesh& mesh,
                               const ts::VectorVertex& vertex)
{
  return {static_cast<float>(mesh.lonDeg(vertex)),
          static_cast<float>(mesh.latDeg(vertex))};
}

/** Area of the triangles in longitude/latitude, square degrees. */
double planarArea(const std::vector<ts::VectorVertex>& triangles)
{
  double area = 0.0;
  for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
    const std::array<float, 2>& a = triangles[i].lonLatOffsetDeg;
    const std::array<float, 2>& b = triangles[i + 1].lonLatOffsetDeg;
    const std::array<float, 2>& c = triangles[i + 2].lonLatOffsetDeg;
    area += 0.5 * std::abs(static_cast<double>((b[0] - a[0]) * (c[1] - a[1]) -
                                               (c[0] - a[0]) * (b[1] - a[1])));
  }
  return area;
}

/** A provider serving fixed features at storage level 1 for every level. */
class FixtureProvider : public ts::FeatureProvider
{
public:
  QString id() const override { return QStringLiteral("fixture"); }
  QString name() const override { return QStringLiteral("Fixture"); }
  int maximumLevel() const override { return 1; }
  ts::TileKey storedKey(ts::TileKey key) const override
  {
    while (key.level > 1) {
      key = key.parent();
    }
    return key;
  }
  std::optional<ts::FeatureTile>
  loadFeatures(ts::TileKey key, const std::atomic<bool>& cancelled) override
  {
    loads.fetch_add(1);
    if (cancelled.load()) {
      return std::nullopt;
    }
    const ts::TileKey stored = storedKey(key);
    ts::FeatureTile tile{stored, {}};
    if (stored == ts::TileKey{1, 2, 0}) {
      // 0°–90°E, 0°–90°N: one square.
      tile.features.push_back(polygon({{10, 10, 30, 10, 30, 30, 10, 30}}));
    }
    return tile;
  }
  std::atomic<int> loads{0};
};

} // namespace

/** Unit tests for ts/vector: styles, meshing, the tile loader. */
class TestVector : public QObject
{
  Q_OBJECT

private slots:
  void cellSizes()
  {
    const ts::VectorMesher mesher;
    // Sixteen cells per tile side, at most 5.625°.
    QCOMPARE(mesher.cellSizeDeg(0), 5.625);
    QCOMPARE(mesher.cellSizeDeg(1), 5.625);
    QCOMPARE(mesher.cellSizeDeg(2), 45.0 / 16.0);
    QCOMPARE(mesher.cellSizeDeg(6), 180.0 / 64.0 / 16.0);
  }

  void fillCoversThePolygon()
  {
    // A square with a lake in tile (2, 4, 1) (0°–45°E, 0°–45°N): the
    // triangles cover it without the lake, each within one cell, and
    // their vertices sit on the ellipsoid.
    ts::VectorMesher mesher;
    const ts::TileKey key{2, 4, 1};
    ts::FeatureTile tile{key, {}};
    tile.features.push_back(polygon(
        {{5, 5, 40, 5, 40, 40, 5, 40}, {20, 20, 25, 20, 25, 25, 20, 25}}));
    const std::optional<ts::VectorTileMesh> mesh =
        mesher.build(tile, notCancelled);
    if (!mesh) {
      QFAIL("no mesh");
    }
    QVERIFY(mesh->key == key);
    QVERIFY(!mesh->triangles.empty());
    QCOMPARE(mesh->triangles.size() % 3, std::size_t{0});
    QVERIFY(std::abs(planarArea(mesh->triangles) - (35.0 * 35.0 - 25.0)) <
            1e-3);
    const double cell = mesher.cellSizeDeg(2);
    for (std::size_t i = 0; i + 2 < mesh->triangles.size(); i += 3) {
      double west = 1e9;
      double east = -1e9;
      double south = 1e9;
      double north = -1e9;
      double lonSum = 0.0;
      double latSum = 0.0;
      for (std::size_t k = 0; k < 3; ++k) {
        const std::array<float, 2> p = lonLatDeg(*mesh, mesh->triangles[i + k]);
        west = std::min(west, static_cast<double>(p[0]));
        east = std::max(east, static_cast<double>(p[0]));
        south = std::min(south, static_cast<double>(p[1]));
        north = std::max(north, static_cast<double>(p[1]));
        lonSum += static_cast<double>(p[0]);
        latSum += static_cast<double>(p[1]);
      }
      QVERIFY(east - west <= cell + 1e-4);
      QVERIFY(north - south <= cell + 1e-4);
      // No triangle inside the lake.
      const double lon = lonSum / 3.0;
      const double lat = latSum / 3.0;
      QVERIFY(lon <= 20.0 || lon >= 25.0 || lat <= 20.0 || lat >= 25.0);
    }
    for (const ts::VectorVertex& v : mesh->triangles) {
      const ts::Vec3 expected = ts::geodeticToEcef(
          ts::Geodetic{mesh->latDeg(v), mesh->lonDeg(v), 0.0});
      const ts::Vec3 actual{
          mesh->centreEcef.x + static_cast<double>(v.offsetM[0]),
          mesh->centreEcef.y + static_cast<double>(v.offsetM[1]),
          mesh->centreEcef.z + static_cast<double>(v.offsetM[2])};
      QVERIFY(ts::length(actual - expected) < 2.0);
    }
  }

  void fillFollowsLongEdges()
  {
    // A long slanting edge is a straight line in longitude and latitude,
    // which is a curve on the globe and on a Mercator map. The fill has
    // vertices along it as close together as the outline's, so the two
    // agree in every view.
    ts::VectorMesher mesher;
    ts::FeatureTile tile;
    tile.key = ts::TileKey{0, 1, 0};
    tile.features.push_back(polygon({{0, 60, 40, 80, 0, 80}}));
    const std::optional<ts::VectorTileMesh> mesh =
        mesher.build(tile, std::atomic<bool>(false));
    if (!mesh) {
      QFAIL("no mesh");
    }
    const double longest = mesher.cellSizeDeg(0) * 0.25 + 1e-6;
    std::vector<double> alongEdge;
    for (const ts::VectorVertex& v : mesh->triangles) {
      const double lon = mesh->lonDeg(v);
      if (std::abs(mesh->latDeg(v) - (60.0 + 0.5 * lon)) < 1e-4) {
        alongEdge.push_back(lon);
      }
    }
    std::sort(alongEdge.begin(), alongEdge.end());
    QVERIFY(alongEdge.size() >= 30);
    QVERIFY(std::abs(alongEdge.front()) < 1e-4);
    QVERIFY(std::abs(alongEdge.back() - 40.0) < 1e-4);
    for (std::size_t i = 1; i < alongEdge.size(); ++i) {
      QVERIFY(alongEdge[i] - alongEdge[i - 1] <= longest);
    }
    // The outline has the same points.
    int outlineOnEdge = 0;
    for (const ts::VectorSegment& segment : mesh->segments) {
      const double lon = mesh->lonDeg(segment.a);
      if (std::abs(mesh->latDeg(segment.a) - (60.0 + 0.5 * lon)) < 1e-4 &&
          std::abs(mesh->latDeg(segment.b) -
                   (60.0 + 0.5 * mesh->lonDeg(segment.b))) < 1e-4) {
        ++outlineOnEdge;
        QVERIFY(
            std::any_of(alongEdge.begin(), alongEdge.end(),
                        [lon](double x) { return std::abs(x - lon) < 1e-4; }));
      }
    }
    QCOMPARE(outlineOnEdge, 29);
    // The area is unchanged: 40° × 20° / 2.
    QVERIFY(std::abs(planarArea(mesh->triangles) - 400.0) < 1e-2);
  }

  void outlinesLeaveTileEdgesOut()
  {
    // In tile (0, 1, 0) (0°–180°E): a polygon ending on 180° and one
    // reaching the north pole. Their edges along 180°, 0° and 90°N are
    // cuts, not shores.
    ts::VectorMesher mesher;
    ts::FeatureTile tile{ts::TileKey{0, 1, 0}, {}};
    tile.features.push_back(polygon({{170, -25, 180, -25, 180, -5, 170, -5}}));
    tile.features.push_back(polygon({{0, 75, 180, 75, 180, 90, 0, 90}}));
    const std::optional<ts::VectorTileMesh> mesh =
        mesher.build(tile, notCancelled);
    if (!mesh) {
      QFAIL("no mesh");
    }
    QVERIFY(!mesh->segments.empty());
    const double longest = mesher.cellSizeDeg(0) * 0.25 + 1e-6;
    for (const ts::VectorSegment& segment : mesh->segments) {
      const std::array<float, 2> a = lonLatDeg(*mesh, segment.a);
      const std::array<float, 2> b = lonLatDeg(*mesh, segment.b);
      QVERIFY(a[0] != 180.0F || b[0] != 180.0F);
      QVERIFY(a[0] != 0.0F || b[0] != 0.0F);
      QVERIFY(a[1] != 90.0F || b[1] != 90.0F);
      QVERIFY(static_cast<double>(std::max(std::abs(b[0] - a[0]),
                                           std::abs(b[1] - a[1]))) <= longest);
    }
  }

  void linesAndTinyPolygons()
  {
    ts::VectorMesher mesher;
    const ts::TileKey key{3, 9, 2}; // 22.5°–45°E, 22.5°–45°N
    ts::FeatureTile tile{key, {}};
    // A line string keeps every segment (split to a quarter cell).
    tile.features.push_back(lineString({23, 23, 44, 44}));
    // Smaller than tile/512: dropped; the same touching the edge: kept.
    tile.features.push_back(
        polygon({{40, 25, 40.01, 25, 40.01, 25.01, 40, 25.01}}));
    tile.features.push_back(
        polygon({{22.5, 30, 22.51, 30, 22.51, 30.01, 22.5, 30.01}}));
    const std::optional<ts::VectorTileMesh> mesh =
        mesher.build(tile, notCancelled);
    if (!mesh) {
      QFAIL("no mesh");
    }
    const double quarter = mesher.cellSizeDeg(3) * 0.25;
    const int expectedLine = static_cast<int>(std::ceil(21.0 / quarter));
    int fromLine = 0;
    bool tinyInside = false;
    for (const ts::VectorSegment& segment : mesh->segments) {
      const std::array<float, 2> a = lonLatDeg(*mesh, segment.a);
      fromLine += std::abs(a[0] - a[1]) < 1e-4F ? 1 : 0;
      tinyInside = tinyInside || (a[0] >= 39.99F && a[0] <= 40.02F &&
                                  a[1] >= 24.99F && a[1] <= 25.02F);
    }
    QCOMPARE(fromLine, expectedLine);
    QVERIFY(!tinyInside);
    QVERIFY(!mesh->triangles.empty()); // the one at the edge
  }

  void cancelledBuild()
  {
    ts::VectorMesher mesher;
    ts::FeatureTile tile{ts::TileKey{0, 0, 0}, {}};
    tile.features.push_back(polygon({{-170, 0, -10, 0, -10, 60, -170, 60}}));
    const std::atomic<bool> cancelled{true};
    QVERIFY(!mesher.build(tile, cancelled).has_value());
  }

  void concurrentMeshers()
  {
    // Reentrant: one mesher per thread gives the same mesh everywhere.
    ts::FeatureTile tile{ts::TileKey{1, 2, 0}, {}};
    tile.features.push_back(polygon(
        {{10, 10, 80, 10, 80, 60, 10, 60}, {30, 30, 40, 30, 40, 40, 30, 40}}));
    const std::optional<ts::VectorTileMesh> reference =
        ts::VectorMesher().build(tile, notCancelled);
    if (!reference) {
      QFAIL("no mesh");
    }
    const std::size_t expected = reference->triangles.size();
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&tile, expected](int) {
          ts::VectorMesher mesher;
          for (int i = 0; i < 5; ++i) {
            const std::optional<ts::VectorTileMesh> mesh =
                mesher.build(tile, notCancelled);
            if (!mesh || mesh->triangles.size() != expected) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkMeshing()
  {
    // A ragged coastline of 20 000 vertices with a lake, in a stored
    // high-resolution (level 6) tile: the size of a busy GSHHG tile.
    const ts::TileKey key{6, 70, 15}; // 16.9°–19.7°E, 47.8°–50.6°N
    const ts::GeoRect rect = key.bounds();
    std::vector<double> coast;
    const int vertices = 20000;
    for (int i = 0; i < vertices; ++i) {
      const double angle = 2.0 * 3.14159265358979 * i / vertices;
      const double radius =
          1.1 + 0.15 * std::sin(37.0 * angle) + 0.05 * std::sin(311.0 * angle);
      coast.push_back(rect.centreLonDeg() + radius * std::cos(angle));
      coast.push_back(rect.centreLatDeg() + 0.8 * radius * std::sin(angle));
    }
    const double lon = rect.centreLonDeg();
    const double lat = rect.centreLatDeg();
    ts::FeatureTile tile{key, {}};
    tile.features.push_back(
        polygon({coast,
                 {lon - 0.1, lat - 0.1, lon + 0.1, lat - 0.1, lon + 0.1,
                  lat + 0.1, lon - 0.1, lat + 0.1}}));
    ts::VectorMesher mesher;
    std::size_t triangles = 0;
    QBENCHMARK
    {
      const std::optional<ts::VectorTileMesh> mesh =
          mesher.build(tile, notCancelled);
      triangles = mesh ? mesh->triangles.size() / 3 : 0;
    }
    qInfo("%zu triangles", triangles);
  }

  void loader()
  {
    const std::shared_ptr<FixtureProvider> provider =
        std::make_shared<FixtureProvider>();
    ts::VectorTileLoader loader(provider);
    std::atomic<int> ready{0};
    loader.setTilesReadyCallback([&ready]() { ready.fetch_add(1); });
    loader.setRequests({ts::TileKey{1, 2, 0}, ts::TileKey{1, 0, 0}});
    std::vector<ts::VectorTileMesh> meshes;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          for (ts::VectorTileMesh& mesh : loader.takeLoaded(8)) {
            meshes.push_back(std::move(mesh));
          }
          return meshes.size() == 2;
        },
        10000));
    QCOMPARE(loader.outstanding(), 0);
    QCOMPARE(ready.load(), 2);
    for (const ts::VectorTileMesh& mesh : meshes) {
      // The tile with the square has triangles; the other is empty but
      // loaded.
      QCOMPARE(mesh.triangles.empty(), mesh.key != (ts::TileKey{1, 2, 0}));
    }
    // Asking again after taking loads again (the renderer keeps them).
    loader.setRequests({ts::TileKey{1, 2, 0}});
    QVERIFY(QTest::qWaitFor(
        [&]() {
          return loader.outstanding() == 0 || !loader.takeLoaded(1).empty();
        },
        10000));
    // Dropping every request leaves nothing outstanding.
    loader.setRequests({});
    QVERIFY(QTest::qWaitFor(
        [&]() {
          loader.takeLoaded(64);
          return loader.outstanding() == 0;
        },
        10000));
  }

  void loaderStress()
  {
    // A shared provider, two workers, the wanted set changing all the time
    // (run under ThreadSanitizer in CI).
    const std::shared_ptr<FixtureProvider> provider =
        std::make_shared<FixtureProvider>();
    ts::VectorTileLoaderSettings settings;
    settings.threads = 3;
    ts::VectorTileLoader loader(provider, settings);
    int taken = 0;
    for (int frame = 0; frame < 200; ++frame) {
      std::vector<ts::TileKey> keys;
      keys.reserve(6);
      for (int i = 0; i < 6; ++i) {
        keys.push_back(ts::TileKey{1, (frame + i) % 4, (frame / 4 + i) % 2});
      }
      loader.setRequests(keys);
      taken += static_cast<int>(loader.takeLoaded(4).size());
      QTest::qWait(1);
    }
    loader.setRequests({});
    QVERIFY(QTest::qWaitFor(
        [&]() {
          taken += static_cast<int>(loader.takeLoaded(64).size());
          return loader.outstanding() == 0;
        },
        10000));
    QVERIFY(taken > 0);
    QVERIFY(provider->loads.load() >= taken);
  }
};

QTEST_GUILESS_MAIN(TestVector)
#include "tst_vector.moc"
