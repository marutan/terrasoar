// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A small base pack for tests: solid-colour Blue Marble tiles (levels 0
 * and 1) and a shoreline with one island, with a correct manifest.
 */
#ifndef TS_TESTS_FIXTUREPACK_H
#define TS_TESTS_FIXTUREPACK_H

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QString>

#include "ts/deps/sqliteconnection.h"
#include "ts/packs/basepack.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts::test
{

/**
 * Writes a base pack into the empty directory @p directory and returns
 * its manifest. The imagery is @p color everywhere.
 */
/** Appends @p value to @p bytes, little-endian. */
inline void appendUint32(QByteArray& bytes, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    bytes.append(
        static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xffU));
  }
}

/**
 * A GeoPackage geometry blob (no envelope, SRS 4326) of a MULTIPOLYGON
 * with one ring per polygon, each given as longitude/latitude pairs.
 */
inline QByteArray
multiPolygonBlob(const std::vector<std::vector<double>>& rings)
{
  QByteArray blob("GP");
  blob.append(static_cast<char>(0));    // version
  blob.append(static_cast<char>(0x01)); // little-endian, no envelope
  appendUint32(blob, 4326);
  blob.append(static_cast<char>(1));
  appendUint32(blob, 6); // MultiPolygon
  appendUint32(blob, static_cast<std::uint32_t>(rings.size()));
  for (const std::vector<double>& ring : rings) {
    blob.append(static_cast<char>(1));
    appendUint32(blob, 3); // Polygon
    appendUint32(blob, 1);
    appendUint32(blob, static_cast<std::uint32_t>(ring.size() / 2 + 1));
    for (std::size_t i = 0; i < ring.size() + 2; ++i) {
      const double value = ring[i % ring.size()];
      blob.append(reinterpret_cast<const char*>(&value), sizeof value);
    }
  }
  return blob;
}

/**
 * Writes the countries table the packager writes (only what
 * ts::CountryAreas reads): "Fixture Island" (FIX) around the fixture
 * shoreline's island, and "Fixture Atoll" (ATL) across the antimeridian.
 */
inline void writeFixtureCountries(const QString& path)
{
  SqliteConnection connection(path,
                              SqliteConnection::OpenMode::ReadWriteCreate);
  connection.execute(QStringLiteral(
      "CREATE TABLE countries (fid INTEGER PRIMARY KEY, id TEXT, name TEXT, "
      "iso_a2 TEXT, iso_a3 TEXT, geom BLOB)"));
  const std::function<void(const char*, const char*, const QByteArray&)>
      insert = [&connection](const char* id, const char* name,
                             const QByteArray& blob) {
        connection.execute(QStringLiteral("INSERT INTO countries (id, name, "
                                          "iso_a2, iso_a3, geom) VALUES "
                                          "('%1', '%2', '', '', X'%3')")
                               .arg(QLatin1String(id), QLatin1String(name),
                                    QString::fromLatin1(blob.toHex())));
      };
  insert("FIX", "Fixture Island",
         multiPolygonBlob({{9.9, 39.9, 20.1, 39.9, 20.1, 50.1, 9.9, 50.1}}));
  insert("ATL", "Fixture Atoll",
         multiPolygonBlob({{179, -10, 181, -10, 181, -5, 179, -5}}));
}

inline PackManifest writeFixtureBasePack(const QString& directory,
                                         const QColor& color = QColor(20, 60,
                                                                      140))
{
  const QDir dir(directory);
  const QString file = QStringLiteral("bluemarble.sqlite");
  SqliteTileStoreInfo info;
  info.format = QStringLiteral("png");
  info.tileSizePx = 16;
  info.maxLevel = 1;
  info.name = QStringLiteral("Fixture Blue Marble");
  {
    QImage tile(16, 16, QImage::Format_RGB32);
    tile.fill(color);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    tile.save(&buffer, "PNG");
    SqliteTileStoreWriter writer(dir.filePath(file), info);
    for (int level = 0; level <= 1; ++level) {
      for (int y = 0; y < TileKey::rows(level); ++y) {
        for (int x = 0; x < TileKey::columns(level); ++x) {
          writer.putTile(TileKey{level, x, y}, png);
        }
      }
    }
    writer.finish();
  }
  // One island (10°–20°E, 40°–50°N) in the eastern level-0 tile.
  const QString shoreline = QStringLiteral("shoreline.gpkg");
  {
    GeoPackageFeatureWriter writer(dir.filePath(shoreline));
    FeatureTableSpec table;
    table.name = QStringLiteral("land_c");
    table.storageLevel = 0;
    table.minLevel = 0;
    table.maxLevel = -1;
    QByteArray wkb;
    const std::array<double, 10> ring = {10, 40, 20, 40, 20,
                                         50, 10, 50, 10, 40};
    wkb.append(static_cast<char>(1));
    for (const std::uint32_t value : {3U, 1U, 5U}) {
      for (int i = 0; i < 4; ++i) {
        wkb.append(static_cast<char>(
            (value >> (8U * static_cast<unsigned>(i))) & 0xffU));
      }
    }
    for (const double value : ring) {
      wkb.append(reinterpret_cast<const char*>(&value), sizeof value);
    }
    writer.addTable(table);
    writer.putFeature(table.name, TileKey{0, 1, 0}, wkb);
    writer.finish();
  }
  const QString countries = QStringLiteral("countries.gpkg");
  writeFixtureCountries(dir.filePath(countries));
  const std::atomic<bool> notCancelled{false};
  PackManifest manifest;
  manifest.id = basePackId();
  manifest.version = QStringLiteral("test");
  for (const QString& path : {file, shoreline, countries}) {
    PackFile packFile;
    packFile.path = path;
    packFile.sizeBytes = QFileInfo(dir.filePath(path)).size();
    packFile.sha256 = fileSha256(dir.filePath(path), notCancelled);
    manifest.files.push_back(packFile);
  }
  PackDataset dataset;
  dataset.id = QStringLiteral("bluemarble");
  dataset.kind = PackDataset::Kind::Imagery;
  dataset.file = file;
  dataset.name = info.name;
  dataset.attribution = QStringLiteral("Fixture imagery");
  manifest.datasets.push_back(dataset);
  dataset.id = QStringLiteral("shoreline");
  dataset.kind = PackDataset::Kind::Features;
  dataset.file = shoreline;
  dataset.name = QStringLiteral("Fixture shoreline");
  dataset.attribution = QStringLiteral("Fixture shoreline");
  manifest.datasets.push_back(dataset);
  dataset.id = QStringLiteral("countries");
  dataset.kind = PackDataset::Kind::Areas;
  dataset.file = countries;
  dataset.name = QStringLiteral("Fixture countries");
  dataset.attribution = QStringLiteral("Fixture countries");
  manifest.datasets.push_back(dataset);
  QFile json(dir.filePath(PackManifest::fileName()));
  if (json.open(QIODevice::WriteOnly)) {
    json.write(QJsonDocument(manifest.toJson()).toJson());
  }
  return manifest;
}

} // namespace ts::test

#endif // TS_TESTS_FIXTUREPACK_H
