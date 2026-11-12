// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <gdal.h>
#define GEOS_USE_ONLY_R_API
#include <geos_c.h>
#include <ogr_api.h>

#include "imagerytiler.h"
#include "packbuilder.h"
#include "shorelinebuilder.h"
#include "ts/deps/dependencies.h"
#include "ts/deps/geoscontext.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/regions/countryareas.h"
#include "ts/tiling/tilekey.h"

namespace
{

const std::atomic<bool> notCancelled{false};

/** One fixture polygon (a single ring of lon, lat pairs). */
struct FixturePolygon {
  QString id;
  std::vector<double> ring;
  double areaKm2 = 0.0;
};

std::vector<double> box(double west, double south, double east, double north)
{
  return {west, south, east, south, east, north, west, north};
}

/** Writes GSHHS_<resolution>_L<level>.shp with GSHHG's fields. */
bool writeShapefile(const QString& path, int level,
                    const std::vector<FixturePolygon>& polygons)
{
  ts::initialiseGdal();
  GDALDriverH driver = GDALGetDriverByName("ESRI Shapefile");
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALCreate(driver, encoded.constData(), 0, 0, 0, GDT_Unknown, nullptr);
  if (dataset == nullptr) {
    return false;
  }
  OGRLayerH layer =
      GDALDatasetCreateLayer(dataset, "polygons", nullptr, wkbPolygon, nullptr);
  OGRFieldDefnH idField = OGR_Fld_Create("id", OFTString);
  OGRFieldDefnH levelField = OGR_Fld_Create("level", OFTInteger);
  OGRFieldDefnH areaField = OGR_Fld_Create("area", OFTReal);
  OGR_L_CreateField(layer, idField, TRUE);
  OGR_L_CreateField(layer, levelField, TRUE);
  OGR_L_CreateField(layer, areaField, TRUE);
  OGR_Fld_Destroy(idField);
  OGR_Fld_Destroy(levelField);
  OGR_Fld_Destroy(areaField);
  bool ok = true;
  for (const FixturePolygon& polygon : polygons) {
    OGRGeometryH ring = OGR_G_CreateGeometry(wkbLinearRing);
    for (std::size_t i = 0; i + 1 < polygon.ring.size(); i += 2) {
      OGR_G_AddPoint_2D(ring, polygon.ring[i], polygon.ring[i + 1]);
    }
    OGR_G_AddPoint_2D(ring, polygon.ring[0], polygon.ring[1]);
    OGRGeometryH geometry = OGR_G_CreateGeometry(wkbPolygon);
    OGR_G_AddGeometryDirectly(geometry, ring);
    OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(layer));
    const QByteArray id = polygon.id.toUtf8();
    OGR_F_SetFieldString(feature, 0, id.constData());
    OGR_F_SetFieldInteger(feature, 1, level);
    OGR_F_SetFieldDouble(feature, 2, polygon.areaKm2);
    OGR_F_SetGeometryDirectly(feature, geometry);
    ok = ok && OGR_L_CreateFeature(layer, feature) == OGRERR_NONE;
    OGR_F_Destroy(feature);
  }
  GDALClose(dataset);
  return ok;
}

double areaOf(const std::vector<double>& ring)
{
  return ts::packager::rhumbRingAreaKm2(ring);
}

/** The fixture world, one shapefile set per resolution in @p root. */
struct Fixture {
  /** Expected land area: L1 + L5 − L2 + L3, km². */
  double landAreaKm2 = 0.0;
};

Fixture writeFixture(const QString& root, const QStringList& resolutions)
{
  // An island across the prime meridian and the equator, with a lake
  // (also across the meridian) holding an islet.
  const FixturePolygon island{QStringLiteral("1"), box(-30, -20, 30, 40),
                              areaOf(box(-30, -20, 30, 40))};
  const FixturePolygon lake{QStringLiteral("2"), box(-5, 0, 5, 10),
                            areaOf(box(-5, 0, 5, 10))};
  const FixturePolygon islet{QStringLiteral("3"), box(-1, 4, 1, 6),
                             areaOf(box(-1, 4, 1, 6))};
  // A polygon split at the antimeridian: both halves carry the whole area.
  const double splitArea =
      areaOf(box(170, 50, 180, 60)) + areaOf(box(-180, 50, -170, 60));
  const FixturePolygon east{QStringLiteral("9-E"), box(170, 50, 180, 60),
                            splitArea};
  const FixturePolygon west{QStringLiteral("9-W"), box(-180, 50, -170, 60),
                            splitArea};
  // Antarctica: down to the pole, edges along ±180° and −90°.
  const std::vector<double> capRing = {-180, -70, 180,  -70,
                                       180,  -90, -180, -90};
  const FixturePolygon cap{QStringLiteral("4"), capRing, areaOf(capRing)};

  for (const QString& resolution : resolutions) {
    const QDir dir(root + QStringLiteral("/GSHHS_shp/") + resolution);
    QDir().mkpath(dir.path());
    const QString prefix = QStringLiteral("GSHHS_%1_L").arg(resolution);
    writeShapefile(dir.filePath(prefix + QStringLiteral("1.shp")), 1,
                   {island, east, west});
    writeShapefile(dir.filePath(prefix + QStringLiteral("2.shp")), 2, {lake});
    writeShapefile(dir.filePath(prefix + QStringLiteral("3.shp")), 3, {islet});
    writeShapefile(dir.filePath(prefix + QStringLiteral("5.shp")), 5, {cap});
  }
  Fixture fixture;
  fixture.landAreaKm2 =
      island.areaKm2 + splitArea + cap.areaKm2 - lake.areaKm2 + islet.areaKm2;
  return fixture;
}

