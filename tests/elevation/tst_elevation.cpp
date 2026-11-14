// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "concurrency.h"
#include "mockhttpserver.h"
#include "ts/deps/dependencies.h"
#include "ts/elevation/compositeelevationprovider.h"
#include "ts/elevation/elevationprofile.h"
#include "ts/elevation/gdalelevationprovider.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/heightgrid.h"
#include "ts/elevation/sqliteelevationprovider.h"
#include "ts/elevation/webterrainprovider.h"
#include "ts/network/networkclient.h"
#include "ts/packs/packlibrary.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/providerregistry.h"
#include "ts/providers/sqlitetileimageryprovider.h"

namespace
{

const std::atomic<bool> notCancelled{false};

/** The fixture geoid: N = lat / 2 + lon / 100 metres. */
double fixtureUndulation(double latDeg, double lonDeg)
{
  return latDeg / 2.0 + lonDeg / 100.0;
}

/**
 * Writes a global 1° geoid grid of fixtureUndulation() in PROJ's GeoTIFF
 * form (points on whole degrees, as PROJ's own grids).
 */
bool writeFixtureGeoid(const QString& path, double offsetM = 0.0)
{
  ts::initialiseGdal();
  const int width = 360;
  const int height = 181;
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALCreate(GDALGetDriverByName("GTiff"), encoded.constData(), width,
                 height, 1, GDT_Float32, nullptr);
  if (dataset == nullptr) {
    return false;
  }
  std::array<double, 6> transform{-180.5, 1.0, 0.0, 90.5, 0.0, -1.0};
  GDALSetGeoTransform(dataset, transform.data());
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(srs, "WGS84");
  char* wkt = nullptr;
  OSRExportToWkt(srs, &wkt);
  GDALSetProjection(dataset, wkt);
  CPLFree(wkt);
  OSRRelease(srs);
  GDALSetMetadataItem(dataset, "AREA_OR_POINT", "Point", nullptr);
  GDALSetMetadataItem(dataset, "TYPE", "VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL",
                      nullptr);
  std::vector<float> values(static_cast<std::size_t>(width) *
                            static_cast<std::size_t>(height));
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      values[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(column)] =
          static_cast<float>(fixtureUndulation(90.0 - row, -180.0 + column) +
                             offsetM);
    }
  }
  const bool ok =
      GDALRasterIO(GDALGetRasterBand(dataset, 1), GF_Write, 0, 0, width, height,
                   values.data(), width, height, GDT_Float32, 0, 0) == CE_None;
  GDALClose(dataset);
  return ok;
}

/**
 * Writes a regional 0.1° geoid grid over 45°–47° N, 7°–9° E (like
 * OSGM15's over Great Britain): fixtureUndulation() − 20 m.
 */
bool writeRegionalGeoid(const QString& path)
{
  ts::initialiseGdal();
  const int width = 21;
  const int height = 21;
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALCreate(GDALGetDriverByName("GTiff"), encoded.constData(), width,
                 height, 1, GDT_Float32, nullptr);
  if (dataset == nullptr) {
    return false;
  }
  std::array<double, 6> transform{6.95, 0.1, 0.0, 47.05, 0.0, -0.1};
  GDALSetGeoTransform(dataset, transform.data());
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(srs, "WGS84");
  GDALSetSpatialRef(dataset, srs);
  OSRRelease(srs);
  GDALSetMetadataItem(dataset, "AREA_OR_POINT", "Point", nullptr);
  GDALSetMetadataItem(dataset, "TYPE", "VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL",
                      nullptr);
  std::vector<float> values;
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      values.push_back(static_cast<float>(
          fixtureUndulation(47.0 - 0.1 * row, 7.0 + 0.1 * column) - 20.0));
    }
  }
  const bool ok =
      GDALRasterIO(GDALGetRasterBand(dataset, 1), GF_Write, 0, 0, width, height,
                   values.data(), width, height, GDT_Float32, 0, 0) == CE_None;
  GDALClose(dataset);
  return ok;
}

/** The fixture terrain: 1000 m + 10 m per degree east, metres. */
double fixtureHeight(double latDeg, double lonDeg)
{
  return 1000.0 + 10.0 * lonDeg + 0.0 * latDeg;
}

/** A heights store of fixtureHeight() at levels 0–2, 9 samples a side. */
// The file, then the datum it names.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool writeFixtureHeights(const QString& path, const QString& verticalDatum)
{
  ts::SqliteTileStoreInfo info;
  info.format = QStringLiteral("heights");
  info.tileSizePx = 9;
  info.maxLevel = 2;
  info.name = QStringLiteral("Fixture heights");
  info.verticalDatum = verticalDatum;
  ts::SqliteTileStoreWriter writer(path, info);
  for (int level = 0; level <= 2; ++level) {
    for (int y = 0; y < static_cast<int>(ts::TileKey::rows(level)); ++y) {
      for (int x = 0; x < static_cast<int>(ts::TileKey::columns(level)); ++x) {
        const ts::TileKey key{level, x, y};
        std::vector<float> heights;
        for (int row = 0; row < 9; ++row) {
          for (int column = 0; column < 9; ++column) {
            const ts::GeoPoint point =
                ts::heightSamplePoint(key, 9, row, column);
            heights.push_back(
                static_cast<float>(fixtureHeight(point.latDeg, point.lonDeg)));
          }
        }
        if (!writer.putTile(key, ts::encodeHeightGrid(heights, 9))) {
          return false;
        }
      }
    }
  }
  return writer.finish();
}

/** Heights of the fixture DEM files: 1000 m + 100 m/° east + 50 m/° north. */
double demHeight(double latDeg, double lonDeg)
{
  return 1000.0 + 100.0 * (lonDeg - 7.0) + 50.0 * (latDeg - 45.0);
}

/**
 * Writes a one-band Float32 GeoTIFF of @p width × @p height pixels in the
 * CRS @p crs (anything OSRSetFromUserInput takes), geotransform
 * @p transform, each pixel @p value(x, y) of its centre in CRS units.
 */
bool writeDem(const QString& path, const char* crs,
              const std::array<double, 6>& transform, int width, int height,
              const std::function<double(double, double)>& value,
              const double* noDataValue = nullptr)
{
  ts::initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALCreate(GDALGetDriverByName("GTiff"), encoded.constData(), width,
                 height, 1, GDT_Float32, nullptr);
  if (dataset == nullptr) {
    return false;
  }
  std::array<double, 6> copy = transform;
  GDALSetGeoTransform(dataset, copy.data());
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  OSRSetFromUserInput(srs, crs);
  GDALSetSpatialRef(dataset, srs);
  OSRRelease(srs);
  GDALRasterBandH band = GDALGetRasterBand(dataset, 1);
  if (noDataValue != nullptr) {
    GDALSetRasterNoDataValue(band, *noDataValue);
  }
  std::vector<float> values;
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const double x = transform[0] + transform[1] * (column + 0.5);
      const double y = transform[3] + transform[5] * (row + 0.5);
      values.push_back(static_cast<float>(value(x, y)));
    }
  }
  const bool ok =
      GDALRasterIO(band, GF_Write, 0, 0, width, height, values.data(), width,
                   height, GDT_Float32, 0, 0) == CE_None;
  GDALClose(dataset);
  return ok;
}

/** The fixture DEM: demHeight() at 0.01° over 7°–9° E, 45°–47° N. */
bool writeGeographicDem(const QString& path, const char* crs = "EPSG:4326",
                        const double* noDataValue = nullptr)
{
  return writeDem(
      path, crs, {7.0, 0.01, 0.0, 47.0, 0.0, -0.01}, 200, 200,
      [noDataValue](double lon, double lat) {
        // A no-data hole around 46.5° N, 7.5° E.
        if (noDataValue != nullptr && std::abs(lat - 46.5) < 0.05 &&
            std::abs(lon - 7.5) < 0.05) {
          return *noDataValue;
        }
        return demHeight(lat, lon);
      },
      noDataValue);
}

/** Sample (@p row, @p column) of @p tile. */
double heightAt(const ts::HeightTile& tile, int row, int column)
{
  return static_cast<double>(
      tile.heightsM[static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(tile.samples) +
                    static_cast<std::size_t>(column)]);
}

