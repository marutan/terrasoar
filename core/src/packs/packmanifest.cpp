// SPDX-License-Identifier: GPL-2.0-or-later
/*
  Terra Soar - A virtual globe

  Copyright (C) 2026 Peter Howkins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "ts/packs/packmanifest.h"

#include <algorithm>
#include <cstdint>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace ts
{

namespace
{

const char* kindName(PackDataset::Kind kind)
{
  switch (kind) {
  case PackDataset::Kind::Features:
    return "features";
  case PackDataset::Kind::Areas:
    return "areas";
  case PackDataset::Kind::Elevation:
    return "elevation";
  case PackDataset::Kind::Geoid:
    return "geoid";
  case PackDataset::Kind::Imagery:
    break;
  }
  return "imagery";
}

bool isSafeRelativePath(const QString& path)
{
  // Only plain relative paths inside the pack: no absolute paths, drive
  // letters, backslashes or "..".
  if (path.isEmpty() || path.startsWith(QLatin1Char('/')) ||
      path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))) {
    return false;
  }
  const QStringList parts = path.split(QLatin1Char('/'));
  return std::none_of(parts.cbegin(), parts.cend(), [](const QString& part) {
    return part.isEmpty() || part == QLatin1String("..") ||
           part == QLatin1String(".");
  });
}

QString text(const QJsonObject& json, const char* name)
{
  return json.value(QLatin1String(name)).toString();
}

bool fail(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return false;
}

} // namespace

QString PackManifest::fileName()
{
  return QStringLiteral("manifest.json"); // ts-notr: file name
}

std::optional<PackManifest> PackManifest::read(const QString& directory,
                                               QString* errorString)
{
  QFile file(QDir(directory).filePath(fileName()));
  if (!file.open(QIODevice::ReadOnly)) {
    fail(errorString, QStringLiteral("Cannot read %1: %2")
                          .arg(file.fileName(), file.errorString()));
    return std::nullopt;
  }
  QJsonParseError parseError{};
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail(errorString, QStringLiteral("%1 is not a JSON object: %2")
                          .arg(file.fileName(), parseError.errorString()));
    return std::nullopt;
  }
  return fromJson(document.object(), errorString);
}

namespace
{

/**
 * Reads the optional "datumShifts" of @p json into @p manifest, whose files
 * are already read; false and @p errorString if one is malformed.
 */
bool readDatumShifts(const QJsonObject& json, PackManifest* manifest,
                     QString* errorString)
{
  // Datum shift grids: apart from the datasets, which versions 0.0.2
  // and earlier reject when they don't know the kind.
  const QJsonArray shifts = json.value(QStringLiteral("datumShifts")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < shifts.size(); ++i) {
    const QJsonObject entry = shifts.at(i).toObject();
    PackDataset shift;
    shift.id = text(entry, "id");
    shift.file = text(entry, "file");
    shift.name = text(entry, "name");
    shift.licence = text(entry, "licence");
    shift.attribution = text(entry, "attribution");
    const QJsonObject source = entry.value(QStringLiteral("source")).toObject();
    shift.source.name = text(source, "name");
    shift.source.version = text(source, "version");
    shift.source.url = text(source, "url");
    shift.source.sha256 = text(source, "sha256");
    const bool listed =
        std::any_of(manifest->files.begin(), manifest->files.end(),
                    [&shift](const PackFile& packFile) {
                      return packFile.path == shift.file;
                    });
    if (shift.id.isEmpty() || !listed) {
      fail(errorString,
           QStringLiteral("Datum shift %1 in the pack manifest has no id or "
                          "its file is not listed")
               .arg(i));
      return false;
    }
    manifest->datumShifts.push_back(shift);
  }

  return true;
}

/**
 * Reads the "notices", and the optional "name" and "layers", of @p json
 * into @p manifest.
 */
bool readNoticesAndLayers(const QJsonObject& json, PackManifest* manifest,
                          QString* errorString)
{
  const QJsonArray notices = json.value(QStringLiteral("notices")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < notices.size(); ++i) {
    const QString path = notices.at(i).toString();
    if (!isSafeRelativePath(path)) {
      fail(errorString,
           QStringLiteral("Bad notice entry %1 in the pack manifest").arg(i));
      return false;
    }
    manifest->notices.push_back(path);
  }
  manifest->name = text(json, "name");
  const QJsonArray layers = json.value(QStringLiteral("layers")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < layers.size(); ++i) {
    if (layers.at(i).isObject()) {
      manifest->layers.append(layers.at(i));
    }
  }
  return true;
}

} // namespace

