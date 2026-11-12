// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <QFile>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#include "mockhttpserver.h"
#include "mockwms.h"
#include "ts/deps/sqliteconnection.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/regions/offlineregionstore.h"
#include "ts/regions/regiondownload.h"
#include "ts/regions/regiongeometry.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"

namespace
{

using ts::test::MockHttpServer;
using ts::test::MockWmsBehaviour;
using State = ts::RegionDownloadProgress::State;

ts::NetworkSettings testNetwork()
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.retryDelay = std::chrono::milliseconds(20);
  settings.attempts = 1;
  settings.timeout = std::chrono::milliseconds(5000);
  return settings;
}

ts::WmsSource mockSource(const QUrl& url)
{
  ts::WmsSource source;
  source.url = url;
  source.layers = QStringList{QStringLiteral("pattern")};
  source.styles = QStringList{QString()};
  source.title = QStringLiteral("Test pattern");
  return source;
}

// Size, then the byte to fill with, as QByteArray(int, char).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ts::CachedTile tileOf(int bytes, char fill, long long expiresS = 0)
{
  ts::CachedTile tile;
  tile.data = QByteArray(bytes, fill);
  tile.contentType = QByteArrayLiteral("image/png");
  tile.expiresS = expiresS;
  return tile;
}

/** Every tile at @p level that the region intersects, by brute force. */
std::set<ts::TileKey> bruteForce(const ts::RegionGeometry& region, int level,
                                 const std::optional<ts::GeoRect>& data = {})
{
  std::set<ts::TileKey> keys;
  for (int y = 0; y < ts::TileKey::rows(level); ++y) {
    for (int x = 0; x < ts::TileKey::columns(level); ++x) {
      const ts::TileKey key{level, x, y};
      const ts::GeoRect rect = key.bounds();
      ts::GeoRect clipped = rect;
      if (data) {
        clipped = ts::GeoRect{std::max(rect.westDeg, data->westDeg),
                              std::max(rect.southDeg, data->southDeg),
                              std::min(rect.eastDeg, data->eastDeg),
                              std::min(rect.northDeg, data->northDeg)};
      }
      if (clipped.widthDeg() > 0.0 && clipped.heightDeg() > 0.0 &&
          region.intersects(clipped)) {
        keys.insert(key);
      }
    }
  }
  return keys;
}

std::vector<ts::TileKey> iterate(ts::RegionTileIterator iterator)
{
  std::vector<ts::TileKey> keys;
  while (std::optional<ts::TileKey> key = iterator.next()) {
    keys.push_back(*key);
  }
  return keys;
}

/** A rough outline of Great Britain and Ireland, lon/lat. */
std::vector<ts::LonLat> britishIsles()
{
  return {{-5.8, 49.9}, {1.8, 51.0},  {1.8, 52.9},   {0.0, 53.5},
          {-1.5, 55.8}, {-1.8, 57.6}, {-3.0, 58.7},  {-5.0, 58.7},
          {-7.6, 57.0}, {-6.2, 55.5}, {-10.5, 54.0}, {-10.5, 51.4},
          {-6.0, 52.0}};
}

const std::atomic<bool> notCancelled{false};

/** The polygon through @p vertices, or an empty geometry (isValid() false). */
ts::RegionGeometry polygonOf(const std::vector<ts::LonLat>& vertices)
{
  QString error;
  return ts::RegionGeometry::polygon(vertices, &error)
      .value_or(ts::RegionGeometry());
}

/**
 * Two islands, the first (0°–20°E, 0°–20°N) with a lake (5°–15°E,
 * 5°–15°N), the second 30°–35°E, 0°–5°N.
 */
ts::RegionGeometry islandsWithALake()
{
  QString error;
  return ts::RegionGeometry::polygons({{{0, 0}, {20, 0}, {20, 20}, {0, 20}},
                                       {{5, 5}, {15, 5}, {15, 15}, {5, 15}},
                                       {{30, 0}, {35, 0}, {35, 5}, {30, 5}}},
                                      &error)
      .value_or(ts::RegionGeometry());
}

/**
 * A country split at the antimeridian as Natural Earth gives one: a ring
 * 175°E–180° and one 180°–175°W, written with wrapped longitudes.
 */
ts::RegionGeometry ringsAcrossTheAntimeridian()
{
  QString error;
  return ts::RegionGeometry::polygons(
             {{{175, 60}, {180, 60}, {180, 65}, {175, 65}},
              {{-180, 60}, {-175, 60}, {-175, 66}, {-180, 66}}},
             &error)
      .value_or(ts::RegionGeometry());
}

} // namespace

/** Unit tests for ts/regions. */
class TestRegions : public QObject
{
  Q_OBJECT

private slots:
  // --- Geometry
  // ---------------------------------------------------------------

  void rectangles()
  {
    const ts::RegionGeometry plain =
        ts::RegionGeometry::rectangle(-10.0, 49.0, 2.0, 61.0);
    QVERIFY(plain.isValid());
    QCOMPARE(plain.kind(), ts::RegionGeometry::Kind::Rectangle);
    QCOMPARE(plain.extent().westDeg, -10.0);
    QCOMPARE(plain.extent().eastDeg, 2.0);
    QVERIFY(plain.contains(55.0, -3.0));
    QVERIFY(!plain.contains(55.0, 3.0));
    QVERIFY(plain.contains(55.0, -3.0 + 360.0)); // any longitude

    // Across the antimeridian: 170° E to 170° W, kept as 170° to 190°.
    const ts::RegionGeometry pacific =
        ts::RegionGeometry::rectangle(170.0, -10.0, -170.0, 10.0);
    QCOMPARE(pacific.extent().westDeg, 170.0);
    QCOMPARE(pacific.extent().eastDeg, 190.0);
    QVERIFY(pacific.contains(0.0, 179.0));
    QVERIFY(pacific.contains(0.0, -175.0));
    QVERIFY(!pacific.contains(0.0, 0.0));
    QVERIFY(pacific.intersects(ts::GeoRect{-180.0, -5.0, -175.0, 5.0}));
    QVERIFY(pacific.covers(ts::GeoRect{-180.0, -5.0, -175.0, 5.0}));
    QVERIFY(pacific.covers(ts::GeoRect{175.0, -5.0, 180.0, 5.0}));
    QVERIFY(!pacific.intersects(ts::GeoRect{-170.0, -5.0, -160.0, 5.0}));

    // Latitudes clamped and swapped; a span of 360° is the whole way round.
    const ts::RegionGeometry world =
        ts::RegionGeometry::rectangle(30.0, 100.0, 390.0, -100.0);
    QCOMPARE(world.extent().southDeg, -90.0);
    QCOMPARE(world.extent().northDeg, 90.0);
    QCOMPARE(world.extent().westDeg, -180.0);
    QCOMPARE(world.extent().eastDeg, 180.0);
    QVERIFY(world.covers(ts::GeoRect{-180.0, -90.0, 0.0, 90.0}));
    // Area of the whole Earth: 510 million km².
    QVERIFY(std::abs(world.areaKm2() / 510.07e6 - 1.0) < 1e-3);

    // Touching an edge isn't intersecting.
    QVERIFY(!plain.intersects(ts::GeoRect{2.0, 50.0, 5.0, 55.0}));
    QVERIFY(!ts::RegionGeometry().isValid());
    QVERIFY(!ts::RegionGeometry::rectangle(0.0, 10.0, 5.0, 10.0).isValid());
  }