/** The web fixture's heights: linear in the zoom's global pixels. */
double webHeight(double globalX, double globalY)
{
  return 0.01 * globalX + 0.02 * globalY;
}

/** Terrarium PNG of web tile (@p zoom, @p x, @p y) of webHeight(). */
QByteArray terrariumTile(int x, int y)
{
  QImage image(256, 256, QImage::Format_ARGB32);
  for (int row = 0; row < 256; ++row) {
    for (int column = 0; column < 256; ++column) {
      const double height =
          webHeight(x * 256 + column + 0.5, y * 256 + row + 0.5) + 32768.0;
      const int whole = static_cast<int>(std::floor(height));
      const int fraction =
          static_cast<int>(std::lround((height - whole) * 256.0));
      image.setPixel(column, row,
                     qRgb(whole / 256, whole % 256, std::min(fraction, 255)));
    }
  }
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return png;
}

/** Network settings for the mock server: no proxy, quick retries. */
ts::NetworkSettings testNetwork()
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.retryDelay = std::chrono::milliseconds(20);
  settings.attempts = 2;
  settings.timeout = std::chrono::milliseconds(5000);
  return settings;
}

/**
 * Answers /z/x/y.png with terrariumTile(); 404 for x = 3 at zoom 2, 500
 * while @p failing is set.
 */
ts::test::MockHttpServer::Handler
terrariumHandler(const std::shared_ptr<std::atomic<bool>>& failing)
{
  return [failing](const ts::test::MockHttpRequest& request) {
    const QStringList parts =
        request.url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (failing->load()) {
      return ts::test::mockResponse(500, "broken");
    }
    if (parts.size() != 3) {
      return ts::test::mockResponse(404, "no");
    }
    const int zoom = parts[0].toInt();
    const int x = parts[1].toInt();
    const int y = QString(parts[2]).remove(QStringLiteral(".png")).toInt();
    if (zoom == 2 && x == 3) {
      return ts::test::mockResponse(404, "none here");
    }
    return ts::test::mockResponse(200, terrariumTile(x, y), "image/png");
  };
}

/** The global pixel coordinates of (@p latDeg, @p lonDeg) at @p zoom. */
// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::pair<double, double> webPixel(double latDeg, double lonDeg, int zoom)
{
  const double world = std::ldexp(256.0, zoom);
  const double lat = latDeg * 3.14159265358979323846 / 180.0;
  return {(lonDeg + 180.0) / 360.0 * world,
          (1.0 - std::asinh(std::tan(lat)) / 3.14159265358979323846) / 2.0 *
              world};
}

} // namespace

/** Unit tests for ts/elevation. */
class TestElevation : public QObject
{
  Q_OBJECT

private slots:
  void quantization()
  {
    // 2 m steps down to −100 m, 10 m steps below.
    QCOMPARE(ts::quantizedHeightM(1344.6), 1344.0);
    QCOMPARE(ts::quantizedHeightM(1345.2), 1346.0);
    QCOMPARE(ts::quantizedHeightM(-430.0), -430.0);
    QCOMPARE(ts::quantizedHeightM(-99.0), -100.0);
    QCOMPARE(ts::quantizedHeightM(-104.0), -100.0);
    QCOMPARE(ts::quantizedHeightM(-106.0), -110.0);
    QCOMPARE(ts::quantizedHeightM(-10994.0), -10990.0);
    for (int step = 0; step < 54000; ++step) {
      const double h = -11000.0 + 0.37 * step;
      QVERIFY(std::abs(ts::quantizedHeightM(h) - h) <=
              (h >= -100.0 ? 1.0 : 5.0));
    }
  }