std::optional<PackManifest> PackManifest::fromJson(const QJsonObject& json,
                                                   QString* errorString)
{
  const int32_t format = json.value(QStringLiteral("format")).toInt(0);
  if (format < 1 || format > currentFormat) {
    fail(errorString,
         QStringLiteral("Unsupported pack manifest format %1").arg(format));
    return std::nullopt;
  }
  PackManifest manifest;
  manifest.id = text(json, "id");
  manifest.version = text(json, "version");
  if (manifest.id.isEmpty()) {
    fail(errorString, QStringLiteral("The pack manifest has no id"));
    return std::nullopt;
  }

  const QJsonArray files = json.value(QStringLiteral("files")).toArray();
  // An index loop: QJsonArray's iterator differs between Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < files.size(); ++i) {
    const QJsonObject entry = files.at(i).toObject();
    PackFile packFile;
    packFile.path = text(entry, "path");
    packFile.sha256 = text(entry, "sha256").toLower();
    packFile.sizeBytes =
        static_cast<qint64>(entry.value(QStringLiteral("bytes")).toDouble(-1));
    if (!isSafeRelativePath(packFile.path) || packFile.sha256.size() != 64 ||
        packFile.sizeBytes < 0) {
      fail(errorString,
           QStringLiteral("Bad file entry %1 in the pack manifest").arg(i));
      return std::nullopt;
    }
    manifest.files.push_back(packFile);
  }

  const QJsonArray datasets = json.value(QStringLiteral("datasets")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < datasets.size(); ++i) {
    const QJsonObject entry = datasets.at(i).toObject();
    PackDataset dataset;
    dataset.id = text(entry, "id");
    const QString kind = text(entry, "kind");
    if (kind == QLatin1String("imagery")) {
      dataset.kind = PackDataset::Kind::Imagery;
    } else if (kind == QLatin1String("features")) {
      dataset.kind = PackDataset::Kind::Features;
    } else if (kind == QLatin1String("areas")) {
      dataset.kind = PackDataset::Kind::Areas;
    } else if (kind == QLatin1String("elevation")) {
      dataset.kind = PackDataset::Kind::Elevation;
    } else if (kind == QLatin1String("geoid")) {
      dataset.kind = PackDataset::Kind::Geoid;
    } else {
      fail(errorString,
           QStringLiteral("Unknown dataset kind \"%1\" in the pack manifest")
               .arg(kind));
      return std::nullopt;
    }
    dataset.file = text(entry, "file");
    dataset.name = text(entry, "name");
    dataset.licence = text(entry, "licence");
    dataset.attribution = text(entry, "attribution");
    const QJsonObject source = entry.value(QStringLiteral("source")).toObject();
    dataset.source.name = text(source, "name");
    dataset.source.version = text(source, "version");
    dataset.source.url = text(source, "url");
    dataset.source.sha256 = text(source, "sha256");
    dataset.replaces = text(entry, "replaces");
    dataset.variantOf = text(entry, "variantOf");
    dataset.variant = text(entry, "variant");
    bool listed = false;
    for (const PackFile& packFile : manifest.files) {
      listed = listed || packFile.path == dataset.file;
    }
    if (dataset.id.isEmpty() || !listed) {
      fail(errorString,
           QStringLiteral("Dataset %1 in the pack manifest has no id or its "
                          "file is not listed")
               .arg(i));
      return std::nullopt;
    }
    manifest.datasets.push_back(dataset);
  }

  if (!readDatumShifts(json, &manifest, errorString)) {
    return std::nullopt;
  }

  if (!readNoticesAndLayers(json, &manifest, errorString)) {
    return std::nullopt;
  }
  return manifest;
}