/** Even-odd point in ring test in longitude/latitude. */
bool inRing(const std::vector<ts::GeoPoint>& ring, double lon, double lat)
{
  bool inside = false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const ts::GeoPoint& a = ring[i];
    const ts::GeoPoint& b = ring[j];
    if ((a.latDeg > lat) != (b.latDeg > lat) &&
        lon < (b.lonDeg - a.lonDeg) * (lat - a.latDeg) / (b.latDeg - a.latDeg) +
                  a.lonDeg) {
      inside = !inside;
    }
  }
  return inside;
}

bool isLand(ts::FeatureProvider& provider, int level, double lon, double lat)
{
  const ts::TileKey key =
      ts::TileKey::containing(level, ts::Geodetic{lat, lon, 0.0});
  const std::optional<ts::FeatureTile> tile =
      provider.loadFeatures(key, notCancelled);
  bool land = false;
  for (const ts::Feature& feature :
       tile ? tile->features : std::vector<ts::Feature>()) {
    bool inside = !feature.parts.empty() && inRing(feature.parts[0], lon, lat);
    for (std::size_t hole = 1; hole < feature.parts.size(); ++hole) {
      inside = inside && !inRing(feature.parts[hole], lon, lat);
    }
    land = land || inside;
  }
  return land;
}

/** True if every ring of @p feature is closed and inside @p rect. */
bool ringsClosedAndInside(const ts::Feature& feature, const ts::GeoRect& rect)
{
  constexpr double slackDeg = 1e-9;
  for (const std::vector<ts::GeoPoint>& ring : feature.parts) {
    if (ring.size() < 4 || ring.front().lonDeg != ring.back().lonDeg ||
        ring.front().latDeg != ring.back().latDeg) {
      return false;
    }
    for (const ts::GeoPoint& point : ring) {
      if (point.lonDeg < rect.westDeg - slackDeg ||
          point.lonDeg > rect.eastDeg + slackDeg ||
          point.latDeg < rect.southDeg - slackDeg ||
          point.latDeg > rect.northDeg + slackDeg) {
        return false;
      }
    }
  }
  return !feature.parts.empty();
}

/** True if GEOS finds the polygon @p feature valid. */
bool isValidPolygon(GEOSContextHandle_t context, const ts::Feature& feature)
{
  std::vector<GEOSGeometry*> rings;
  for (const std::vector<ts::GeoPoint>& ring : feature.parts) {
    GEOSCoordSequence* sequence = GEOSCoordSeq_create_r(
        context, static_cast<unsigned int>(ring.size()), 2);
    for (std::size_t i = 0; i < ring.size(); ++i) {
      GEOSCoordSeq_setXY_r(context, sequence, static_cast<unsigned int>(i),
                           ring[i].lonDeg, ring[i].latDeg);
    }
    rings.push_back(GEOSGeom_createLinearRing_r(context, sequence));
  }
  GEOSGeometry* polygon =
      GEOSGeom_createPolygon_r(context, rings.front(), rings.data() + 1,
                               static_cast<unsigned int>(rings.size() - 1));
  const bool valid = GEOSisValid_r(context, polygon) == 1;
  GEOSGeom_destroy_r(context, polygon);
  return valid;
}

/**
 * Checks every polygon of every stored tile of @p table: closed rings,
 * inside the tile, valid for GEOS. Returns the number of polygons, or −1.
 */
int checkPolygons(const QString& path, const ts::FeatureTableSpec& table)
{
  ts::GeoPackageFeatureProvider provider(path);
  const ts::GeosContext geos;
  GEOSContextHandle_t context = static_cast<GEOSContextHandle_t>(geos.get());
  int polygons = 0;
  const int level = table.storageLevel;
  for (int y = 0; y < ts::TileKey::rows(level); ++y) {
    for (int x = 0; x < ts::TileKey::columns(level); ++x) {
      const ts::TileKey key{level, x, y};
      const std::optional<ts::FeatureTile> tile =
          provider.loadFeatures(key, notCancelled);
      for (const ts::Feature& feature :
           tile ? tile->features : std::vector<ts::Feature>()) {
        if (feature.attributes.value(QStringLiteral("table")).toString() !=
            table.name) {
          continue;
        }
        if (!ringsClosedAndInside(feature, key.bounds()) ||
            !isValidPolygon(context, feature)) {
          qWarning() << "Bad polygon in" << key.toString();
          return -1;
        }
        polygons += 1;
      }
    }
  }
  return polygons;
}

/**
 * A smooth test world that wraps around like a real one: red rises from
 * 180°W to 0° and falls back to 180°E, green follows latitude.
 */
QImage gradientWorld(int width, int height)
{
  QImage image(width, height, QImage::Format_RGB888);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int fromEdge = std::min(x, width - 1 - x);
      image.setPixelColor(
          x, y,
          QColor(fromEdge * 255 / (width / 2), y * 255 / (height - 1), 128));
    }
  }
  return image;
}

/**
 * A world with steep detail in red: a triangle wave rising and falling 8
 * per pixel across longitude, so a misplaced pixel shows.
 */
QImage waveWorld(int width, int height)
{
  QImage image(width, height, QImage::Format_RGB888);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      image.setPixelColor(x, y, QColor(std::abs(x % 64 - 32) * 7, y, 64));
    }
  }
  return image;
}