  void polygons()
  {
    QString error;
    // Unwrapped across the antimeridian, whichever side it starts.
    const ts::RegionGeometry fiji = polygonOf(
        {{176.0, -20.0}, {-178.0, -20.0}, {-178.0, -15.0}, {176.0, -15.0}});
    QVERIFY(fiji.isValid());
    QCOMPARE(fiji.extent().westDeg, 176.0);
    QCOMPARE(fiji.extent().eastDeg, 182.0);
    QVERIFY(fiji.contains(-17.0, 179.0));
    QVERIFY(fiji.contains(-17.0, -179.0));
    QVERIFY(!fiji.contains(-17.0, -177.0));
    const ts::RegionGeometry fromEast =
        polygonOf({{-178.0, -20.0},
                   {-178.0, -15.0},
                   {176.0, -15.0},
                   {176.0, -20.0},
                   {-178.0, -20.0}}); // closed: dropped
    QVERIFY(fromEast.isValid());
    QCOMPARE(fromEast.vertices().size(), std::size_t{4});
    QCOMPARE(fromEast.extent().westDeg, 176.0);
    QCOMPARE(fromEast.extent().eastDeg, 182.0);

    // A triangle; contains() by the even–odd rule.
    const ts::RegionGeometry triangle =
        polygonOf({{0.0, 0.0}, {40.0, 0.0}, {0.0, 40.0}});
    QVERIFY(triangle.isValid());
    QVERIFY(triangle.contains(10.0, 10.0));
    QVERIFY(!triangle.contains(30.0, 30.0));
    QVERIFY(triangle.intersects(ts::GeoRect{15.0, 15.0, 25.0, 25.0}));
    QVERIFY(!triangle.intersects(ts::GeoRect{25.0, 25.0, 30.0, 30.0}));
    QVERIFY(triangle.covers(ts::GeoRect{1.0, 1.0, 5.0, 5.0}));
    QVERIFY(!triangle.covers(ts::GeoRect{15.0, 15.0, 25.0, 25.0}));
    // The rectangle holds the whole triangle: intersecting, not covered.
    QVERIFY(triangle.intersects(ts::GeoRect{-5.0, -5.0, 45.0, 45.0}));
    QVERIFY(!triangle.covers(ts::GeoRect{-5.0, -5.0, 45.0, 45.0}));

    // Refused: too few points, off the Earth, round a pole.
    QVERIFY(!ts::RegionGeometry::polygon({{0.0, 0.0}, {1.0, 1.0}}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!ts::RegionGeometry::polygon(
        {{0.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}}, &error));
    QVERIFY(!ts::RegionGeometry::polygon({{0.0, 0.0}, {1.0, 95.0}, {2.0, 0.0}},
                                         &error));
    error.clear();
    QVERIFY(!ts::RegionGeometry::polygon(
        {{0.0, 80.0}, {90.0, 80.0}, {180.0, 80.0}, {-90.0, 80.0}}, &error));
    QVERIFY(error.contains(QStringLiteral("pole")));
  }

  void multipleRings()
  {
    // Even–odd over all rings: the lake is a hole, the second island land.
    const ts::RegionGeometry islands = islandsWithALake();
    QVERIFY(islands.isValid());
    QCOMPARE(islands.kind(), ts::RegionGeometry::Kind::Polygon);
    QCOMPARE(islands.rings().size(), std::size_t{3});
    QCOMPARE(islands.vertices().size(), std::size_t{4});
    QVERIFY(islands.contains(2.0, 2.0));
    QVERIFY(!islands.contains(10.0, 10.0)); // the lake
    QVERIFY(islands.contains(2.0, 32.0));
    QVERIFY(!islands.contains(2.0, 25.0)); // between the islands
    const ts::GeoRect inLake{8.0, 8.0, 12.0, 12.0};
    QVERIFY(!islands.intersects(inLake));
    QVERIFY(islands.covers(ts::GeoRect{1.0, 1.0, 4.0, 4.0}));
    QVERIFY(!islands.covers(ts::GeoRect{1.0, 1.0, 9.0, 9.0})); // lake edge
    QCOMPARE(islands.extent().westDeg, 0.0);
    QCOMPARE(islands.extent().eastDeg, 35.0);
    // The area: island minus lake, plus the second island.
    QString error;
    const double outer = ts::RegionGeometry::polygon(
                             {{0, 0}, {20, 0}, {20, 20}, {0, 20}}, &error)
                             .value_or(ts::RegionGeometry())
                             .areaKm2();
    const double lake = ts::RegionGeometry::polygon(
                            {{5, 5}, {15, 5}, {15, 15}, {5, 15}}, &error)
                            .value_or(ts::RegionGeometry())
                            .areaKm2();
    const double second = ts::RegionGeometry::polygon(
                              {{30, 0}, {35, 0}, {35, 5}, {30, 5}}, &error)
                              .value_or(ts::RegionGeometry())
                              .areaKm2();
    QVERIFY(std::abs(islands.areaKm2() - (outer - lake + second)) < 1e-6);

    // Rings either side of the antimeridian stay together: 175°E to 185°.
    const ts::RegionGeometry split = ringsAcrossTheAntimeridian();
    QVERIFY(split.isValid());
    QCOMPARE(split.extent().westDeg, 175.0);
    QCOMPARE(split.extent().eastDeg, 185.0);
    QVERIFY(split.contains(62.0, 179.0));
    QVERIFY(split.contains(62.0, -179.0));
    QVERIFY(split.contains(65.5, -176.0));
    QVERIFY(!split.contains(65.5, 176.0));
    QVERIFY(split.intersects(ts::GeoRect{-180.0, 60.0, -177.0, 63.0}));
    QVERIFY(split.intersects(ts::GeoRect{177.0, 60.0, 180.0, 63.0}));
    QVERIFY(split.covers(ts::GeoRect{-180.0, 61.0, -177.0, 63.0}));
    QVERIFY(!split.intersects(ts::GeoRect{170.0, 60.0, 174.0, 63.0}));

    // More than the whole way round is refused.
    QVERIFY(!ts::RegionGeometry::polygons({{{0, 0}, {10, 0}, {10, 5}},
                                           {{170, 0}, {-100, 0}, {-100, 5}},
                                           {{-170, 0}, {100, 0}, {100, 5}}},
                                          &error)
                 .has_value());
    QVERIFY(!error.isEmpty());
    QVERIFY(!ts::RegionGeometry::polygons({}, &error).has_value());
    // A bad ring spoils the whole.
    QVERIFY(!ts::RegionGeometry::polygons(
                 {{{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {1, 0}}}, &error)
                 .has_value());
  }

  void geometryJson()
  {
    QString error;
    const std::vector<ts::RegionGeometry> geometries{
        ts::RegionGeometry::rectangle(-10.0, 49.0, 2.0, 61.0),
        ts::RegionGeometry::rectangle(170.0, -10.0, -170.0, 10.0),
        ts::RegionGeometry::rectangle(-180.0, -90.0, 180.0, 90.0),
        polygonOf(britishIsles()),
        polygonOf({{176.0, -20.0}, {-178.0, -20.0}, {-178.0, -15.0}}),
        islandsWithALake(),
        ringsAcrossTheAntimeridian()};
    for (const ts::RegionGeometry& geometry : geometries) {
      const std::optional<ts::RegionGeometry> back =
          ts::RegionGeometry::fromJson(geometry.toJson(), &error);
      QVERIFY2(back, qPrintable(error));
      QVERIFY(back.value_or(ts::RegionGeometry()) == geometry);
    }
    QVERIFY(!ts::RegionGeometry::fromJson(QJsonObject{}, &error));
    QVERIFY(!ts::RegionGeometry::fromJson(
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("rectangle")},
                    {QStringLiteral("west"), 1.0}},
        &error));
  }