  void codecRoundTrip()
  {
    // Mountains, a cliff and deep sea in one grid.
    const int samples = 65;
    std::vector<float> heights;
    for (int row = 0; row < samples; ++row) {
      for (int column = 0; column < samples; ++column) {
        double h = 3000.0 * std::sin(row * 0.2) * std::cos(column * 0.15);
        if (column > 40) {
          h -= 9000.0;
        }
        heights.push_back(static_cast<float>(h));
      }
    }
    const QByteArray encoded = ts::encodeHeightGrid(heights, samples);
    QVERIFY(!encoded.isEmpty());
    const std::optional<std::vector<float>> decoded =
        ts::decodeHeightGrid(encoded, samples);
    if (!decoded) {
      QFAIL("not decoded");
    }
    const std::vector<float> values = decoded.value_or(std::vector<float>());
    QCOMPARE(values.size(), heights.size());
    for (std::size_t i = 0; i < heights.size(); ++i) {
      QCOMPARE(values[i], static_cast<float>(ts::quantizedHeightM(
                              static_cast<double>(heights[i]))));
    }
    // Damaged or mismatched data is refused.
    QVERIFY(!ts::decodeHeightGrid(encoded, samples - 1));
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral("qUncompress")));
    QVERIFY(!ts::decodeHeightGrid(encoded.left(encoded.size() / 2), samples));
    QByteArray otherVersion = encoded;
    otherVersion[0] = 2;
    QVERIFY(!ts::decodeHeightGrid(otherVersion, samples));
    QVERIFY(!ts::decodeHeightGrid(QByteArray(), samples));
    QVERIFY(ts::encodeHeightGrid(heights, samples + 1).isEmpty());
  }

  void sampling()
  {
    // A plane is interpolated exactly, and resampled exactly.
    ts::HeightTile tile;
    tile.key = ts::TileKey{2, 5, 1};
    tile.samples = 5;
    for (int row = 0; row < 5; ++row) {
      for (int column = 0; column < 5; ++column) {
        const ts::GeoPoint point =
            ts::heightSamplePoint(tile.key, 5, row, column);
        tile.heightsM.push_back(
            static_cast<float>(fixtureHeight(point.latDeg, point.lonDeg)));
      }
    }
    const ts::GeoRect bounds = tile.key.bounds();
    QCOMPARE(ts::heightSamplePoint(tile.key, 5, 0, 0).latDeg, bounds.northDeg);
    QCOMPARE(ts::heightSamplePoint(tile.key, 5, 4, 4).lonDeg, bounds.eastDeg);
    const double lat = bounds.southDeg + 0.3 * bounds.heightDeg();
    const double lon = bounds.westDeg + 0.7 * bounds.widthDeg();
    QVERIFY(std::abs(ts::sampleHeightM(tile, lat, lon).value_or(0.0) -
                     fixtureHeight(lat, lon)) < 1e-3);
    QVERIFY(!ts::sampleHeightM(tile, bounds.northDeg + 1.0, lon));
    const ts::TileKey child = tile.key.child(3);
    const ts::HeightTile resampled = ts::resampleHeightTile(tile, child, 9);
    QCOMPARE(resampled.key, child);
    QCOMPARE(resampled.heightsM.size(), std::size_t{81});
    const ts::GeoPoint corner = ts::heightSamplePoint(child, 9, 8, 0);
    QVERIFY(std::abs(static_cast<double>(resampled.heightsM[72]) -
                     fixtureHeight(corner.latDeg, corner.lonDeg)) < 1e-3);
  }

  void geoidModel()
  {
    const QTemporaryDir dir(QDir::tempPath() +
                            QStringLiteral("/ts geoid XXXXXX"));
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("fixture.tif"))));
    // A directory with a space in its name: found through PROJ's path.
    const ts::GeoidModel model(dir.path(), QStringLiteral("fixture.tif"));
    QVERIFY2(model.isValid(), qPrintable(model.errorString()));
    QVERIFY(std::abs(model.undulationM(10.25, 20.5) -
                     fixtureUndulation(10.25, 20.5)) < 1e-4);
    QVERIFY(std::abs(model.undulationM(-60.0, -120.0) -
                     fixtureUndulation(-60.0, -120.0)) < 1e-4);
    const std::vector<double> many =
        model.undulationsM({ts::GeoPoint{1.0, 2.0}, ts::GeoPoint{45.5, -3.25}});
    QCOMPARE(many.size(), std::size_t{2});
    QVERIFY(std::abs(many[1] - fixtureUndulation(45.5, -3.25)) < 1e-4);

    QVERIFY(
        !ts::GeoidModel(dir.path(), QStringLiteral("missing.tif")).isValid());
    QVERIFY(!ts::GeoidModel(dir.path(), QStringLiteral("a b.tif")).isValid());
    QVERIFY(!ts::GeoidModel(dir.path(), QStringLiteral("../fixture.tif"))
                 .isValid());
  }

  void provider()
  {
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("geoid.tif"))));
    const QString path = dir.filePath(QStringLiteral("heights.sqlite"));
    QVERIFY(writeFixtureHeights(path, QStringLiteral("geoid")));
    std::shared_ptr<const ts::GeoidModel> geoid =
        std::make_shared<const ts::GeoidModel>(dir.path(),
                                               QStringLiteral("geoid.tif"));
    ts::SqliteElevationProvider provider(path, geoid);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    QCOMPARE(provider.maximumLevel(), 2);
    QCOMPARE(provider.samples(), 9);

    // Heights above the geoid as stored; above the ellipsoid as loaded.
    const ts::TileKey key{2, 5, 1};
    const std::optional<ts::HeightTile> stored =
        provider.loadStoredHeights(key);
    const std::optional<ts::HeightTile> loaded =
        provider.loadHeights(key, notCancelled);
    if (!stored || !loaded) {
      QFAIL("no heights");
    }
    for (int row = 0; row < 9; row += 4) {
      for (int column = 0; column < 9; column += 4) {
        const ts::GeoPoint point = ts::heightSamplePoint(key, 9, row, column);
        const std::size_t i = static_cast<std::size_t>(row) * 9 +
                              static_cast<std::size_t>(column);
        QVERIFY(std::abs(static_cast<double>(stored->heightsM[i]) -
                         fixtureHeight(point.latDeg, point.lonDeg)) <= 1.0);
        QVERIFY(std::abs(static_cast<double>(loaded->heightsM[i] -
                                             stored->heightsM[i]) -
                         fixtureUndulation(point.latDeg, point.lonDeg)) < 1e-3);
      }
    }
    // Deeper than stored: interpolated from the ancestor, still converted.
    const ts::TileKey deep{5, 50, 9};
    const std::optional<ts::HeightTile> deeper =
        provider.loadHeights(deep, notCancelled);
    if (!deeper) {
      QFAIL("no heights for a deeper key");
    }
    const ts::GeoPoint centre = ts::heightSamplePoint(deep, 9, 4, 4);
    QVERIFY(std::abs(static_cast<double>(deeper->heightsM[40]) -
                     fixtureHeight(centre.latDeg, centre.lonDeg) -
                     fixtureUndulation(centre.latDeg, centre.lonDeg)) <= 1.5);
    QVERIFY(!provider.loadStoredHeights(deep));
    std::atomic<bool> cancelled{true};
    QVERIFY(!provider.loadHeights(key, cancelled));

    // A datum without its geoid, or a store of images, can't be used.
    QVERIFY(!ts::SqliteElevationProvider(path, nullptr).isValid());
    const QString images = dir.filePath(QStringLiteral("images.sqlite"));
    {
      ts::SqliteTileStoreInfo info;
      info.maxLevel = 0;
      ts::SqliteTileStoreWriter writer(images, info);
      QVERIFY(writer.finish());
    }
    QVERIFY(!ts::SqliteElevationProvider(images, geoid).isValid());
  }

  void registry()
  {
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("egm.tif"))));
    QVERIFY(writeFixtureHeights(dir.filePath(QStringLiteral("dem.sqlite")),
                                QStringLiteral("egm")));
    ts::PackManifest manifest;
    manifest.id = QStringLiteral("terrasoar-base");
    ts::PackDataset dataset;
    dataset.id = QStringLiteral("elevation");
    dataset.kind = ts::PackDataset::Kind::Elevation;
    dataset.file = QStringLiteral("dem.sqlite");
    manifest.datasets.push_back(dataset);
    dataset.id = QStringLiteral("egm");
    dataset.kind = ts::PackDataset::Kind::Geoid;
    dataset.file = QStringLiteral("egm.tif");
    manifest.datasets.push_back(dataset);
    for (const char* file : {"dem.sqlite", "egm.tif"}) {
      ts::PackFile packFile;
      packFile.path = QString::fromLatin1(file);
      packFile.sha256 = QString(64, QLatin1Char('0'));
      packFile.sizeBytes = 1;
      manifest.files.push_back(packFile);
    }
    // The kinds survive the manifest's JSON.
    QString error;
    const std::optional<ts::PackManifest> back =
        ts::PackManifest::fromJson(manifest.toJson(), &error);
    QVERIFY2(back && back->datasets.size() == 2 &&
                 back->datasets[1].kind == ts::PackDataset::Kind::Geoid,
             qPrintable(error));

    ts::ProviderRegistry registry;
    const QJsonObject source{
        {QStringLiteral("kind"), QStringLiteral("basepack")},
        {QStringLiteral("dataset"), QStringLiteral("elevation")}};
    QVERIFY(!registry.elevation(source, &error));
    registry.setBasePack(dir.path(), manifest);
    std::shared_ptr<ts::ElevationProvider> provider =
        registry.elevation(source, &error);
    QVERIFY2(provider, qPrintable(error));
    QCOMPARE(registry.elevation(source, &error), provider); // shared
    const std::optional<ts::HeightTile> tile =
        provider->loadHeights(ts::TileKey{1, 2, 1}, notCancelled);
    QVERIFY(tile.has_value());
    std::shared_ptr<const ts::GeoidModel> geoid =
        registry.geoid(QStringLiteral("egm"), &error);
    QVERIFY2(geoid, qPrintable(error));
    QVERIFY(std::abs(geoid->undulationM(30.0, 60.0) -
                     fixtureUndulation(30.0, 60.0)) < 1e-4);
    QVERIFY(!registry.geoid(QStringLiteral("egm2008"), &error));
    QVERIFY(!registry.elevation(
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("basepack")},
                    {QStringLiteral("dataset"), QStringLiteral("other")}},
        &error));
  }

  void gdalProvider()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dem.tif"));
    QVERIFY(writeGeographicDem(path));
    ts::GdalElevationProvider provider(path, QStringLiteral("ellipsoid"),
                                       nullptr, nullptr);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    QVERIFY(std::abs(provider.bounds().westDeg - 7.0) < 1e-9);
    QVERIFY(std::abs(provider.bounds().northDeg - 47.0) < 1e-9);
    // 0.01° pixels: level 9's samples (0.0055°) are the first as fine.
    QCOMPARE(provider.maximumLevel(), 9);
    QCOMPARE(provider.name(), QStringLiteral("dem.tif"));

    // Inside: the plane exactly, at full resolution and averaged.
    for (const int level : {9, 8}) {
      const ts::TileKey key =
          ts::TileKey::containing(level, ts::Geodetic{46.0, 8.0, 0.0});
      const std::optional<ts::HeightTile> tile =
          provider.loadHeights(key, notCancelled);
      if (!tile) {
        QFAIL("no heights");
      }
      QCOMPARE(tile->samples, 65);
      QVERIFY(tile->seaLevelM.empty());
      for (int row = 0; row < 65; row += 8) {
        for (int column = 0; column < 65; column += 8) {
          const ts::GeoPoint point =
              ts::heightSamplePoint(key, 65, row, column);
          QVERIFY2(std::abs(heightAt(*tile, row, column) -
                            demHeight(point.latDeg, point.lonDeg)) < 0.01,
                   qPrintable(QStringLiteral("level %1 (%2, %3): %4")
                                  .arg(level)
                                  .arg(point.latDeg)
                                  .arg(point.lonDeg)
                                  .arg(heightAt(*tile, row, column))));
        }
      }
    }

    // Neighbours share their edge samples exactly, also when averaged.
    for (const int level : {6, 8}) {
      const ts::TileKey west =
          ts::TileKey::containing(level, ts::Geodetic{46.0, 7.9, 0.0});
      const ts::TileKey east{west.level, west.x + 1, west.y};
      const std::optional<ts::HeightTile> a =
          provider.loadHeights(west, notCancelled);
      const std::optional<ts::HeightTile> b =
          provider.loadHeights(east, notCancelled);
      if (!a) {
        QFAIL("no heights west");
      }
      if (!b) {
        QFAIL("no heights east");
      }
      const ts::HeightTile& westTile = *a;
      const ts::HeightTile& eastTile = *b;
      for (int row = 0; row < 65; ++row) {
        const double left = heightAt(westTile, row, 64);
        const double right = heightAt(eastTile, row, 0);
        QVERIFY((std::isnan(left) && std::isnan(right)) || left == right);
      }
    }

    // Across the DEM's edge: no data (NaN) outside it.
    const ts::TileKey edge =
        ts::TileKey::containing(6, ts::Geodetic{47.0, 9.0, 0.0});
    const std::optional<ts::HeightTile> partial =
        provider.loadHeights(edge, notCancelled);
    if (!partial) {
      QFAIL("no heights across the edge");
    }
    const ts::HeightTile& across = *partial;
    int inside = 0;
    int outside = 0;
    for (int row = 0; row < 65; ++row) {
      for (int column = 0; column < 65; ++column) {
        const ts::GeoPoint point = ts::heightSamplePoint(edge, 65, row, column);
        const double height = heightAt(across, row, column);
        if (point.latDeg > 47.01 || point.lonDeg > 9.01 ||
            point.latDeg < 44.99 || point.lonDeg < 6.99) {
          QVERIFY(std::isnan(height));
          ++outside;
        } else if (point.latDeg < 46.9 && point.lonDeg < 8.9 &&
                   point.latDeg > 45.1 && point.lonDeg > 7.1) {
          QVERIFY(std::abs(height - demHeight(point.latDeg, point.lonDeg)) <
                  0.01);
          ++inside;
        }
      }
    }
    QVERIFY(inside > 0 && outside > 0);

    // Nothing at all: a tile elsewhere, or one so coarse that a file
    // without overviews would be read whole for it.
    QVERIFY(!provider.loadHeights(
        ts::TileKey::containing(8, ts::Geodetic{-30.0, 100.0, 0.0}),
        notCancelled));
    QVERIFY(!provider.loadHeights(ts::TileKey{2, 4, 1}, notCancelled));
    std::atomic<bool> cancelled{true};
    QVERIFY(!provider.loadHeights(
        ts::TileKey::containing(9, ts::Geodetic{46.0, 8.0, 0.0}), cancelled));

    // A file that isn't a DEM.
    QFile text(dir.filePath(QStringLiteral("notes.txt")));
    QVERIFY(text.open(QIODevice::WriteOnly));
    text.write("not a raster");
    text.close();
    const ts::GdalElevationProvider bad(
        text.fileName(), QStringLiteral("ellipsoid"), nullptr, nullptr);
    QVERIFY(!bad.isValid());
    QVERIFY(!bad.errorString().isEmpty());
  }

  void gdalProviderNoData()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("holes.tif"));
    const double hole = -32768.0;
    QVERIFY(writeGeographicDem(path, "EPSG:4326", &hole));
    ts::GdalElevationProvider provider(path, QStringLiteral("ellipsoid"),
                                       nullptr, nullptr);
    QVERIFY(provider.isValid());
    const ts::TileKey key =
        ts::TileKey::containing(9, ts::Geodetic{46.5, 7.5, 0.0});
    const std::optional<ts::HeightTile> tile =
        provider.loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    int holes = 0;
    for (int row = 0; row < 65; ++row) {
      for (int column = 0; column < 65; ++column) {
        const ts::GeoPoint point = ts::heightSamplePoint(key, 65, row, column);
        const double height = heightAt(*tile, row, column);
        if (std::abs(point.latDeg - 46.5) < 0.03 &&
            std::abs(point.lonDeg - 7.5) < 0.03) {
          QVERIFY(std::isnan(height));
          ++holes;
        } else if (!std::isnan(height)) {
          // Never the no-data value, nor pulled towards it.
          QVERIFY(std::abs(height - demHeight(point.latDeg, point.lonDeg)) <
                  1.0);
        }
      }
    }
    QVERIFY(holes > 0);
  }

  void gdalProviderProjected()
  {
    // UTM zone 32 N, 100 m pixels: a plane in easting and northing.
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("utm.tif"));
    const std::function<double(double, double)> plane = [](double easting,
                                                           double northing) {
      return 500.0 + 0.01 * (easting - 400000.0) +
             0.02 * (5100000.0 - northing);
    };
    QVERIFY(writeDem(path, "EPSG:32632",
                     {400000.0, 100.0, 0.0, 5100000.0, 0.0, -100.0}, 200, 200,
                     plane));
    ts::GdalElevationProvider provider(path, QStringLiteral("ellipsoid"),
                                       nullptr, nullptr);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    // 100 m is about 0.0009° of latitude: level 12 (0.00067°).
    QCOMPARE(provider.maximumLevel(), 12);
    OGRSpatialReferenceH wgs84 = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(wgs84, 4326);
    OSRSetAxisMappingStrategy(wgs84, OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReferenceH utm = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(utm, 32632);
    OSRSetAxisMappingStrategy(utm, OAMS_TRADITIONAL_GIS_ORDER);
    OGRCoordinateTransformationH transform =
        OCTNewCoordinateTransformation(wgs84, utm);
    // The middle of the raster.
    OGRCoordinateTransformationH inverse =
        OCTNewCoordinateTransformation(utm, wgs84);
    double lon = 410000.0;
    double lat = 5090000.0;
    QVERIFY(OCTTransform(inverse, 1, &lon, &lat, nullptr) != 0);
    OCTDestroyCoordinateTransformation(inverse);
    const ts::TileKey key =
        ts::TileKey::containing(12, ts::Geodetic{lat, lon, 0.0});
    const std::optional<ts::HeightTile> tile =
        provider.loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    for (int row = 0; row < 65; row += 16) {
      for (int column = 0; column < 65; column += 16) {
        const ts::GeoPoint point = ts::heightSamplePoint(key, 65, row, column);
        double x = point.lonDeg;
        double y = point.latDeg;
        QVERIFY(OCTTransform(transform, 1, &x, &y, nullptr) != 0);
        QVERIFY2(std::abs(heightAt(*tile, row, column) - plane(x, y)) < 0.01,
                 qPrintable(QString::number(heightAt(*tile, row, column))));
      }
    }
    OCTDestroyCoordinateTransformation(transform);
    OSRRelease(wgs84);
    OSRRelease(utm);
  }

  void gdalProviderConcurrently()
  {
    // Loader threads share one provider; each load uses its own dataset.
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dem.tif"));
    QVERIFY(writeGeographicDem(path));
    ts::GdalElevationProvider provider(path, QStringLiteral("ellipsoid"),
                                       nullptr, nullptr);
    QVERIFY(provider.isValid());
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&](int thread) {
          for (int i = 0; i < 4; ++i) {
            const ts::TileKey key = ts::TileKey::containing(
                9, ts::Geodetic{45.2 + 0.2 * thread, 7.2 + 0.4 * i, 0.0});
            const std::optional<ts::HeightTile> tile =
                provider.loadHeights(key, notCancelled);
            if (!tile) {
              return false;
            }
            const ts::GeoPoint point = ts::heightSamplePoint(key, 65, 32, 32);
            if (std::abs(heightAt(*tile, 32, 32) -
                         demHeight(point.latDeg, point.lonDeg)) > 0.01) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void demVerticalDatum()
  {
    const QTemporaryDir dir;
    struct Case {
      const char* crs;
      ts::DemVerticalDatum expected;
    };
    const std::array<Case, 5> cases = {
        {{"EPSG:4326", ts::DemVerticalDatum::Unknown},
         {"EPSG:4326+3855", ts::DemVerticalDatum::Egm2008},
         {"EPSG:4326+5773", ts::DemVerticalDatum::Egm96},
         {"EPSG:4979", ts::DemVerticalDatum::Ellipsoid},
         {"EPSG:4326+5701", ts::DemVerticalDatum::Odn}}};
    int index = 0;
    for (const Case& c : cases) {
      const QString path =
          dir.filePath(QStringLiteral("dem%1.tif").arg(index++));
      QVERIFY(writeGeographicDem(path, c.crs));
      QCOMPARE(static_cast<int>(ts::detectDemVerticalDatum(path)),
               static_cast<int>(c.expected));
    }
    QCOMPARE(ts::demVerticalDatumId(ts::DemVerticalDatum::Egm96),
             QStringLiteral("egm96"));
    QCOMPARE(ts::demVerticalDatumId(ts::DemVerticalDatum::Odn),
             QStringLiteral("osgm15"));
    QVERIFY(ts::demVerticalDatumId(ts::DemVerticalDatum::Unknown).isEmpty());
  }

  void gdalProviderDatum()
  {
    // Heights above a geoid become heights above the ellipsoid.
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("geoid.tif"))));
    const QString path = dir.filePath(QStringLiteral("dem.tif"));
    QVERIFY(writeGeographicDem(path));
    std::shared_ptr<const ts::GeoidModel> geoid =
        std::make_shared<const ts::GeoidModel>(dir.path(),
                                               QStringLiteral("geoid.tif"));
    ts::GdalElevationProvider provider(path, QStringLiteral("geoid"), geoid,
                                       nullptr);
    QVERIFY(provider.isValid());
    const ts::TileKey key =
        ts::TileKey::containing(9, ts::Geodetic{46.0, 8.0, 0.0});
    const std::optional<ts::HeightTile> tile =
        provider.loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    QCOMPARE(tile->seaLevelM.size(), tile->heightsM.size());
    const ts::GeoPoint point = ts::heightSamplePoint(key, 65, 32, 32);
    const double n = fixtureUndulation(point.latDeg, point.lonDeg);
    QVERIFY(std::abs(heightAt(*tile, 32, 32) -
                     demHeight(point.latDeg, point.lonDeg) - n) < 0.01);
    QVERIFY(std::abs(static_cast<double>(tile->seaLevelM[32 * 65 + 32]) - n) <
            1e-3);

    // Heights above the ellipsoid stay; sea level comes from the other.
    ts::GdalElevationProvider ellipsoidal(path, QStringLiteral("ellipsoid"),
                                          nullptr, geoid);
    const std::optional<ts::HeightTile> plain =
        ellipsoidal.loadHeights(key, notCancelled);
    if (!plain) {
      QFAIL("no heights");
    }
    QVERIFY(std::abs(heightAt(*plain, 32, 32) -
                     demHeight(point.latDeg, point.lonDeg)) < 0.01);
    QVERIFY(std::abs(static_cast<double>(plain->seaLevelM[32 * 65 + 32]) - n) <
            1e-3);
    QVERIFY(provider.id() != ellipsoidal.id());
  }

  void compositePriority()
  {
    // A detailed DEM over 7°–9° E, 45°–47° N above the global heights.
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("geoid.tif"))));
    QVERIFY(writeFixtureHeights(dir.filePath(QStringLiteral("global.sqlite")),
                                QStringLiteral("geoid")));
    QVERIFY(writeGeographicDem(dir.filePath(QStringLiteral("dem.tif"))));
    std::shared_ptr<const ts::GeoidModel> geoid =
        std::make_shared<const ts::GeoidModel>(dir.path(),
                                               QStringLiteral("geoid.tif"));
    std::shared_ptr<ts::ElevationProvider> global =
        std::make_shared<ts::SqliteElevationProvider>(
            dir.filePath(QStringLiteral("global.sqlite")), geoid);
    std::shared_ptr<ts::ElevationProvider> local =
        std::make_shared<ts::GdalElevationProvider>(
            dir.filePath(QStringLiteral("dem.tif")),
            QStringLiteral("ellipsoid"), nullptr, nullptr);
    ts::CompositeElevationProvider composite({local, global});
    QCOMPARE(composite.maximumLevel(), 9);
    QCOMPARE(composite.name(), QStringLiteral("dem.tif"));
    QVERIFY(composite.id().contains(local->id()) &&
            composite.id().contains(global->id()));

    // Across the DEM's corner: its heights inside, the global ones outside.
    const ts::TileKey key =
        ts::TileKey::containing(6, ts::Geodetic{47.0, 9.0, 0.0});
    const std::optional<ts::HeightTile> tile =
        composite.loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    QCOMPARE(tile->samples, 65);
    QCOMPARE(tile->seaLevelM.size(), tile->heightsM.size());
    int fromLocal = 0;
    int fromGlobal = 0;
    for (int row = 0; row < 65; ++row) {
      for (int column = 0; column < 65; ++column) {
        const ts::GeoPoint point = ts::heightSamplePoint(key, 65, row, column);
        const double height = heightAt(*tile, row, column);
        QVERIFY(!std::isnan(height));
        if (point.latDeg < 46.9 && point.lonDeg < 8.9 && point.latDeg > 45.1) {
          QVERIFY(std::abs(height - demHeight(point.latDeg, point.lonDeg)) <
                  0.01);
          ++fromLocal;
        } else if (point.latDeg > 47.01 || point.lonDeg > 9.01) {
          QVERIFY(std::abs(height - fixtureHeight(point.latDeg, point.lonDeg) -
                           fixtureUndulation(point.latDeg, point.lonDeg)) <
                  1.5);
          ++fromGlobal;
        }
      }
    }
    QVERIFY(fromLocal > 0 && fromGlobal > 0);

    // Far from the DEM: the global heights alone; coarse levels too.
    QVERIFY(composite.loadHeights(ts::TileKey{2, 0, 0}, notCancelled));
    // With nothing below, what the DEM lacks is sea level (here 0).
    ts::CompositeElevationProvider alone({local});
    const std::optional<ts::HeightTile> lone =
        alone.loadHeights(key, notCancelled);
    if (!lone) {
      QFAIL("no heights");
    }
    QCOMPARE(heightAt(*lone, 0, 64), 0.0);
    QVERIFY(!alone.loadHeights(ts::TileKey{2, 0, 0}, notCancelled));
  }

  void registryFiles()
  {
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("egm2008.tif"))));
    QVERIFY(writeGeographicDem(dir.filePath(QStringLiteral("dem.tif"))));
    ts::PackManifest manifest;
    manifest.id = QStringLiteral("terrasoar-base");
    ts::PackDataset dataset;
    dataset.id = QStringLiteral("egm2008");
    dataset.kind = ts::PackDataset::Kind::Geoid;
    dataset.file = QStringLiteral("egm2008.tif");
    manifest.datasets.push_back(dataset);
    ts::ProviderRegistry registry;
    registry.setBasePack(dir.path(), manifest);

    QJsonObject source{
        {QStringLiteral("kind"), QStringLiteral("file")},
        {QStringLiteral("path"), dir.filePath(QStringLiteral("dem.tif"))},
        {QStringLiteral("verticalDatum"), QStringLiteral("egm2008")}};
    QString error;
    std::shared_ptr<ts::ElevationProvider> provider =
        registry.elevation(source, &error);
    QVERIFY2(provider, qPrintable(error));
    QCOMPARE(registry.elevation(source, &error), provider); // shared
    const ts::TileKey key =
        ts::TileKey::containing(9, ts::Geodetic{46.0, 8.0, 0.0});
    const std::optional<ts::HeightTile> tile =
        provider->loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    const ts::GeoPoint point = ts::heightSamplePoint(key, 65, 32, 32);
    QVERIFY(std::abs(heightAt(*tile, 32, 32) -
                     demHeight(point.latDeg, point.lonDeg) -
                     fixtureUndulation(point.latDeg, point.lonDeg)) < 0.01);

    // Heights above the ellipsoid: the same file, another provider.
    source.insert(QStringLiteral("verticalDatum"), QStringLiteral("ellipsoid"));
    std::shared_ptr<ts::ElevationProvider> ellipsoidal =
        registry.elevation(source, &error);
    QVERIFY2(ellipsoidal, qPrintable(error));
    QVERIFY(ellipsoidal != provider);

    // An unknown datum, a datum whose geoid isn't there, a missing file.
    source.insert(QStringLiteral("verticalDatum"), QStringLiteral("odn"));
    QVERIFY(!registry.elevation(source, &error));
    source.insert(QStringLiteral("verticalDatum"), QStringLiteral("egm96"));
    QVERIFY(!registry.elevation(source, &error));
    source.insert(QStringLiteral("verticalDatum"), QStringLiteral("ellipsoid"));
    source.insert(QStringLiteral("path"),
                  dir.filePath(QStringLiteral("missing.tif")));
    error.clear();
    QVERIFY(!registry.elevation(source, &error));
    QVERIFY(error.contains(QStringLiteral("missing.tif")));

    // The same sources make the same composite while it is in use.
    std::shared_ptr<ts::ElevationProvider> composite =
        registry.elevationComposite({ellipsoidal, provider});
    QCOMPARE(registry.elevationComposite({ellipsoidal, provider}), composite);
    QVERIFY(registry.elevationComposite({provider, ellipsoidal}) != composite);
  }

  void terrainEncodings()
  {
    QCOMPARE(
        ts::decodeTerrainPixel(qRgb(128, 0, 0), ts::TerrainEncoding::Terrarium),
        0.0);
    QCOMPARE(ts::decodeTerrainPixel(qRgb(130, 10, 128),
                                    ts::TerrainEncoding::Terrarium),
             522.5);
    QVERIFY(std::abs(ts::decodeTerrainPixel(
                qRgb(1, 134, 160), ts::TerrainEncoding::TerrainRgb)) < 1e-9);
    QVERIFY(std::abs(ts::decodeTerrainPixel(qRgb(1, 140, 124),
                                            ts::TerrainEncoding::TerrainRgb) -
                     150.0) < 1e-6);
    QVERIFY(std::isnan(ts::decodeTerrainPixel(qRgba(1, 2, 3, 0),
                                              ts::TerrainEncoding::Terrarium)));

    // The layer source form.
    QString error;
    const QJsonObject json{
        {QStringLiteral("kind"), QStringLiteral("web")},
        {QStringLiteral("url"),
         QStringLiteral("https://tiles.example/{z}/{x}/{y}.png")},
        {QStringLiteral("encoding"), QStringLiteral("terrain-rgb")},
        {QStringLiteral("maximumZoom"), 12},
        {QStringLiteral("tileSize"), 512},
        {QStringLiteral("verticalDatum"), QStringLiteral("egm2008")},
        {QStringLiteral("attribution"), QStringLiteral("Example")}};
    const std::optional<ts::WebTerrainSource> source =
        ts::WebTerrainSource::fromJson(json, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    const ts::WebTerrainSource& parsed = *source;
    QCOMPARE(parsed.encoding, ts::TerrainEncoding::TerrainRgb);
    QCOMPARE(parsed.maximumZoom, 12);
    QCOMPARE(parsed.tileSizePx, 512);
    QCOMPARE(parsed.toJson(), json);
    QCOMPARE(parsed.tileUrl(5, 17, 11).toString(),
             QStringLiteral("https://tiles.example/5/17/11.png"));
    for (const std::pair<const char*, QJsonValue>& bad :
         {std::pair<const char*, QJsonValue>{
              "url", QStringLiteral("ftp://x/{z}/{x}/{y}")},
          {"url", QStringLiteral("https://x/{z}/{x}.png")},
          {"encoding", QStringLiteral("lerc")},
          {"tileSize", 300},
          {"maximumZoom", 30}}) {
      QJsonObject broken = json;
      broken.insert(QString::fromLatin1(bad.first), bad.second);
      error.clear();
      QVERIFY(!ts::WebTerrainSource::fromJson(broken, &error));
      QVERIFY(!error.isEmpty());
    }
  }

  void webTerrain()
  {
    std::shared_ptr<std::atomic<bool>> failing =
        std::make_shared<std::atomic<bool>>(false);
    ts::test::MockHttpServer server(terrariumHandler(failing));
    ts::WebTerrainSource source;
    source.urlTemplate =
        server.url().toString() + QStringLiteral("{z}/{x}/{y}.png");
    source.maximumZoom = 4;
    source.verticalDatum = QStringLiteral("ellipsoid");
    ts::WebTileSettings settings;
    settings.failuresBeforeOffline = 100;
    ts::WebTerrainProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), settings,
        nullptr, nullptr);
    QCOMPARE(provider.zoomForLevel(3), 2);
    QCOMPARE(provider.zoomForLevel(0), 0);
    QCOMPARE(provider.zoomForLevel(9), 4);
    QCOMPARE(provider.maximumLevel(), 5);
    QCOMPARE(provider.name(), QStringLiteral("127.0.0.1"));

    // Level 3 reads zoom 2: the plane exactly, edges shared.
    const ts::TileKey key =
        ts::TileKey::containing(3, ts::Geodetic{30.0, -50.0, 0.0});
    const std::optional<ts::HeightTile> tile =
        provider.loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    const ts::HeightTile& heights = *tile;
    QVERIFY(!heights.incomplete);
    for (int row = 0; row < 65; row += 8) {
      for (int column = 0; column < 65; column += 8) {
        const ts::GeoPoint point = ts::heightSamplePoint(key, 65, row, column);
        const std::pair<double, double> pixel =
            webPixel(point.latDeg, point.lonDeg, 2);
        // Within the tiles' first and last half pixel the nearest pixel
        // stands in; elsewhere it is the plane.
        QVERIFY2(std::abs(heightAt(heights, row, column) -
                          webHeight(pixel.first, pixel.second)) < 0.02,
                 qPrintable(QString::number(heightAt(heights, row, column))));
      }
    }
    const ts::TileKey east{key.level, key.x + 1, key.y};
    const std::optional<ts::HeightTile> next =
        provider.loadHeights(east, notCancelled);
    if (!next) {
      QFAIL("no heights east");
    }
    const ts::HeightTile& eastHeights = *next;
    for (int row = 0; row < 65; ++row) {
      QCOMPARE(heightAt(heights, row, 64), heightAt(eastHeights, row, 0));
    }
    // Web tiles shared by neighbours are fetched once.
    const int requests = provider.statistics().requests;
    QVERIFY(provider.loadHeights(key, notCancelled).has_value());
    QCOMPARE(provider.statistics().requests, requests);

    // Beyond the Mercator limit: no data.
    const ts::TileKey polar{3, 3, 0};
    const std::optional<ts::HeightTile> top =
        provider.loadHeights(polar, notCancelled);
    if (!top) {
      QFAIL("no heights near the pole");
    }
    QVERIFY(std::isnan(heightAt(*top, 0, 32)));
    QVERIFY(!std::isnan(heightAt(*top, 64, 32)));

    // Where the server has no tiles (404): nothing, and not a failure.
    const ts::TileKey missing =
        ts::TileKey::containing(3, ts::Geodetic{10.0, 150.0, 0.0});
    QVERIFY(!provider.loadHeights(missing, notCancelled));
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);

    // A failing server: an incomplete tile of no data, asked for again.
    failing->store(true);
    const ts::TileKey elsewhere =
        ts::TileKey::containing(3, ts::Geodetic{-30.0, 20.0, 0.0});
    const std::optional<ts::HeightTile> failed =
        provider.loadHeights(elsewhere, notCancelled);
    if (!failed) {
      QFAIL("no incomplete tile");
    }
    QVERIFY(failed->incomplete);
    QVERIFY(std::isnan(heightAt(*failed, 32, 32)));
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Error);
    failing->store(false);
    const std::optional<ts::HeightTile> again =
        provider.loadHeights(elsewhere, notCancelled);
    if (!again) {
      QFAIL("no heights after the server recovered");
    }
    QVERIFY(!again->incomplete);
    QVERIFY(!std::isnan(heightAt(*again, 32, 32)));
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);

    // Composed over other heights, a failure leaves them and is passed on.
    failing->store(true);
    std::shared_ptr<ts::ElevationProvider> web =
        std::make_shared<ts::WebTerrainProvider>(
            source, std::make_shared<ts::NetworkClient>(testNetwork()),
            settings, nullptr, nullptr);
    const QTemporaryDir dir;
    QVERIFY(writeGeographicDem(dir.filePath(QStringLiteral("dem.tif"))));
    std::shared_ptr<ts::ElevationProvider> local =
        std::make_shared<ts::GdalElevationProvider>(
            dir.filePath(QStringLiteral("dem.tif")),
            QStringLiteral("ellipsoid"), nullptr, nullptr);
    ts::CompositeElevationProvider composite({web, local});
    const ts::TileKey alps =
        ts::TileKey::containing(9, ts::Geodetic{46.0, 8.0, 0.0});
    const std::optional<ts::HeightTile> combined =
        composite.loadHeights(alps, notCancelled);
    if (!combined) {
      QFAIL("no combined heights");
    }
    QVERIFY(combined->incomplete);
    const ts::GeoPoint middle = ts::heightSamplePoint(alps, 65, 32, 32);
    QVERIFY(std::abs(heightAt(*combined, 32, 32) -
                     demHeight(middle.latDeg, middle.lonDeg)) < 0.01);
  }

  void webTerrainDatumAndRegistry()
  {
    std::shared_ptr<std::atomic<bool>> failing =
        std::make_shared<std::atomic<bool>>(false);
    ts::test::MockHttpServer server(terrariumHandler(failing));
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("egm96.tif"))));
    ts::PackManifest manifest;
    manifest.id = QStringLiteral("terrasoar-base");
    ts::PackDataset dataset;
    dataset.id = QStringLiteral("egm96");
    dataset.kind = ts::PackDataset::Kind::Geoid;
    dataset.file = QStringLiteral("egm96.tif");
    manifest.datasets.push_back(dataset);
    ts::ProviderRegistry registry;
    registry.setBasePack(dir.path(), manifest);
    registry.setWmsNetwork(std::make_shared<ts::NetworkClient>(testNetwork()),
                           ts::WebTileSettings{});

    QJsonObject source{
        {QStringLiteral("kind"), QStringLiteral("web")},
        {QStringLiteral("url"),
         QString(server.url().toString() + QStringLiteral("{z}/{x}/{y}.png"))},
        {QStringLiteral("encoding"), QStringLiteral("terrarium")},
        {QStringLiteral("maximumZoom"), 4},
        {QStringLiteral("verticalDatum"), QStringLiteral("egm96")},
        {QStringLiteral("attribution"), QStringLiteral("Fixture terrain")}};
    QString error;
    std::shared_ptr<ts::ElevationProvider> provider =
        registry.elevation(source, &error);
    QVERIFY2(provider, qPrintable(error));
    QCOMPARE(registry.elevation(source, &error), provider); // shared
    QCOMPARE(registry.attribution(source), QStringLiteral("Fixture terrain"));
    QVERIFY(provider->preferredLoaderThreads() > 2);

    // Heights above EGM96 become heights above the ellipsoid.
    const ts::TileKey key =
        ts::TileKey::containing(3, ts::Geodetic{30.0, -50.0, 0.0});
    const std::optional<ts::HeightTile> tile =
        provider->loadHeights(key, notCancelled);
    if (!tile) {
      QFAIL("no heights");
    }
    const ts::GeoPoint point = ts::heightSamplePoint(key, 65, 32, 32);
    const std::pair<double, double> pixel =
        webPixel(point.latDeg, point.lonDeg, 2);
    QVERIFY(std::abs(heightAt(*tile, 32, 32) -
                     webHeight(pixel.first, pixel.second) -
                     fixtureUndulation(point.latDeg, point.lonDeg)) < 0.02);
    QCOMPARE(tile->seaLevelM.size(), tile->heightsM.size());

    // EGM2008 isn't in this pack; a broken source says why.
    source.insert(QStringLiteral("verticalDatum"), QStringLiteral("egm2008"));
    QVERIFY(!registry.elevation(source, &error));
    source.insert(QStringLiteral("verticalDatum"), QStringLiteral("egm96"));
    source.insert(QStringLiteral("encoding"), QStringLiteral("png"));
    error.clear();
    QVERIFY(!registry.elevation(source, &error));
    QVERIFY(!error.isEmpty());
  }

  void profilePath()
  {
    // Evenly spaced along geodesic legs, the ends exact.
    const std::vector<std::pair<double, ts::GeoPoint>> along =
        ts::samplePath({ts::GeoPoint{0.0, 0.0}, ts::GeoPoint{0.0, 1.0}}, 11);
    QCOMPARE(along.size(), std::size_t{11});
    QVERIFY(std::abs(along.back().first - 111319.49) < 0.1);
    for (int i = 0; i < 11; ++i) {
      const std::pair<double, ts::GeoPoint>& at =
          along[static_cast<std::size_t>(i)];
      QVERIFY(std::abs(at.second.lonDeg - 0.1 * i) < 1e-9);
      QVERIFY(std::abs(at.first - 11131.949 * i) < 0.01);
    }
    // Two legs: the corner is passed at its distance.
    const std::vector<std::pair<double, ts::GeoPoint>> bent =
        ts::samplePath({ts::GeoPoint{0.0, 0.0}, ts::GeoPoint{0.0, 1.0},
                        ts::GeoPoint{1.0, 1.0}},
                       3);
    QVERIFY(std::abs(bent[1].second.lonDeg - 1.0) < 0.01);
    QVERIFY(std::abs(bent[2].second.latDeg - 1.0) < 1e-12);
    QVERIFY(ts::samplePath({ts::GeoPoint{0.0, 0.0}}, 10).empty());
    QVERIFY(ts::samplePath({ts::GeoPoint{0.0, 0.0}, ts::GeoPoint{0.0, 1.0}}, 1)
                .empty());

    // The level whose samples are as close as the profile's.
    QCOMPARE(ts::profileLevel(5000.0, 20), 6);
    QCOMPARE(ts::profileLevel(5000.0, 4), 4);
    QCOMPARE(ts::profileLevel(1.0e7, 20), 0);
  }

  void profileOfDem()
  {
    // Across the plane DEM: the heights GDAL reads there, exactly.
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("geoid.tif"))));
    const QString path = dir.filePath(QStringLiteral("dem.tif"));
    QVERIFY(writeGeographicDem(path));
    std::shared_ptr<const ts::GeoidModel> geoid =
        std::make_shared<const ts::GeoidModel>(dir.path(),
                                               QStringLiteral("geoid.tif"));
    ts::GdalElevationProvider provider(path, QStringLiteral("geoid"), geoid,
                                       nullptr);
    const std::optional<ts::ElevationProfile> profile =
        ts::computeElevationProfile(
            provider, {ts::GeoPoint{45.5, 7.5}, ts::GeoPoint{46.5, 8.5}}, 101,
            notCancelled);
    if (!profile) {
      QFAIL("no profile");
    }
    const ts::ElevationProfile& result = *profile;
    QCOMPARE(result.points.size(), std::size_t{101});
    QVERIFY(result.lengthM > 130000.0 && result.lengthM < 140000.0);
    QVERIFY(result.level >= 6 && result.level <= 9);
    for (const ts::ProfilePoint& point : result.points) {
      QVERIFY2(std::abs(point.aboveSeaLevelM() -
                        demHeight(point.position.latDeg,
                                  point.position.lonDeg)) < 0.05,
               qPrintable(QString::number(point.aboveSeaLevelM())));
      QVERIFY(std::abs(point.seaLevelM -
                       fixtureUndulation(point.position.latDeg,
                                         point.position.lonDeg)) < 1e-3);
    }
    // Uphill all the way: north-east is up on the plane.
    QVERIFY(std::abs(result.minimumM - demHeight(45.5, 7.5)) < 0.05);
    QVERIFY(std::abs(result.maximumM - demHeight(46.5, 8.5)) < 0.05);
    QVERIFY(std::abs(result.ascentM - (result.maximumM - result.minimumM)) <
            0.1);
    QVERIFY(result.descentM < 0.1);

    // Off the DEM, too short, or cancelled: nothing.
    QVERIFY(!ts::computeElevationProfile(
        provider, {ts::GeoPoint{10.0, 10.0}, ts::GeoPoint{10.5, 10.5}}, 11,
        notCancelled));
    QVERIFY(!ts::computeElevationProfile(provider, {ts::GeoPoint{46.0, 8.0}},
                                         11, notCancelled));
    std::atomic<bool> cancelled{true};
    QVERIFY(!ts::computeElevationProfile(
        provider, {ts::GeoPoint{45.5, 7.5}, ts::GeoPoint{46.5, 8.5}}, 11,
        cancelled));
  }

  void geoidPacks()
  {
    // A base pack with EGM2008 and a DEM on it; packs with a finer EGM2008
    // (here: the fixture plus 10 m) and a datum of their own.
    const QTemporaryDir dir;
    const QString base = dir.filePath(QStringLiteral("base"));
    const QString fine = dir.filePath(QStringLiteral("fine"));
    const QString odn = dir.filePath(QStringLiteral("odn"));
    for (const QString& path : {base, fine, odn}) {
      QVERIFY(QDir().mkpath(path));
    }
    QVERIFY(
        writeFixtureGeoid(QDir(base).filePath(QStringLiteral("egm2008.tif"))));
    QVERIFY(
        writeFixtureHeights(QDir(base).filePath(QStringLiteral("dem.sqlite")),
                            QStringLiteral("egm2008")));
    QVERIFY(writeFixtureGeoid(
        QDir(fine).filePath(QStringLiteral("egm2008.tif")), 10.0));
    QVERIFY(writeFixtureGeoid(QDir(odn).filePath(QStringLiteral("osgm15.tif")),
                              -20.0));
    ts::PackManifest manifest;
    manifest.id = QStringLiteral("terrasoar-base");
    ts::PackDataset dataset;
    dataset.id = QStringLiteral("elevation");
    dataset.kind = ts::PackDataset::Kind::Elevation;
    dataset.file = QStringLiteral("dem.sqlite");
    manifest.datasets.push_back(dataset);
    dataset.id = QStringLiteral("egm2008");
    dataset.kind = ts::PackDataset::Kind::Geoid;
    dataset.file = QStringLiteral("egm2008.tif");
    manifest.datasets.push_back(dataset);
    ts::ProviderRegistry registry;
    registry.setBasePack(base, manifest);

    const QJsonObject terrain{
        {QStringLiteral("kind"), QStringLiteral("basepack")},
        {QStringLiteral("dataset"), QStringLiteral("elevation")}};
    QString error;
    std::shared_ptr<ts::ElevationProvider> before =
        registry.elevation(terrain, &error);
    QVERIFY2(before, qPrintable(error));
    std::vector<ts::VerticalDatum> datums = registry.verticalDatums();
    QCOMPARE(datums.size(), std::size_t{2});
    QCOMPARE(datums.front().id, QStringLiteral("egm2008"));
    QCOMPARE(datums.back().id, QStringLiteral("ellipsoid"));

    ts::InstalledPack finer;
    finer.directory = fine;
    finer.manifest.id = QStringLiteral("geoid-egm2008-fine");
    dataset.replaces = QStringLiteral("egm2008");
    finer.manifest.datasets = {dataset};
    ts::InstalledPack newlyn;
    newlyn.directory = odn;
    newlyn.manifest.id = QStringLiteral("geoid-osgm15");
    dataset.id = QStringLiteral("osgm15");
    dataset.file = QStringLiteral("osgm15.tif");
    dataset.replaces.clear();
    dataset.name = QStringLiteral("OSGM15 (Ordnance Datum Newlyn)");
    newlyn.manifest.datasets = {dataset};
    registry.setInstalledPacks({finer, newlyn});

    // The finer grid is used, by a new provider (the old one keeps its
    // grid while still in use).
    std::shared_ptr<const ts::GeoidModel> geoid =
        registry.geoid(QStringLiteral("egm2008"), &error);
    QVERIFY2(geoid, qPrintable(error));
    QVERIFY(std::abs(geoid->undulationM(30.0, 60.0) -
                     fixtureUndulation(30.0, 60.0) - 10.0) < 1e-3);
    std::shared_ptr<ts::ElevationProvider> after =
        registry.elevation(terrain, &error);
    QVERIFY2(after, qPrintable(error));
    QVERIFY(after != before && after->id() != before->id());
    const ts::TileKey key{2, 5, 1};
    const std::optional<ts::HeightTile> old =
        before->loadHeights(key, notCancelled);
    const std::optional<ts::HeightTile> now =
        after->loadHeights(key, notCancelled);
    if (!old) {
      QFAIL("no heights before");
    }
    if (!now) {
      QFAIL("no heights after");
    }
    const ts::HeightTile& oldHeights = *old;
    const ts::HeightTile& newHeights = *now;
    QVERIFY(std::abs(static_cast<double>(newHeights.heightsM[40] -
                                         oldHeights.heightsM[40]) -
                     10.0) < 1e-3);

    // The pack's own datum is offered and usable.
    datums = registry.verticalDatums();
    QCOMPARE(datums.size(), std::size_t{3});
    QCOMPARE(datums[1].id, QStringLiteral("osgm15"));
    QCOMPARE(datums[1].name, QStringLiteral("OSGM15 (Ordnance Datum Newlyn)"));
    const QString demPath = dir.filePath(QStringLiteral("dem.tif"));
    QVERIFY(writeGeographicDem(demPath));
    const QJsonObject file{
        {QStringLiteral("kind"), QStringLiteral("file")},
        {QStringLiteral("path"), demPath},
        {QStringLiteral("verticalDatum"), QStringLiteral("osgm15")}};
    std::shared_ptr<ts::ElevationProvider> local =
        registry.elevation(file, &error);
    QVERIFY2(local, qPrintable(error));
    const ts::TileKey alps =
        ts::TileKey::containing(9, ts::Geodetic{46.0, 8.0, 0.0});
    const std::optional<ts::HeightTile> heights =
        local->loadHeights(alps, notCancelled);
    if (!heights) {
      QFAIL("no heights");
    }
    const ts::GeoPoint point = ts::heightSamplePoint(alps, 65, 32, 32);
    QVERIFY(std::abs(heightAt(*heights, 32, 32) -
                     demHeight(point.latDeg, point.lonDeg) -
                     (fixtureUndulation(point.latDeg, point.lonDeg) - 20.0)) <
            0.01);

    // Uninstalled: the base pack's grid again, and no OSGM15.
    registry.setInstalledPacks({});
    geoid = registry.geoid(QStringLiteral("egm2008"), &error);
    QVERIFY(geoid && std::abs(geoid->undulationM(30.0, 60.0) -
                              fixtureUndulation(30.0, 60.0)) < 1e-3);
    QCOMPARE(registry.verticalDatums().size(), std::size_t{2});
    QVERIFY(!registry.elevation(file, &error));
  }

  void regionalDatum()
  {
    // A regional geoid (as OSGM15) converts heights where it has values;
    // outside it the global one stands in rather than nothing.
    const QTemporaryDir dir;
    QVERIFY(writeFixtureGeoid(dir.filePath(QStringLiteral("global.tif"))));
    QVERIFY(writeRegionalGeoid(dir.filePath(QStringLiteral("regional.tif"))));
    const ts::GeoidModel global(dir.path(), QStringLiteral("global.tif"));
    const ts::GeoidModel regional(dir.path(), QStringLiteral("regional.tif"));
    QVERIFY2(regional.isValid(), qPrintable(regional.errorString()));
    QVERIFY(std::abs(regional.undulationM(46.0, 8.0) -
                     (fixtureUndulation(46.0, 8.0) - 20.0)) < 1e-3);
    QVERIFY(!std::isfinite(regional.undulationM(30.0, 60.0)));
    ts::HeightTile tile;
    tile.samples = 2;
    tile.heightsM = {100.0F, 100.0F, 100.0F, 100.0F};
    const std::vector<ts::GeoPoint> points = {
        ts::GeoPoint{46.0, 8.0}, ts::GeoPoint{46.5, 7.5},
        ts::GeoPoint{30.0, 60.0}, ts::GeoPoint{50.0, 9.5}};
    ts::applyVerticalDatum(tile, points, &regional, &global);
    QVERIFY(std::abs(static_cast<double>(tile.heightsM[0]) - 100.0 -
                     (fixtureUndulation(46.0, 8.0) - 20.0)) < 1e-3);
    QVERIFY(std::abs(static_cast<double>(tile.heightsM[2]) - 100.0 -
                     fixtureUndulation(30.0, 60.0)) < 1e-3);
    QVERIFY(std::abs(static_cast<double>(tile.seaLevelM[3]) -
                     fixtureUndulation(50.0, 9.5)) < 1e-3);
  }
};

QTEST_GUILESS_MAIN(TestElevation)
#include "tst_elevation.moc"