/** Halves @p image with a 2 × 2 box filter, as the tiler does. */
QImage halved(const QImage& image)
{
  QImage result(image.width() / 2, image.height() / 2, QImage::Format_RGB888);
  for (int y = 0; y < result.height(); ++y) {
    for (int x = 0; x < result.width(); ++x) {
      std::array<int, 3> sums = {0, 0, 0};
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const QColor color = image.pixelColor(2 * x + dx, 2 * y + dy);
          sums.at(0) += color.red();
          sums.at(1) += color.green();
          sums.at(2) += color.blue();
        }
      }
      result.setPixelColor(x, y,
                           QColor((sums.at(0) + 2) / 4, (sums.at(1) + 2) / 4,
                                  (sums.at(2) + 2) / 4));
    }
  }
  return result;
}

/** Mean absolute difference of the red channels of two images. */
double redDifference(const QImage& a, const QImage& b)
{
  double sum = 0.0;
  for (int y = 0; y < a.height(); ++y) {
    for (int x = 0; x < a.width(); ++x) {
      sum += std::abs(a.pixelColor(x, y).red() - b.pixelColor(x, y).red());
    }
  }
  return sum / (a.width() * a.height());
}

/** Mean absolute difference of column @p a of @p left and @p b of @p right. */
double columnDifference(const QImage& left, int a, const QImage& right, int b)
{
  double sum = 0.0;
  for (int y = 0; y < left.height(); ++y) {
    const QColor p = left.pixelColor(a, y);
    const QColor q = right.pixelColor(b, y);
    sum += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) +
           std::abs(p.blue() - q.blue());
  }
  return sum / (3.0 * left.height());
}

/** Mean absolute difference of row @p a of @p top and @p b of @p bottom. */
double rowDifference(const QImage& top, int a, const QImage& bottom, int b)
{
  double sum = 0.0;
  for (int x = 0; x < top.width(); ++x) {
    const QColor p = top.pixelColor(x, a);
    const QColor q = bottom.pixelColor(x, b);
    sum += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) +
           std::abs(p.blue() - q.blue());
  }
  return sum / (3.0 * top.width());
}

/** Index of tile (@p x, @p y) in a row-major list @p columns wide. */
std::size_t tileIndex(int x, int y, int columns)
{
  return static_cast<std::size_t>(y) * static_cast<std::size_t>(columns) +
         static_cast<std::size_t>(x);
}

/**
 * Checks that every tile of levels 0 to @p maxLevel exists and decodes,
 * and that tiles line up with their east and south neighbours (east
 * across the antimeridian too): the step across a tile edge is no larger
 * than the steps inside tiles next to it, on average.
 */
bool checkPyramid(ts::SqliteTileImageryProvider& provider, int maxLevel)
{
  const int size = provider.tileSizePx();
  for (int level = 0; level <= maxLevel; ++level) {
    const int columns = static_cast<int>(ts::TileKey::columns(level));
    const int rows = static_cast<int>(ts::TileKey::rows(level));
    std::vector<QImage> tiles;
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < columns; ++x) {
        const QImage tile =
            provider.loadImage(ts::TileKey{level, x, y}, notCancelled);
        if (tile.isNull() || tile.width() != size || tile.height() != size) {
          qWarning() << "Missing tile" << level << x << y;
          return false;
        }
        tiles.push_back(tile.convertToFormat(QImage::Format_RGB888));
      }
    }
    double across = 0.0;
    double inside = 0.0;
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < columns; ++x) {
        const QImage& tile = tiles.at(tileIndex(x, y, columns));
        const QImage& east = tiles.at(tileIndex((x + 1) % columns, y, columns));
        across += columnDifference(tile, size - 1, east, 0);
        inside += columnDifference(tile, size - 2, tile, size - 1) +
                  columnDifference(east, 0, east, 1);
        if (y + 1 < rows) {
          const QImage& south = tiles.at(tileIndex(x, y + 1, columns));
          across += rowDifference(tile, size - 1, south, 0);
          inside += rowDifference(tile, size - 2, tile, size - 1) +
                    rowDifference(south, 0, south, 1);
        }
      }
    }
    // inside counts two steps per edge.
    if (across > inside / 2.0 * 1.5 + 2.0 * columns * rows) {
      qWarning() << "Level" << level << "edges do not line up:" << across
                 << "across," << inside / 2.0 << "inside";
      return false;
    }
  }
  return true;
}

} // namespace