  // --- Tile sets
  // ----------------------------------------------------------------

  void knownTileSets()
  {
    // The north-east quarter of the world: one tile per level down to
    // level 1, then four times as many each level.
    const ts::RegionGeometry quarter =
        ts::RegionGeometry::rectangle(0.0, 0.0, 90.0, 90.0);
    const ts::RegionTileCount counts =
        ts::countRegionTiles(quarter, 0, 3, std::nullopt);
    QCOMPARE(counts.perLevel, (std::vector<long long>{1, 1, 4, 16}));
    QCOMPARE(counts.total(), 22LL);
    const std::vector<ts::TileKey> keys =
        iterate(ts::RegionTileIterator(quarter, 0, 3, std::nullopt));
    QCOMPARE(keys.size(), std::size_t{22});
    QCOMPARE(keys.at(0), (ts::TileKey{0, 1, 0}));
    QCOMPARE(keys.at(1), (ts::TileKey{1, 2, 0}));
    // Coarsest first.
    QVERIFY(std::is_sorted(
        keys.begin(), keys.end(),
        [](ts::TileKey a, ts::TileKey b) { return a.level < b.level; }));

    // 170° E to 170° W, 10° S to 10° N at level 3 (22.5° tiles): the
    // first and last columns, two rows.
    const ts::RegionGeometry pacific =
        ts::RegionGeometry::rectangle(170.0, -10.0, -170.0, 10.0);
    const std::vector<ts::TileKey> pacificKeys =
        iterate(ts::RegionTileIterator(pacific, 3, 3, std::nullopt));
    const std::set<ts::TileKey> pacificSet(pacificKeys.begin(),
                                           pacificKeys.end());
    QCOMPARE(pacificSet, (std::set<ts::TileKey>{
                             {3, 0, 3}, {3, 15, 3}, {3, 0, 4}, {3, 15, 4}}));
    QCOMPARE(ts::countRegionTiles(pacific, 3, 3, std::nullopt).total(), 4LL);

    // A rectangle reaching the north pole takes the whole top row there.
    const ts::RegionGeometry arctic =
        ts::RegionGeometry::rectangle(-180.0, 80.0, 180.0, 90.0);
    QCOMPARE(ts::countRegionTiles(arctic, 4, 4, std::nullopt).total(),
             ts::TileKey::columns(4));

    // Levels the wrong way round, or an empty area: nothing.
    QCOMPARE(ts::countRegionTiles(quarter, 3, 2, std::nullopt).total(), 0LL);
    QVERIFY(!ts::RegionTileIterator(ts::RegionGeometry(), 0, 3, std::nullopt)
                 .next());
  }

  void tileSetsMatchBruteForce_data()
  {
    QTest::addColumn<int>("which");
    QTest::newRow("British Isles") << 0;
    QTest::newRow("Fiji across the antimeridian") << 1;
    QTest::newRow("Bering Strait polygon") << 2;
    QTest::newRow("Pacific rectangle") << 3;
    QTest::newRow("concave") << 4;
    QTest::newRow("with data bounds") << 5;
    QTest::newRow("islands with a lake") << 6;
    QTest::newRow("rings either side of the antimeridian") << 7;
  }

