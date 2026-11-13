// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include <QDir>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "ts/deps/dependencies.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/heightgrid.h"
#include "ts/elevation/sqliteelevationprovider.h"
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
bool writeFixtureGeoid(const QString& path)
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
          static_cast<float>(fixtureUndulation(90.0 - row, -180.0 + column));
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
};

QTEST_GUILESS_MAIN(TestElevation)
#include "tst_elevation.moc"