namespace
{

/**
 * Checks the real country outlines in @p path: places inside and outside
 * known countries, and that counting a big country's tiles stays quick.
 */
void checkCountries(const QString& path)
{
  const ts::CountryAreas areas(path);
  QVERIFY2(areas.isValid(), qPrintable(areas.errorString()));
  QVERIFY(areas.countries().size() > 200);
  struct Place {
    const char* country;
    double latDeg;
    double lonDeg;
    bool inside;
  };
  const std::array<Place, 14> places{{
      {"GBR", 51.5, -0.1, true},     // London
      {"GBR", 54.6, -5.93, true},    // Belfast
      {"GBR", 60.15, -1.15, true},   // Lerwick, Shetland
      {"GBR", 48.86, 2.35, false},   // Paris
      {"FRA", 48.86, 2.35, true},    //
      {"FRA", -21.1, 55.5, true},    // Réunion
      {"FRA", 4.9, -52.3, true},     // Cayenne, French Guiana
      {"RUS", 55.75, 37.6, true},    // Moscow
      {"RUS", 66.0, -175.0, true},   // Chukotka, east of 180°
      {"FJI", -18.14, 178.44, true}, // Suva
      {"FJI", -16.8, -179.9, true},  // Taveuni's east, across 180°
      {"ZAF", -29.6, 28.2, false},   // Lesotho, inside South Africa
      {"USA", 21.3, -157.85, true},  // Honolulu
      {"USA", 52.0, 177.5, true},    // Aleutians, west of 180°
  }};
  for (const Place& place : places) {
    QString error;
    const std::optional<ts::RegionGeometry> area =
        areas.area(QString::fromLatin1(place.country), &error);
    if (!area) {
      QFAIL(qPrintable(error));
    }
    QVERIFY2(area->contains(place.latDeg, place.lonDeg) == place.inside,
             qPrintable(QStringLiteral("%1 at %2, %3")
                            .arg(QLatin1String(place.country))
                            .arg(place.latDeg)
                            .arg(place.lonDeg)));
  }
  for (const char* id : {"GBR", "CAN", "RUS", "IDN"}) {
    QString error;
    const std::optional<ts::RegionGeometry> area =
        areas.area(QString::fromLatin1(id), &error);
    if (!area) {
      QFAIL(qPrintable(error));
    }
    // For the log (timings would make the test flaky): the whole count to
    // level 12, and to level 16 stopped past a million tiles, as the New
    // Region dialog counts.
    QElapsedTimer timer;
    timer.start();
    const long long tiles =
        ts::countRegionTiles(*area, 0, 12, std::nullopt).total();
    const qint64 fullMs = timer.restart();
    const ts::RegionTileCount capped =
        ts::countRegionTiles(*area, 0, 16, std::nullopt, 1000000);
    qInfo("%s: %zu rings; levels 0-12: %lld tiles in %lld ms; 0-16 up to a "
          "million: %lld tiles%s in %lld ms",
          id, area->rings().size(), tiles, fullMs, capped.total(),
          capped.complete ? "" : " (stopped)", timer.elapsed());
  }
}

} // namespace

/** Unit tests for the base-pack packager, and checks of a built pack. */
class TestPackager : public QObject
{
  Q_OBJECT

private slots:
  void rhumbArea()
  {
    // A 1° × 1° cell at the equator: 110.57 km × 111.32 km on WGS 84.
    QVERIFY(std::abs(areaOf(box(0, 0, 1, 1)) - 12308.5) < 1.0);
    // Closed or open rings, either orientation: the same area.
    QCOMPARE(areaOf({0, 0, 1, 0, 1, 1, 0, 1, 0, 0}), areaOf(box(0, 0, 1, 1)));
    QCOMPARE(areaOf({0, 0, 0, 1, 1, 1, 1, 0}), areaOf(box(0, 0, 1, 1)));
    // The cap south of 70°S, (1 − sin 70°) / 2 of the Earth's 510 million
    // km²: about 15.4 million. Its edge along 70°S spans 360°, and the edges
    // along −90° add nothing.
    const double cap = areaOf({-180, -70, 180, -70, 180, -90, -180, -90});
    QVERIFY2(cap > 15.2e6 && cap < 15.6e6, qPrintable(QString::number(cap)));
  }

  void poleClosure()
  {
    using ts::packager::closeHalfAtPole;
    // Antarctica's eastern half as GSHHG gives it at high resolution:
    // coast from 0° to 180°, closed along 180°, −90° and 0°.
    const std::vector<double> good = {180, -78, 180, -90, 0,   -90, 0,
                                      -70, 90,  -66, 179, -78, 180, -78};
    const double area = areaOf(good);
    QVERIFY(area > 5e6);
    QCOMPARE(areaOf(closeHalfAtPole(good)), area);
    // ... and at full resolution: a spike at 0° where the closure should
    // be, so the last edge cuts back from 180° to 0°.
    const std::vector<double> broken = {0,  -70, 0,   -90, 0,   -90, 0, -70,
                                        90, -66, 179, -78, 180, -78, 0, -70};
    QVERIFY(areaOf(broken) < area / 2);
    QCOMPARE(areaOf(closeHalfAtPole(broken)), area);
    // The western half ending 0.002° short of 0°: snapped onto it.
    const std::vector<double> west = {-0.002, -70, -0.002, -90, -180,   -90,
                                      -180,   -78, -90,    -66, -0.002, -70};
    const std::vector<double> closed = closeHalfAtPole(west);
    QCOMPARE(areaOf(closed),
             areaOf({0, -70, 0, -90, -180, -90, -180, -78, -90, -66}));
    for (std::size_t i = 0; i < closed.size(); i += 2) {
      QVERIFY(closed[i] == 0.0 || closed[i] <= -90.0);
    }
    // Rings not reaching the pole, or without one edge across the half,
    // are kept.
    const std::vector<double> island = box(10, 10, 20, 20);
    QCOMPARE(closeHalfAtPole(island), island);
    const std::vector<double> sliver = {0, -70, 0, -90, 0, -71};
    QCOMPARE(closeHalfAtPole(sliver), sliver);
  }