  void tileSetsMatchBruteForce()
  {
    // countRegionTiles() (with its shortcut for covered tiles) and the
    // iterator give exactly the tiles a test of every tile gives.
    QFETCH(int, which);
    ts::RegionGeometry region;
    std::optional<ts::GeoRect> data;
    switch (which) {
    case 0:
    case 5:
      region = polygonOf(britishIsles());
      if (which == 5) {
        data = ts::GeoRect{-8.0, 50.0, 0.0, 70.0};
      }
      break;
    case 1:
      region = polygonOf(
          {{176.0, -20.0}, {-178.0, -20.0}, {-178.0, -15.0}, {176.0, -15.0}});
      break;
    case 2:
      region = polygonOf(
          {{160.0, 50.0}, {-150.0, 52.0}, {-160.0, 70.0}, {175.0, 68.0}});
      break;
    case 3:
      region = ts::RegionGeometry::rectangle(150.0, -40.0, -120.0, 25.0);
      break;
    case 6:
      region = islandsWithALake();
      break;
    case 7:
      region = ringsAcrossTheAntimeridian();
      break;
    default:
      region = polygonOf(
          {{0.0, 0.0}, {40.0, 0.0}, {40.0, 40.0}, {20.0, 10.0}, {0.0, 40.0}});
      break;
    }
    QVERIFY(region.isValid());
    const int maxLevel = 7;
    const ts::RegionTileCount counts =
        ts::countRegionTiles(region, 0, maxLevel, data);
    const std::vector<ts::TileKey> keys =
        iterate(ts::RegionTileIterator(region, 0, maxLevel, data));
    QCOMPARE(static_cast<long long>(keys.size()), counts.total());
    for (int level = 0; level <= maxLevel; ++level) {
      std::set<ts::TileKey> fromIterator;
      for (const ts::TileKey key : keys) {
        if (key.level == level) {
          QVERIFY(fromIterator.insert(key).second); // no duplicates
        }
      }
      const std::set<ts::TileKey> expected = bruteForce(region, level, data);
      QCOMPARE(fromIterator, expected);
      QCOMPARE(counts.perLevel.at(static_cast<std::size_t>(level)),
               static_cast<long long>(expected.size()));
    }
  }

  void countingStopsPastALimit()
  {
    const ts::RegionGeometry islands = islandsWithALake();
    const ts::RegionTileCount all =
        ts::countRegionTiles(islands, 0, 12, std::nullopt);
    QVERIFY(all.complete);
    QVERIFY(all.total() > 100000);
    const ts::RegionTileCount stopped =
        ts::countRegionTiles(islands, 0, 12, std::nullopt, 1000);
    QVERIFY(!stopped.complete);
    QVERIFY(stopped.total() > 1000);
    QVERIFY(stopped.total() < all.total());
    const ts::RegionTileCount under =
        ts::countRegionTiles(islands, 0, 12, std::nullopt, all.total());
    QVERIFY(under.complete);
    QCOMPARE(under.total(), all.total());
  }

  void largeCountsAreFast()
  {
    // The British Isles to level 16: millions of tiles, counted without
    // visiting them all.
    const ts::RegionGeometry isles = polygonOf(britishIsles());
    QVERIFY(isles.isValid());
    QElapsedTimer timer;
    timer.start();
    const ts::RegionTileCount counts =
        ts::countRegionTiles(isles, 0, 16, std::nullopt);
    QVERIFY2(timer.elapsed() < 5000,
             qPrintable(QString::number(timer.elapsed())));
    // Each level about four times the one before, near the end.
    const double ratio = static_cast<double>(counts.perLevel.at(16)) /
                         static_cast<double>(counts.perLevel.at(15));
    QVERIFY(ratio > 3.9 && ratio < 4.1);
    // Level 16 tiles are 0.00275° square: the area over the tile area,
    // plus the tiles along the edge.
    const double tileKm2 =
        isles.areaKm2() / static_cast<double>(counts.perLevel.at(16));
    const double expectedKm2 = std::pow(ts::TileKey::sizeDeg(16) * 111.2, 2.0) *
                               std::cos(54.5 * 3.14159265358979 / 180.0);
    QVERIFY2(std::abs(tileKm2 / expectedKm2 - 1.0) < 0.02,
             qPrintable(QString::number(tileKm2 / expectedKm2)));
  }

  void samplesLieInTheRegion()
  {
    const ts::RegionGeometry isles = polygonOf(britishIsles());
    QVERIFY(isles.isValid());
    const ts::RegionTileCount counts =
        ts::countRegionTiles(isles, 2, 9, std::nullopt);
    // A fixed seed: the same samples every run.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
    std::mt19937_64 random(7);
    const std::vector<ts::TileKey> samples =
        ts::sampleRegionTiles(isles, counts, std::nullopt, 40, random);
    QCOMPARE(samples.size(), std::size_t{40});
    std::set<ts::TileKey> distinct(samples.begin(), samples.end());
    QCOMPARE(distinct.size(), samples.size());
    int deepest = 0;
    for (const ts::TileKey key : samples) {
      QVERIFY(key.level >= 2 && key.level <= 9);
      QVERIFY(isles.intersects(key.bounds()));
      deepest += key.level == 9 ? 1 : 0;
    }
    // Level 9 holds three quarters of the tiles.
    QVERIFY2(deepest >= 20, qPrintable(QString::number(deepest)));
    // A region of three tiles: at most three samples.
    const ts::RegionGeometry small =
        ts::RegionGeometry::rectangle(0.0, 0.0, 90.0, 90.0);
    const std::vector<ts::TileKey> few = ts::sampleRegionTiles(
        small, ts::countRegionTiles(small, 0, 1, std::nullopt), std::nullopt,
        10, random);
    QCOMPARE(few.size(), std::size_t{2});
  }

  // --- The list of regions
  // -----------------------------------------------------