QJsonObject PackManifest::toJson() const
{
  QJsonArray fileArray;
  for (const PackFile& packFile : files) {
    QJsonObject entry;
    entry.insert(QStringLiteral("path"), packFile.path);
    entry.insert(QStringLiteral("sha256"), packFile.sha256);
    entry.insert(QStringLiteral("bytes"),
                 static_cast<double>(packFile.sizeBytes));
    fileArray.append(entry);
  }
  QJsonArray datasetArray;
  for (const PackDataset& dataset : datasets) {
    QJsonObject source;
    source.insert(QStringLiteral("name"), dataset.source.name);
    source.insert(QStringLiteral("version"), dataset.source.version);
    source.insert(QStringLiteral("url"), dataset.source.url);
    source.insert(QStringLiteral("sha256"), dataset.source.sha256);
    QJsonObject entry;
    entry.insert(QStringLiteral("id"), dataset.id);
    entry.insert(QStringLiteral("kind"),
                 QString::fromLatin1(kindName(dataset.kind)));
    entry.insert(QStringLiteral("file"), dataset.file);
    entry.insert(QStringLiteral("name"), dataset.name);
    entry.insert(QStringLiteral("licence"), dataset.licence);
    entry.insert(QStringLiteral("attribution"), dataset.attribution);
    entry.insert(QStringLiteral("source"), source);
    if (!dataset.replaces.isEmpty()) {
      entry.insert(QStringLiteral("replaces"), dataset.replaces);
    }
    if (!dataset.variantOf.isEmpty()) {
      entry.insert(QStringLiteral("variantOf"), dataset.variantOf);
      entry.insert(QStringLiteral("variant"), dataset.variant);
    }
    datasetArray.append(entry);
  }
  QJsonArray shiftArray;
  for (const PackDataset& shift : datumShifts) {
    QJsonObject source;
    source.insert(QStringLiteral("name"), shift.source.name);
    source.insert(QStringLiteral("version"), shift.source.version);
    source.insert(QStringLiteral("url"), shift.source.url);
    source.insert(QStringLiteral("sha256"), shift.source.sha256);
    QJsonObject entry;
    entry.insert(QStringLiteral("id"), shift.id);
    entry.insert(QStringLiteral("file"), shift.file);
    entry.insert(QStringLiteral("name"), shift.name);
    entry.insert(QStringLiteral("licence"), shift.licence);
    entry.insert(QStringLiteral("attribution"), shift.attribution);
    entry.insert(QStringLiteral("source"), source);
    shiftArray.append(entry);
  }
  QJsonArray noticeArray;
  for (const QString& notice : notices) {
    noticeArray.append(notice);
  }
  QJsonObject json;
  json.insert(QStringLiteral("format"), currentFormat);
  json.insert(QStringLiteral("id"), id);
  json.insert(QStringLiteral("version"), version);
  json.insert(QStringLiteral("files"), fileArray);
  json.insert(QStringLiteral("datasets"), datasetArray);
  if (!shiftArray.isEmpty()) {
    json.insert(QStringLiteral("datumShifts"), shiftArray);
  }
  json.insert(QStringLiteral("notices"), noticeArray);
  if (!name.isEmpty()) {
    json.insert(QStringLiteral("name"), name);
  }
  if (!layers.isEmpty()) {
    json.insert(QStringLiteral("layers"), layers);
  }
  return json;
}

PackVerification PackManifest::verify(const QString& directory,
                                      const std::atomic<bool>& cancelled) const
{
  PackVerification result;
  const QDir dir(directory);
  for (const PackFile& packFile : files) {
    if (cancelled.load()) {
      result.problems.push_back(QStringLiteral("Verification cancelled"));
      return result;
    }
    const QFileInfo info(dir.filePath(packFile.path));
    if (!info.isFile()) {
      result.problems.push_back(
          QStringLiteral("%1: missing").arg(packFile.path));
      continue;
    }
    if (info.size() != packFile.sizeBytes) {
      result.problems.push_back(QStringLiteral("%1: %2 bytes, expected %3")
                                    .arg(packFile.path)
                                    .arg(info.size())
                                    .arg(packFile.sizeBytes));
      continue;
    }
    const QString sha256 = fileSha256(info.filePath(), cancelled);
    if (sha256 != packFile.sha256) {
      result.problems.push_back(
          QStringLiteral("%1: checksum mismatch").arg(packFile.path));
    }
  }
  result.ok = result.problems.empty() && !cancelled.load();
  return result;
}

std::optional<PackDataset> PackManifest::dataset(const QString& datasetId) const
{
  for (const PackDataset& entry : datasets) {
    if (entry.id == datasetId) {
      return entry;
    }
  }
  return std::nullopt;
}

QString fileSha256(const QString& path, const std::atomic<bool>& cancelled)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QString();
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  constexpr qint64 chunkBytes = qint64{1} << 20;
  while (!file.atEnd()) {
    if (cancelled.load()) {
      return QString();
    }
    const QByteArray chunk = file.read(chunkBytes);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
      return QString();
    }
    hash.addData(chunk);
  }
  return QString::fromLatin1(hash.result().toHex());
}

} // namespace ts