  void shoreline()
  {
    const QTemporaryDir dir;
    const Fixture fixture =
        writeFixture(dir.path(), {QStringLiteral("c"), QStringLiteral("i")});
    const QString path = dir.filePath(QStringLiteral("shoreline.gpkg"));
    ts::GeoPackageFeatureWriter writer(path);
    ts::packager::ShorelineOptions options;
    options.shapefileDirectory = dir.filePath(QStringLiteral("GSHHS_shp"));
    options.lods = {{QStringLiteral("c"), 0, 0, 2},
                    {QStringLiteral("i"), 3, 3, -1}};
    std::vector<ts::packager::ShorelineLodStats> stats;
    QString error;
    QVERIFY2(ts::packager::buildShoreline(options, &writer, &stats, &error),
             qPrintable(error));
    QVERIFY2(writer.finish(), qPrintable(writer.errorString()));
    QCOMPARE(stats.size(), std::size_t{2});
    for (const ts::packager::ShorelineLodStats& lod : stats) {
      // Cutting and merging keep the land, and GSHHG's fields (the split
      // polygon counted once) agree with the geometry.
      QVERIFY(std::abs(lod.sourceAreaKm2 / fixture.landAreaKm2 - 1.0) < 1e-9);
      QVERIFY(std::abs(lod.gshhgAreaKm2 / fixture.landAreaKm2 - 1.0) < 1e-9);
      QVERIFY2(std::abs(lod.landAreaKm2 / lod.sourceAreaKm2 - 1.0) <
                   ts::packager::areaTolerance / 100.0,
               qPrintable(QString::number(lod.landAreaKm2)));
      QCOMPARE(lod.sourcePolygons, 6);
    }
    // The coarse level has both level-0 tiles; the fine one only tiles
    // with land.
    QCOMPARE(stats[0].landTiles, 2);
    QVERIFY(stats[1].landTiles > 10 &&
            stats[1].landTiles < ts::TileKey::columns(3) * 4);

    ts::GeoPackageFeatureProvider provider(path);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    QCOMPARE(provider.maximumLevel(), 3);
    for (const ts::FeatureTableSpec& table : provider.tables()) {
      QVERIFY(checkPolygons(path, table) > 0);
    }
    for (const int level : {0, 2, 3, 5}) {
      QVERIFY(isLand(provider, level, -20.0, 30.0));  // the island
      QVERIFY(!isLand(provider, level, 3.0, 2.0));    // its lake
      QVERIFY(isLand(provider, level, 0.5, 5.0));     // the islet
      QVERIFY(!isLand(provider, level, 60.0, 0.0));   // sea
      QVERIFY(isLand(provider, level, 175.0, 55.0));  // east of 180°
      QVERIFY(isLand(provider, level, -175.0, 55.0)); // west of 180°
      QVERIFY(isLand(provider, level, 10.0, -89.9));  // by the pole
    }
  }

  void tableNames()
  {
    // One resolution at two storage levels: the second table is told
    // apart by its level.
    const std::vector<ts::packager::ShorelineLod> lods{
        {QStringLiteral("c"), 0, 0, 3},
        {QStringLiteral("i"), 4, 4, 5},
        {QStringLiteral("i"), 6, 6, -1}};
    QCOMPARE(ts::packager::shorelineTableName(lods, 0),
             QStringLiteral("land_c"));
    QCOMPARE(ts::packager::shorelineTableName(lods, 1),
             QStringLiteral("land_i"));
    QCOMPARE(ts::packager::shorelineTableName(lods, 2),
             QStringLiteral("land_i_6"));
  }

  void minimumArea()
  {
    const QTemporaryDir dir;
    writeFixture(dir.path(), {QStringLiteral("c")});
    ts::GeoPackageFeatureWriter writer(
        dir.filePath(QStringLiteral("shoreline.gpkg")));
    ts::packager::ShorelineOptions options;
    options.shapefileDirectory = dir.filePath(QStringLiteral("GSHHS_shp"));
    options.lods = {{QStringLiteral("c"), 0, 0, -1}};
    // Drops the islet (4.9e4 km²) and nothing larger.
    options.minimumAreaKm2 = 50000.0;
    std::vector<ts::packager::ShorelineLodStats> stats;
    QString error;
    QVERIFY(ts::packager::buildShoreline(options, &writer, &stats, &error));
    QVERIFY2(
        stats.size() == 1 && stats[0].sourcePolygons == 5,
        qPrintable(error + QString::number(
                               stats.empty() ? -1 : stats[0].sourcePolygons)));
  }