  void regionStore()
  {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("sub/regions.json"));
    QString error;
    ts::OfflineRegionStore store(path);
    QVERIFY(store.load(&error)); // no file: empty
    QVERIFY(store.regions().empty());
    ts::OfflineRegion uk;
    uk.name = QStringLiteral("United Kingdom");
    uk.source = QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("wms")},
        {QStringLiteral("url"), QStringLiteral("https://example.org/wms")}};
    uk.sourceName = QStringLiteral("Example");
    uk.geometry = polygonOf(britishIsles());
    uk.minimumLevel = 2;
    uk.maximumLevel = 11;
    uk.created = QDateTime::fromString(QStringLiteral("2026-11-12T10:00:00Z"),
                                       Qt::ISODate);
    QVERIFY(uk.created.isValid());
    uk.tiles = 12345;
    uk.bytes = 678901234;
    const long long first = store.add(uk);
    ts::OfflineRegion pacific = uk;
    pacific.name = QStringLiteral("Pacific");
    pacific.geometry =
        ts::RegionGeometry::rectangle(170.0, -10.0, -170.0, 10.0);
    const long long second = store.add(pacific);
    QVERIFY(first > 0 && second > first);
    QVERIFY(store.save(&error));

    ts::OfflineRegionStore again(path);
    QVERIFY(again.load(&error));
    QCOMPARE(again.regions().size(), std::size_t{2});
    const ts::OfflineRegion* back = again.find(first);
    QVERIFY(back);
    QCOMPARE(back->name, uk.name);
    QCOMPARE(back->source, uk.source);
    QCOMPARE(back->sourceName, uk.sourceName);
    QVERIFY(back->geometry == uk.geometry);
    QCOMPARE(back->minimumLevel, 2);
    QCOMPARE(back->maximumLevel, 11);
    QCOMPARE(back->created, uk.created);
    QVERIFY(!back->downloaded.isValid());
    QCOMPARE(back->tiles, 12345LL);
    QCOMPARE(back->bytes, 678901234LL);
    QVERIFY(again.find(second)->geometry == pacific.geometry);
    // Ids are never reused, even after the newest is removed.
    QVERIFY(again.remove(second));
    QVERIFY(!again.remove(second));
    QVERIFY(again.save(&error));
    ts::OfflineRegionStore third(path);
    QVERIFY(third.load(&error));
    QVERIFY(third.add(pacific) > second);

    // Not a region file, or a newer one: refused.
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // Not a raw string: moc can't read this file with one in it.
    // NOLINTNEXTLINE(modernize-raw-string-literal)
    file.write("{\"format\": \"terrasoar-offline-regions\", \"version\": 99}");
    file.close();
    QVERIFY(!third.load(&error));
    QVERIFY(error.contains(QStringLiteral("newer")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("garbage");
    file.close();
    QVERIFY(!third.load(&error));
    QVERIFY(third.regions().empty());
  }

  // --- Pinned tiles in the disk cache
  // --------------------------------------------

  void pinnedTilesSurviveCachePressure()
  {
    // A 50 KB cache; region 7 pins three 10 KB tiles. Filling the cache
    // with other tiles evicts only unpinned ones, oldest first, and the
    // pinned tiles don't count against the limit.
    QTemporaryDir dir;
    ts::TileDiskCache cache(dir.filePath(QStringLiteral("c.sqlite")),
                            50LL * 1000);
    for (int i = 0; i < 3; ++i) {
      QVERIFY(cache.store(ts::TileKey{5, i, 0}, tileOf(10000, 'p'), 7));
    }
    QVERIFY(cache.isPinned(ts::TileKey{5, 0, 0}));
    QCOMPARE(cache.pinnedBytes(), 30000LL);
    for (int i = 0; i < 5; ++i) {
      QVERIFY(cache.store(ts::TileKey{6, i, 0}, tileOf(10000, 'u')));
    }
    QCOMPARE(cache.count(), 8); // 30 KB pinned + 50 KB unpinned: no eviction
    QVERIFY(cache.store(ts::TileKey{6, 5, 0}, tileOf(10000, 'u')));
    // Unpinned over 50 KB: down to 45 KB, the two oldest unpinned go.
    QVERIFY(!cache.find(ts::TileKey{6, 0, 0}));
    QVERIFY(!cache.find(ts::TileKey{6, 1, 0}));
    QVERIFY(cache.find(ts::TileKey{6, 2, 0}));
    for (int i = 0; i < 3; ++i) {
      QVERIFY(cache.find(ts::TileKey{5, i, 0}));
    }
    QCOMPARE(cache.sizeBytes(), 70000LL);
    // Much more pressure: the pinned tiles stay.
    for (int i = 0; i < 50; ++i) {
      QVERIFY(cache.store(ts::TileKey{8, i, 1}, tileOf(10000, 'v')));
    }
    for (int i = 0; i < 3; ++i) {
      QVERIFY(cache.find(ts::TileKey{5, i, 0}));
    }
    QVERIFY(cache.sizeBytes() - cache.pinnedBytes() <= cache.maximumBytes());
    const ts::RegionUsage usage = cache.regionUsage(7);
    QCOMPARE(usage.tiles, 3LL);
    QCOMPARE(usage.bytes, 30000LL);
    QCOMPARE(cache.pinnedRegions(), std::vector<long long>{7});
  }

  void unpinningReturnsTilesToTheCache()
  {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("c.sqlite"));
    {
      ts::TileDiskCache cache(path, 50LL * 1000);
      // Two regions share tile 4/0/0.
      QVERIFY(cache.store(ts::TileKey{4, 0, 0}, tileOf(10000, 'a'), 1));
      QVERIFY(cache.pin(2, ts::TileKey{4, 0, 0}));
      QVERIFY(cache.store(ts::TileKey{4, 1, 0}, tileOf(10000, 'b'), 2));
      QVERIFY(!cache.pin(2, ts::TileKey{4, 9, 0})); // not stored
      QCOMPARE(cache.pinnedBytes(), 20000LL);
      // Replacing a pinned tile keeps it pinned, with its new size.
      QVERIFY(cache.store(ts::TileKey{4, 1, 0}, tileOf(12000, 'c')));
      QVERIFY(cache.isPinned(ts::TileKey{4, 1, 0}));
      QCOMPARE(cache.pinnedBytes(), 22000LL);
      for (int i = 0; i < 4; ++i) {
        QVERIFY(cache.store(ts::TileKey{7, i, 0}, tileOf(10000, 'u')));
      }
    }
    // After a restart the pins and the sums are still there.
    ts::TileDiskCache cache(path, 50LL * 1000);
    QCOMPARE(cache.pinnedBytes(), 22000LL);
    QCOMPARE(cache.pinnedRegions(), (std::vector<long long>{1, 2}));
    cache.unpinRegion(1);
    QVERIFY(cache.isPinned(ts::TileKey{4, 0, 0})); // region 2 still has it
    cache.unpinRegion(2);
    QVERIFY(!cache.isPinned(ts::TileKey{4, 0, 0}));
    QCOMPARE(cache.pinnedBytes(), 0LL);
    // Now 62 KB unpinned in a 50 KB cache: evicted at once, oldest first
    // (the formerly pinned tiles are the oldest).
    QVERIFY(cache.sizeBytes() <= 45000LL);
    QVERIFY(!cache.find(ts::TileKey{4, 0, 0}));
    QVERIFY(cache.find(ts::TileKey{7, 3, 0}));
    QVERIFY(cache.pinnedRegions().empty());
    // Removing a pinned tile removes its pins.
    QVERIFY(cache.store(ts::TileKey{2, 0, 0}, tileOf(100, 'x'), 3));
    cache.remove(ts::TileKey{2, 0, 0});
    QCOMPARE(cache.pinnedBytes(), 0LL);
    QVERIFY(cache.pinnedRegions().empty());
  }

  void cacheOfVersion1IsUpgraded()
  {
    // A cache written before pins existed keeps its tiles.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("c.sqlite"));
    {
      ts::TileDiskCache cache(path, 50LL * 1000);
      QVERIFY(cache.store(ts::TileKey{1, 0, 0}, tileOf(1000, 'a')));
    }
    {
      ts::SqliteConnection connection(
          path, ts::SqliteConnection::OpenMode::ReadWrite);
      QVERIFY(connection.execute(
          QStringLiteral("DROP TABLE pins; PRAGMA user_version=1")));
    }
    ts::TileDiskCache cache(path, 50LL * 1000);
    QCOMPARE(cache.count(), 1);
    QCOMPARE(cache.recoveries(), 0);
    QVERIFY(cache.pin(5, ts::TileKey{1, 0, 0}));
    QCOMPARE(cache.regionUsage(5).tiles, 1LL);
  }

  // --- The bulk-download flag
  // ------------------------------------------------------

  void bulkDownloadPolicy()
  {
    QString error;
    const std::optional<std::vector<ts::WmsCatalogueEntry>> parsed =
        ts::parseWmsCatalogue(
            "{\"format\": \"terrasoar-wms-catalogue\", \"version\": 1,"
            " \"services\": ["
            "  {\"title\": \"Open\", \"url\": \"https://open.example/wms\","
            "   \"allowBulkDownload\": true},"
            "  {\"title\": \"Closed\","
            "   \"url\": \"https://closed.example/osm/service\","
            "   \"allowBulkDownload\": false},"
            "  {\"title\": \"Silent\", \"url\": "
            "\"https://silent.example/wms\"}]}",
            &error);
    QVERIFY2(parsed, qPrintable(error));
    const std::vector<ts::WmsCatalogueEntry> catalogue =
        parsed.value_or(std::vector<ts::WmsCatalogueEntry>{});
    QCOMPARE(catalogue.at(0).allowBulkDownload, std::optional<bool>(true));
    QCOMPARE(catalogue.at(1).allowBulkDownload, std::optional<bool>(false));
    QVERIFY(!catalogue.at(2).allowBulkDownload);
    using Policy = ts::BulkDownloadPolicy;
    const std::function<Policy(const char*)> policy = [&](const char* url) {
      return ts::bulkDownloadPolicy(QUrl(QLatin1String(url)), catalogue);
    };
    // Query, scheme, host case and a trailing slash don't matter.
    QCOMPARE(policy("https://open.example/wms?map=x&REQUEST=GetMap"),
             Policy::Allowed);
    QCOMPARE(policy("http://OPEN.example/wms/"), Policy::Allowed);
    QCOMPARE(policy("https://closed.example/osm/service"), Policy::Forbidden);
    // Another path on a forbidding host: still forbidden; on an allowing
    // host: unknown (its terms may differ).
    QCOMPARE(policy("https://closed.example/other"), Policy::Forbidden);
    QCOMPARE(policy("https://open.example/elsewhere"), Policy::Unknown);
    QCOMPARE(policy("https://silent.example/wms"), Policy::Unknown);
    QCOMPARE(policy("https://unlisted.example/wms"), Policy::Unknown);
    QCOMPARE(ts::findWmsCatalogueEntry(
                 QUrl(QStringLiteral("https://closed.example/osm/service")),
                 catalogue)
                 ->title,
             QStringLiteral("Closed"));

    // The shipped catalogue: OpenStreetMap-based tiles may not be
    // downloaded in bulk; NASA GIBS may.
    QFile file(QStringLiteral(TS_REPO_DATA_DIR "/wms-catalogue.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const std::vector<ts::WmsCatalogueEntry> shipped =
        ts::parseWmsCatalogue(file.readAll(), &error)
            .value_or(std::vector<ts::WmsCatalogueEntry>{});
    QVERIFY(!shipped.empty());
    QCOMPARE(ts::bulkDownloadPolicy(
                 QUrl(QStringLiteral("https://ows.terrestris.de/osm/service")),
                 shipped),
             Policy::Forbidden);
    QCOMPARE(ts::bulkDownloadPolicy(
                 QUrl(QStringLiteral(
                     "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/"
                     "wms.cgi")),
                 shipped),
             Policy::Allowed);
  }

  void forbiddenSourcesRefuseDownloads()
  {
    // A region of a source whose catalogue entry forbids bulk downloads
    // is refused before any request.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir dir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = dir.path();
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(server.url(QStringLiteral("/wms"))),
            std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    ts::WmsCatalogueEntry entry;
    entry.title = QStringLiteral("Mock");
    entry.url = server.url(QStringLiteral("/wms"));
    entry.allowBulkDownload = false;
    const ts::RegionGeometry area =
        ts::RegionGeometry::rectangle(0.0, 0.0, 90.0, 90.0);
    ts::RegionDownload download(provider, 1, area, 0, 3);
    QString error;
    QVERIFY(!download.start({entry}, &error));
    QVERIFY(error.contains(QStringLiteral("Mock")));
    QCOMPARE(download.progress().state, State::Refused);
    QVERIFY(download.waitUntilSettled(std::chrono::milliseconds(100)));
    QCOMPARE(ts::test::mockGetMapCount(server), 0);
    QCOMPARE(provider->diskCache()->count(), 0);
    // Allowed, or not in the catalogue: it runs.
    entry.allowBulkDownload = true;
    ts::RegionDownload allowed(provider, 2, area, 0, 1);
    QVERIFY(allowed.start({entry}, &error));
    QVERIFY(allowed.waitUntilSettled(std::chrono::seconds(20)));
    QCOMPARE(allowed.progress().state, State::Finished);
    ts::RegionDownload unlisted(provider, 3, area, 0, 1);
    QVERIFY(unlisted.start({}, &error));
    QVERIFY(unlisted.waitUntilSettled(std::chrono::seconds(20)));
    QCOMPARE(unlisted.progress().state, State::Finished);
  }

  // --- Downloads against the mock WMS
  // ---------------------------------------------

  void downloadPinsEveryTile()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(testNetwork());
    QTemporaryDir dir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = dir.path();
    settings.cacheBytes = 1000; // tiny: only pins keep the tiles
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(server.url(QStringLiteral("/wms"))), network, settings);
    QString error;
    const ts::RegionGeometry isles = polygonOf(britishIsles());
    ts::RegionDownload download(provider, 4, isles, 0, 6);
    QVERIFY(download.start({}, &error));
    QVERIFY(download.waitUntilSettled(std::chrono::seconds(60)));
    const ts::RegionDownloadProgress progress = download.progress();
    QCOMPARE(progress.state, State::Finished);
    const long long expected =
        ts::countRegionTiles(isles, 0, 6, std::nullopt).total();
    QCOMPARE(progress.totalTiles, expected);
    QCOMPARE(progress.doneTiles, expected);
    QCOMPARE(progress.downloadedTiles, expected);
    QCOMPARE(progress.failedTiles, 0LL);
    QCOMPARE(progress.fraction(), 1.0);
    QCOMPARE(ts::test::mockGetMapCount(server), static_cast<int>(expected));
    // Every tile is stored and pinned, though the cache's limit is far
    // smaller.
    ts::TileDiskCache* cache = provider->diskCache();
    for (const ts::TileKey key :
         iterate(ts::RegionTileIterator(isles, 0, 6, std::nullopt))) {
      QVERIFY2(cache->isPinned(key), qPrintable(key.toString()));
    }
    const ts::RegionUsage usage = cache->regionUsage(4);
    QCOMPARE(usage.tiles, expected);
    QCOMPARE(usage.bytes, progress.bytes);

    // Again (resuming or refreshing): nothing fetched, all already there.
    ts::RegionDownload again(provider, 4, isles, 0, 6);
    QVERIFY(again.start({}, &error));
    QVERIFY(again.waitUntilSettled(std::chrono::seconds(60)));
    QCOMPARE(again.progress().cachedTiles, expected);
    QCOMPARE(again.progress().downloadedTiles, 0LL);
    QCOMPARE(ts::test::mockGetMapCount(server), static_cast<int>(expected));

    // Data bounds: tiles outside the source's bounds aren't requested.
    ts::WmsSource bounded = mockSource(server.url(QStringLiteral("/wms")));
    bounded.bounds = ts::GeoRect{-3.0, 50.0, 10.0, 60.0};
    std::shared_ptr<ts::WmsImageryProvider> boundedProvider =
        std::make_shared<ts::WmsImageryProvider>(bounded, network, settings);
    ts::RegionDownload clipped(boundedProvider, 5, isles, 5, 6);
    QCOMPARE(clipped.progress().totalTiles,
             ts::countRegionTiles(isles, 5, 6, bounded.bounds).total());
    QVERIFY(clipped.progress().totalTiles <
            ts::countRegionTiles(isles, 5, 6, std::nullopt).total());
  }

  void pauseResumeAndCancel()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->delayMs = 20;
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir dir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = dir.path();
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(server.url(QStringLiteral("/wms"))),
            std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    const ts::RegionGeometry area =
        ts::RegionGeometry::rectangle(-20.0, 30.0, 20.0, 60.0);
    QString error;
    ts::RegionDownload download(provider, 1, area, 0, 6);
    const long long total = download.progress().totalTiles;
    QVERIFY(total > 60);
    QVERIFY(download.start({}, &error));
    QVERIFY(QTest::qWaitFor(
        [&]() { return download.progress().doneTiles >= 10; }, 20000));
    download.pause();
    QVERIFY(download.waitUntilSettled(std::chrono::seconds(10)));
    const ts::RegionDownloadProgress paused = download.progress();
    QCOMPARE(paused.state, State::Paused);
    QVERIFY(paused.doneTiles < total);
    const int requests = ts::test::mockGetMapCount(server);
    QTest::qWait(300);
    QCOMPARE(download.progress().doneTiles, paused.doneTiles);
    QCOMPARE(ts::test::mockGetMapCount(server), requests);
    download.resume();
    QVERIFY(download.waitUntilSettled(std::chrono::seconds(60)));
    QCOMPARE(download.progress().state, State::Finished);
    QCOMPARE(download.progress().doneTiles, total);
    // Every tile requested once.
    QCOMPARE(ts::test::mockGetMapCount(server), static_cast<int>(total));

    // Cancel: stops at once; what was stored stays pinned until the
    // region is unpinned.
    behaviour->delayMs = 200;
    ts::RegionDownload cancelled(provider, 2, area, 7, 8);
    QVERIFY(cancelled.start({}, &error));
    QVERIFY(QTest::qWaitFor(
        [&]() { return cancelled.progress().doneTiles >= 2; }, 20000));
    QElapsedTimer timer;
    timer.start();
    cancelled.cancel();
    QVERIFY(cancelled.waitUntilSettled(std::chrono::seconds(1)));
    QCOMPARE(cancelled.progress().state, State::Cancelled);
    QVERIFY(provider->diskCache()->regionUsage(2).tiles >= 2);
    provider->diskCache()->unpinRegion(2);
    QCOMPARE(provider->diskCache()->regionUsage(2).tiles, 0LL);
    QVERIFY(provider->diskCache()->regionUsage(1).tiles == total);
  }

  void unreachableServerPausesTheDownload()
  {
    // The server can't be reached (it hangs up on every request): after a
    // few tiles in a row fail, the download pauses itself; when the server
    // is back, resume fetches the failed tiles too.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server([](const ts::test::MockHttpRequest&) {
      ts::test::MockHttpResponse response;
      response.hangUp = true;
      return response;
    });
    const QUrl url = server.url(QStringLiteral("/wms"));
    QTemporaryDir dir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = dir.path();
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(url), std::make_shared<ts::NetworkClient>(testNetwork()),
            settings);
    const ts::RegionGeometry area =
        ts::RegionGeometry::rectangle(-20.0, 30.0, 20.0, 60.0);
    QString error;
    ts::RegionDownload download(provider, 1, area, 0, 4);
    QVERIFY(download.start({}, &error));
    QVERIFY(download.waitUntilSettled(std::chrono::seconds(30)));
    ts::RegionDownloadProgress progress = download.progress();
    QCOMPARE(progress.state, State::Paused);
    QVERIFY(!progress.errorString.isEmpty());
    QVERIFY(progress.doneTiles < progress.totalTiles);

    server.setHandler(ts::test::mockWmsHandler(behaviour));
    download.resume();
    QVERIFY(download.waitUntilSettled(std::chrono::seconds(60)));
    progress = download.progress();
    QCOMPARE(progress.state, State::Finished);
    QCOMPARE(progress.failedTiles + progress.downloadedTiles,
             progress.totalTiles);
    // Tiles that failed only once were tried again.
    QVERIFY(progress.downloadedTiles > 0);
  }

  void estimateIsCloseToTheDownload()
  {
    // The size estimate from 32 sample tiles is within 30 % of what the
    // download stores (the agreed margin; see offline-regions.md).
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir dir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = dir.path();
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(server.url(QStringLiteral("/wms"))),
            std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    QString error;
    const ts::RegionGeometry isles = polygonOf(britishIsles());
    const ts::RegionGeometry wide =
        ts::RegionGeometry::rectangle(10.0, 20.0, 50.0, 50.0);
    int which = 0;
    for (const ts::RegionGeometry& area : {isles, wide}) {
      const int maxLevel = which == 0 ? 7 : 6;
      const ts::RegionEstimate estimate = ts::estimateRegionSize(
          *provider, area, 0, maxLevel, 32, notCancelled, 11);
      QCOMPARE(estimate.sampledTiles, 32);
      ts::RegionDownload download(provider, 10 + which, area, 0, maxLevel);
      QCOMPARE(estimate.tiles, download.progress().totalTiles);
      QVERIFY(download.start({}, &error));
      QVERIFY(download.waitUntilSettled(std::chrono::seconds(120)));
      const ts::RegionDownloadProgress progress = download.progress();
      QCOMPARE(progress.state, State::Finished);
      // The samples were kept: not fetched twice.
      QVERIFY(progress.cachedTiles >= 32);
      const double ratio = static_cast<double>(estimate.bytes) /
                           static_cast<double>(progress.bytes);
      qInfo("estimate %lld bytes, download %lld bytes (%.2f)", estimate.bytes,
            progress.bytes, ratio);
      QVERIFY2(ratio > 0.7 && ratio < 1.3, qPrintable(QString::number(ratio)));
      ++which;
    }

    // Without a server: a guess from the format.
    MockHttpServer gone;
    ts::WmsSource nowhere = mockSource(gone.url(QStringLiteral("/wms")));
    nowhere.format = QStringLiteral("image/jpeg");
    ts::WmsProviderSettings noCache;
    ts::WmsImageryProvider offline(
        nowhere, std::make_shared<ts::NetworkClient>(testNetwork()), noCache);
    const ts::RegionEstimate guess =
        ts::estimateRegionSize(offline, wide, 0, 3, 8, notCancelled);
    QCOMPARE(guess.sampledTiles, 0);
    QCOMPARE(guess.bytes, guess.tiles * ts::typicalTileBytes(offline));
  }

  void webMercatorSourcesSkipThePoles()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    ts::WmsSource source = mockSource(server.url(QStringLiteral("/wms")));
    source.crs = QStringLiteral("EPSG:3857");
    ts::WmsImageryProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), {});
    const std::optional<ts::GeoRect> bounds = provider.dataBounds();
    QVERIFY(bounds);
    QVERIFY(std::abs(bounds.value_or(ts::GeoRect{}).northDeg - 85.0511) < 1e-3);
    // Beyond Web Mercator's limit there are no tiles.
    const ts::RegionGeometry arctic =
        ts::RegionGeometry::rectangle(-180.0, 86.0, 180.0, 90.0);
    QCOMPARE(ts::countRegionTiles(arctic, 0, 8, bounds).total(), 0LL);
    // No disk cache: a region can't be kept.
    const ts::RegionTileResult result =
        provider.fetchForRegion(ts::TileKey{2, 3, 1}, 1, notCancelled);
    QCOMPARE(result.outcome, ts::RegionTileResult::Outcome::Failed);
    QVERIFY(!result.errorString.isEmpty());
  }

  void serverErrorsAndNoStore()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir dir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = dir.path();
    ts::WmsImageryProvider provider(
        mockSource(server.url(QStringLiteral("/wms"))),
        std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    using Outcome = ts::RegionTileResult::Outcome;
    behaviour->serviceException = true;
    ts::RegionTileResult result =
        provider.fetchForRegion(ts::TileKey{3, 1, 1}, 1, notCancelled);
    QCOMPARE(result.outcome, Outcome::Failed);
    QVERIFY(!result.unreachable);
    behaviour->serviceException = false;
    behaviour->noStore = true;
    result = provider.fetchForRegion(ts::TileKey{3, 1, 1}, 1, notCancelled);
    QCOMPARE(result.outcome, Outcome::Failed);
    QVERIFY(result.errorString.contains(QStringLiteral("no-store")));
    QCOMPARE(provider.diskCache()->count(), 0);
    // A stale tile is revalidated.
    behaviour->noStore = false;
    behaviour->etag = true;
    behaviour->maxAgeS = 0;
    QCOMPARE(
        provider.fetchForRegion(ts::TileKey{3, 1, 1}, 1, notCancelled).outcome,
        Outcome::Downloaded);
    QCOMPARE(
        provider.fetchForRegion(ts::TileKey{3, 1, 1}, 1, notCancelled).outcome,
        Outcome::Revalidated);
    QVERIFY(provider.diskCache()->isPinned(ts::TileKey{3, 1, 1}));
    // Outside the source: no data.
    ts::WmsSource bounded = mockSource(server.url(QStringLiteral("/wms")));
    bounded.bounds = ts::GeoRect{0.0, 0.0, 1.0, 1.0};
    ts::WmsImageryProvider small(
        bounded, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    QCOMPARE(
        small.fetchForRegion(ts::TileKey{3, 1, 1}, 1, notCancelled).outcome,
        Outcome::NoData);
  }
};

QTEST_GUILESS_MAIN(TestRegions)
#include "tst_regions.moc"
