// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A small base pack for tests: solid-colour Blue Marble tiles, levels 0
 * and 1, with a correct manifest.
 */
#ifndef TS_TESTS_FIXTUREPACK_H
#define TS_TESTS_FIXTUREPACK_H

#include <atomic>

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QString>

#include "ts/packs/basepack.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts::test
{

/**
 * Writes a base pack into the empty directory @p directory and returns
 * its manifest. The imagery is @p color everywhere.
 */
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
  const std::atomic<bool> notCancelled{false};
  PackManifest manifest;
  manifest.id = basePackId();
  manifest.version = QStringLiteral("test");
  PackFile packFile;
  packFile.path = file;
  packFile.sizeBytes = QFileInfo(dir.filePath(file)).size();
  packFile.sha256 = fileSha256(dir.filePath(file), notCancelled);
  manifest.files.push_back(packFile);
  PackDataset dataset;
  dataset.id = QStringLiteral("bluemarble");
  dataset.kind = PackDataset::Kind::Imagery;
  dataset.file = file;
  dataset.name = info.name;
  manifest.datasets.push_back(dataset);
  QFile json(dir.filePath(PackManifest::fileName()));
  if (json.open(QIODevice::WriteOnly)) {
    json.write(QJsonDocument(manifest.toJson()).toJson());
  }
  return manifest;
}

} // namespace ts::test

#endif // TS_TESTS_FIXTUREPACK_H