  void imageryTiles()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tiles.sqlite"));
    ts::packager::ImageryTilerOptions options;
    options.maxLevel = 2;
    options.tileSizePx = 32;
    options.jpegQuality = 90;
    ts::SqliteTileStoreInfo info;
    info.tileSizePx = 32;
    info.maxLevel = 2;
    ts::packager::ImageryTilerStats stats;
    QString error;
    {
      ts::SqliteTileStoreWriter writer(path, info);
      // Wrong mosaic size: refused.
      QVERIFY(!ts::packager::tileMosaic(gradientWorld(100, 50), options,
                                        &writer, &stats, &error));
      QVERIFY(ts::packager::tileMosaic(waveWorld(256, 128), options, &writer,
                                       &stats, &error));
      QVERIFY(writer.finish());
    }
    QCOMPARE(stats.tiles, 2 + 8 + 32);
    ts::SqliteTileImageryProvider provider(path);
    QVERIFY(provider.isValid());
    // Every tile is its exact square of the mosaic (halved per level): a
    // tile one pixel off would differ by the wave's 7 per pixel.
    QImage level = waveWorld(256, 128);
    for (int depth = 2; depth >= 0; --depth) {
      for (int y = 0; y < ts::TileKey::rows(depth); ++y) {
        for (int x = 0; x < ts::TileKey::columns(depth); ++x) {
          const QImage tile =
              provider.loadImage(ts::TileKey{depth, x, y}, notCancelled);
          QVERIFY(!tile.isNull());
          const QImage expected = level.copy(x * 32, y * 32, 32, 32);
          const QImage shifted = level.copy(x * 32 + 1, y * 32, 32, 32);
          const double tileError = redDifference(tile, expected);
          const double shiftedError = redDifference(tile, shifted);
          QVERIFY2(tileError < 4.0, qPrintable(QString::number(tileError)));
          QVERIFY(shiftedError > 2.0 * tileError);
        }
      }
      level = halved(level);
    }
  }

  void readEquirectangular()
  {
    const QTemporaryDir dir;
    QImage world(400, 200, QImage::Format_RGB888);
    world.fill(Qt::red);
    for (int y = 0; y < 200; ++y) {
      for (int x = 200; x < 400; ++x) {
        world.setPixelColor(x, y, Qt::blue);
      }
    }
    const QString path = dir.filePath(QStringLiteral("world.png"));
    QVERIFY(world.save(path));
    QString error;
    const QImage mosaic =
        ts::packager::readEquirectangular(path, 64, 32, &error);
    QVERIFY2(!mosaic.isNull(), qPrintable(error));
    QCOMPARE(mosaic.size(), QSize(64, 32));
    QCOMPARE(mosaic.pixelColor(10, 10), QColor(Qt::red));
    QCOMPARE(mosaic.pixelColor(50, 10), QColor(Qt::blue));

    const QString square = dir.filePath(QStringLiteral("square.png"));
    QVERIFY(QImage(100, 100, QImage::Format_RGB888).save(square));
    QVERIFY(ts::packager::readEquirectangular(square, 64, 32, &error).isNull());
    QVERIFY(error.contains(QLatin1String("2:1")));
  }

  void buildPack()
  {
    const QTemporaryDir dir;
    const QDir root(dir.path());
    root.mkpath(QStringLiteral("sources/gshhg"));
    writeFixture(root.filePath(QStringLiteral("sources/gshhg")),
                 {QStringLiteral("c")});
    QFile licence(root.filePath(QStringLiteral("sources/gshhg/LICENSE.TXT")));
    QVERIFY(licence.open(QIODevice::WriteOnly));
    licence.write("Fixture licence\n");
    licence.close();
    QFile local(root.filePath(QStringLiteral("extra.txt")));
    QVERIFY(local.open(QIODevice::WriteOnly));
    local.write("Extra notice\n");
    local.close();
    QVERIFY(gradientWorld(400, 200).save(
        root.filePath(QStringLiteral("sources/world.png"))));

    const QByteArray configJson =
        "{"
        "\"id\": \"test-pack\", \"version\": \"3\","
        "\"sizeLimitBytes\": 20000000, \"reservedBytes\": 1000000,"
        "\"sources\": ["
        "{\"id\": \"gshhg\", \"file\": \"gshhg\", \"url\": "
        "\"https://example.org/g\","
        "\"sha256\": "
        "\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"name\": \"Fixture shores\", \"version\": \"1\","
        "\"notices\": [\"LICENSE.TXT\"], \"localNotices\": [\"extra.txt\"]},"
        "{\"id\": \"world\", \"file\": \"world.png\", \"url\": "
        "\"https://example.org/w\","
        "\"sha256\": "
        "\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"name\": \"Fixture world\", \"version\": \"1\"}"
        "],"
        "\"imagery\": {\"dataset\": \"bluemarble\", \"source\": \"world\", "
        "\"name\": \"World\", \"replaces\": \"bluemarble\","
        "\"licence\": \"CC0-1.0\", \"attribution\": \"Fixture\","
        "\"maxLevel\": 1, \"tileSize\": 64},"
        "\"shoreline\": {\"dataset\": \"shoreline\", \"source\": \"gshhg\", "
        "\"name\": \"Shores\","
        "\"licence\": \"LGPL-3.0-or-later\", \"attribution\": \"Fixture\","
        "\"lods\": [{\"resolution\": \"c\", \"storageLevel\": 1,"
        "\"minLevel\": 1}]}"
        "}";
    QString error;
    const std::optional<ts::packager::PackConfig> config =
        ts::packager::PackConfig::fromJson(
            QJsonDocument::fromJson(configJson).object(), &error);
    if (!config || !config->imagery || !config->shoreline) {
      QFAIL(qPrintable(error));
    }

    ts::packager::BuildOptions options;
    options.sourcesDirectory = root.filePath(QStringLiteral("sources"));
    options.configDirectory = root.path();
    options.outputDirectory = root.filePath(QStringLiteral("pack"));
    // The sources' checksums are placeholders: checking them fails.
    ts::packager::BuildReport report;
    QVERIFY(!ts::packager::buildPack(config.value(), options, &report, &error));
    QVERIFY(error.contains(QLatin1String("SHA-256")));

    options.verifySources = false;
    report = ts::packager::BuildReport();
    QVERIFY2(ts::packager::buildPack(config.value(), options, &report, &error),
             qPrintable(error));
    QCOMPARE(report.imagery.tiles, 2 + 8);

    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(options.outputDirectory, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(manifest.value().id, QStringLiteral("test-pack"));
    QVERIFY(manifest.value().verify(options.outputDirectory, notCancelled).ok);
    QCOMPARE(manifest.value().datasets.size(), std::size_t{2});
    QCOMPARE(manifest.value().notices.size(), std::size_t{2});
    // "replaces" reaches the manifest where it is set, and only there.
    for (const ts::PackDataset& dataset : manifest.value().datasets) {
      QCOMPARE(dataset.replaces, dataset.kind == ts::PackDataset::Kind::Imagery
                                     ? QStringLiteral("bluemarble")
                                     : QString());
    }
    qint64 listed = 0;
    for (const ts::PackFile& file : manifest.value().files) {
      listed += file.sizeBytes;
    }
    QVERIFY(listed < report.totalBytes); // the manifest is not listed
    const QDir pack(options.outputDirectory);
    QVERIFY(ts::SqliteTileImageryProvider(
                pack.filePath(QStringLiteral("bluemarble.sqlite")))
                .isValid());
    QVERIFY(ts::GeoPackageFeatureProvider(
                pack.filePath(QStringLiteral("shoreline.gpkg")))
                .isValid());
    QVERIFY(QFile::exists(
        pack.filePath(QStringLiteral("licences/gshhg/extra.txt"))));

    // A second build into the same, now full, directory is refused.
    QVERIFY(!ts::packager::buildPack(config.value(), options, &report, &error));

    // Over budget: the build fails and says so.
    ts::packager::PackConfig small = config.value();
    small.sizeLimitBytes = 1000;
    small.reservedBytes = 0;
    options.outputDirectory = root.filePath(QStringLiteral("small"));
    report = ts::packager::BuildReport();
    QVERIFY(!ts::packager::buildPack(small, options, &report, &error));
    QVERIFY(error.contains(QLatin1String("budget")));
  }

  void countries()
  {
    // Natural Earth's fields; a country with a lake, one split at the
    // antimeridian, one without ISO codes, and Antarctica (left out).
    const QTemporaryDir dir;
    const QString source = dir.filePath(QStringLiteral("countries.geojson"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"type": "FeatureCollection", "features": [
      {"type": "Feature", "properties": {"ADM0_A3": "ISL", "ADMIN": "Islandia",
        "ISO_A2_EH": "IS", "ISO_A3_EH": "ISL"},
       "geometry": {"type": "Polygon", "coordinates": [
         [[10, 40], [20, 40], [20, 50], [10, 50], [10, 40]],
         [[13, 43], [13, 47], [17, 47], [17, 43], [13, 43]]]}},
      {"type": "Feature", "properties": {"ADM0_A3": "SPL", "ADMIN": "Splitland",
        "ISO_A2_EH": "SP", "ISO_A3_EH": "SPL"},
       "geometry": {"type": "MultiPolygon", "coordinates": [
         [[[175, 60], [180, 60], [180, 65], [175, 65], [175, 60]]],
         [[[-180, 60], [-175, 60], [-175, 66], [-180, 66], [-180, 60]]]]}},
      {"type": "Feature", "properties": {"ADM0_A3": "ANO", "ADMIN": "Anonymia",
        "ISO_A2_EH": "-99", "ISO_A3_EH": "-99"},
       "geometry": {"type": "Polygon", "coordinates": [
         [[-60, -10], [-50, -10], [-50, 0], [-60, 0], [-60, -10]]]}},
      {"type": "Feature", "properties": {"ADM0_A3": "ATA", "ADMIN": "Antarctica",
        "ISO_A2_EH": "AQ", "ISO_A3_EH": "ATA"},
       "geometry": {"type": "Polygon", "coordinates": [
         [[-180, -90], [180, -90], [180, -70], [-180, -70], [-180, -90]]]}}
    ]})");
    file.close();
    ts::packager::CountryOptions options;
    options.sourcePath = source;
    options.outputPath = dir.filePath(QStringLiteral("countries.gpkg"));
    options.exclude = {QStringLiteral("ATA")};
    ts::packager::CountryStats stats;
    QString error;
    QVERIFY2(ts::packager::buildCountries(options, &stats, &error),
             qPrintable(error));
    QCOMPARE(stats.countries, 3);
    QCOMPARE(stats.excluded, 1);
    QCOMPARE(stats.rings, 4LL); // Splitland's halves joined; the lake

    const ts::CountryAreas areas(options.outputPath);
    QVERIFY2(areas.isValid(), qPrintable(areas.errorString()));
    QCOMPARE(areas.countries().size(), std::size_t{3});
    QCOMPARE(areas.countries().front().name, QStringLiteral("Anonymia"));
    QVERIFY(areas.countries().front().isoA2.isEmpty());
    QCOMPARE(
        areas.country(QStringLiteral("ISL")).value_or(ts::CountryArea{}).isoA3,
        QStringLiteral("ISL"));
    QVERIFY(!areas.country(QStringLiteral("ATA")));
    QVERIFY(!areas.area(QStringLiteral("ATA"), &error));

    // Widened by 0.05° and simplified within 0.02°: the whole country,
    // a little more, and the lake still a hole.
    const std::optional<ts::RegionGeometry> islandia =
        areas.area(QStringLiteral("ISL"), &error);
    if (!islandia) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(islandia->contains(41.0, 11.0));
    QVERIFY(islandia->contains(40.0, 10.0));  // a corner
    QVERIFY(islandia->contains(39.98, 15.0)); // just off the coast
    QVERIFY(!islandia->contains(39.9, 15.0));
    QVERIFY(!islandia->contains(45.0, 15.0)); // the lake
    const std::optional<ts::RegionGeometry> splitland =
        areas.area(QStringLiteral("SPL"), &error);
    if (!splitland) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(splitland->rings().size(), std::size_t{1});
    QVERIFY(splitland->contains(62.0, 179.99));
    QVERIFY(splitland->contains(62.0, -179.99));
    QVERIFY(splitland->contains(65.5, -176.0));
    QVERIFY(!splitland->contains(65.5, 176.0));

    // The buffer must exceed the simplification.
    options.outputPath = dir.filePath(QStringLiteral("bad.gpkg"));
    options.bufferDeg = 0.01;
    QVERIFY(!ts::packager::buildCountries(options, &stats, &error));
  }

  void configErrors()
  {
    QString error;
    QVERIFY(!ts::packager::PackConfig::fromJson(QJsonObject(), &error));
    QJsonObject json =
        QJsonDocument::fromJson(
            "{"
            "\"id\": \"p\", \"sizeLimitBytes\": 10,"
            "\"sources\": [{\"id\": \"s\", \"file\": \"../escape\", \"sha256\":"
            "\"0000000000000000000000000000000000000000000000000000000000000000"
            "\"}]"
            "}")
            .object();
    QVERIFY(!ts::packager::PackConfig::fromJson(json, &error));
    json = QJsonDocument::fromJson(
               "{"
               "\"id\": \"p\", \"sizeLimitBytes\": 10, \"sources\": [],"
               "\"shoreline\": {\"dataset\": \"s\", \"source\": \"none\","
               "\"lods\": [{\"resolution\": \"x\"}]}"
               "}")
               .object();
    QVERIFY(!ts::packager::PackConfig::fromJson(json, &error));
  }

  void basePack()
  {
    // The real base pack, when TS_BASEPACK_DIR names one (CI's base pack
    // job; locally after `make basepack`). An extra pack is checked the
    // same way with TS_PACK_CONFIG naming its configuration
    // (tools/packager/packs/*.json).
    const QString directory = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (directory.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QString configPath = qEnvironmentVariable("TS_PACK_CONFIG");
    if (configPath.isEmpty()) {
      configPath = QStringLiteral(TS_TEST_DATA_DIR
                                  "/../../tools/packager/basepack.json");
    }
    QString error;
    const std::optional<ts::packager::PackConfig> config =
        ts::packager::PackConfig::read(configPath, &error);
    if (!config) {
      QFAIL(qPrintable(error));
    }
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(directory, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(manifest.value().id, config.value().id);
    QCOMPARE(manifest.value().version, config.value().version);
    const ts::PackVerification verification =
        manifest.value().verify(directory, notCancelled);
    QVERIFY2(verification.ok, verification.problems.empty()
                                  ? ""
                                  : qPrintable(verification.problems[0]));

    // Within the configured budget (limit minus reserve).
    qint64 total =
        QFileInfo(QDir(directory).filePath(ts::PackManifest::fileName()))
            .size();
    for (const ts::PackFile& file : manifest.value().files) {
      total += file.sizeBytes;
    }
    QVERIFY2(total <=
                 config.value().sizeLimitBytes - config.value().reservedBytes,
             qPrintable(QString::number(total)));

    // Each dataset replaces what its configuration says (nothing in the
    // base pack).
    QCOMPARE(manifest.value().datasets.size(),
             std::size_t{config->imagery ? 1U : 0U} +
                 std::size_t{config->shoreline ? 1U : 0U} +
                 std::size_t{config->countries ? 1U : 0U});
    const QString imageryReplaces =
        config->imagery ? config->imagery->replaces : QString();
    const QString shorelineReplaces =
        config->shoreline ? config->shoreline->replaces : QString();
    for (const ts::PackDataset& dataset : manifest.value().datasets) {
      switch (dataset.kind) {
      case ts::PackDataset::Kind::Imagery:
        QCOMPARE(dataset.replaces, imageryReplaces);
        break;
      case ts::PackDataset::Kind::Features:
        QCOMPARE(dataset.replaces, shorelineReplaces);
        break;
      case ts::PackDataset::Kind::Areas:
        QVERIFY(dataset.replaces.isEmpty());
        break;
      }
    }

    const QDir pack(directory);
    if (config->imagery) {
      ts::SqliteTileImageryProvider imagery(
          pack.filePath(QStringLiteral("bluemarble.sqlite")));
      QVERIFY(imagery.isValid());
      QCOMPARE(imagery.maximumLevel(),
               config.value().imagery.value().tiling.maxLevel);
      QVERIFY(checkPyramid(imagery, imagery.maximumLevel()));
    }
    if (config->countries) {
      checkCountries(
          pack.filePath(config->countries->dataset + QStringLiteral(".gpkg")));
    }
    if (!config->shoreline) {
      return;
    }

    const QString shoreline = pack.filePath(QStringLiteral("shoreline.gpkg"));
    ts::GeoPackageFeatureProvider features(shoreline);
    QVERIFY(features.isValid());
    QCOMPARE(features.tables().size(),
             config.value().shoreline.value().lods.size());
    for (const ts::FeatureTableSpec& table : features.tables()) {
      const int polygons = checkPolygons(shoreline, table);
      QVERIFY2(polygons > 0, qPrintable(table.name));
    }
    // Spot checks at every level: London is land, the North Sea and the
    // middle of the Pacific are not, the Caspian Sea is a lake, the South
    // Pole is land (ice), Wrangel Island sits on the antimeridian.
    for (int level = 0; level <= 10; ++level) {
      QVERIFY(isLand(features, level, -0.1, 51.5));
      QVERIFY(!isLand(features, level, 3.0, 56.0));
      QVERIFY(!isLand(features, level, -150.0, 0.0));
      QVERIFY(!isLand(features, level, 51.0, 42.0));
      QVERIFY(isLand(features, level, 0.0, -89.5));
      QVERIFY(isLand(features, level, -179.5, 71.3) ||
              isLand(features, level, 179.5, 71.3));
    }
  }
};

QTEST_GUILESS_MAIN(TestPackager)
#include "tst_packager.moc"
