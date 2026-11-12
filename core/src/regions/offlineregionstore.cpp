// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/regions/offlineregionstore.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace ts
{

namespace
{

QString dateText(const QDateTime& date)
{
  return date.isValid() ? date.toUTC().toString(Qt::ISODate) : QString();
}

QDateTime dateFrom(const QJsonValue& value)
{
  QDateTime date = QDateTime::fromString(value.toString(), Qt::ISODate);
  return date.isValid() ? date.toUTC() : QDateTime();
}

long long integer(const QJsonObject& json, const char* name)
{
  return static_cast<long long>(json.value(QLatin1String(name)).toDouble(0.0));
}

} // namespace

QJsonObject OfflineRegion::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("id"), static_cast<double>(id));
  json.insert(QStringLiteral("name"), name);
  json.insert(QStringLiteral("source"), source);
  json.insert(QStringLiteral("sourceName"), sourceName);
  json.insert(QStringLiteral("geometry"), geometry.toJson());
  json.insert(QStringLiteral("minLevel"), minimumLevel);
  json.insert(QStringLiteral("maxLevel"), maximumLevel);
  json.insert(QStringLiteral("created"), dateText(created));
  json.insert(QStringLiteral("downloaded"), dateText(downloaded));
  json.insert(QStringLiteral("complete"), complete);
  json.insert(QStringLiteral("tiles"), static_cast<double>(tiles));
  json.insert(QStringLiteral("failedTiles"), static_cast<double>(failedTiles));
  json.insert(QStringLiteral("bytes"), static_cast<double>(bytes));
  return json;
}

std::optional<OfflineRegion> OfflineRegion::fromJson(const QJsonObject& json)
{
  OfflineRegion region;
  region.id = integer(json, "id");
  QString error;
  const std::optional<RegionGeometry> geometry = RegionGeometry::fromJson(
      json.value(QStringLiteral("geometry")).toObject(), &error);
  region.source = json.value(QStringLiteral("source")).toObject();
  if (region.id <= 0 || !geometry || region.source.isEmpty()) {
    return std::nullopt;
  }
  region.geometry = *geometry;
  region.name = json.value(QStringLiteral("name")).toString();
  region.sourceName = json.value(QStringLiteral("sourceName")).toString();
  region.minimumLevel =
      std::clamp(json.value(QStringLiteral("minLevel")).toInt(0), 0,
                 TileKey::maximumLevel);
  region.maximumLevel =
      std::clamp(json.value(QStringLiteral("maxLevel")).toInt(0),
                 region.minimumLevel, TileKey::maximumLevel);
  region.created = dateFrom(json.value(QStringLiteral("created")));
  region.downloaded = dateFrom(json.value(QStringLiteral("downloaded")));
  region.complete = json.value(QStringLiteral("complete")).toBool(false);
  region.tiles = integer(json, "tiles");
  region.failedTiles = integer(json, "failedTiles");
  region.bytes = integer(json, "bytes");
  return region;
}

OfflineRegionStore::OfflineRegionStore(QString path) : m_path(std::move(path))
{
}

bool OfflineRegionStore::load(QString* errorString)
{
  m_regions.clear();
  m_nextId = 1;
  QFile file(m_path);
  if (!file.exists()) {
    return true;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    if (errorString) {
      *errorString = file.errorString();
    }
    return false;
  }
  QJsonParseError parseError{};
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parseError);
  const QJsonObject root = document.object();
  if (parseError.error != QJsonParseError::NoError ||
      root.value(QStringLiteral("format")).toString() !=
          QLatin1String(offlineRegionsFormatName)) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
          "ts::OfflineRegionStore", "The list of offline regions is damaged.");
    }
    return false;
  }
  if (root.value(QStringLiteral("version")).toInt(0) >
      offlineRegionsFormatVersion) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
          "ts::OfflineRegionStore",
          "The list of offline regions was written by a newer version of "
          "this program.");
    }
    return false;
  }
  const QJsonArray regions = root.value(QStringLiteral("regions")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < regions.size(); ++i) {
    std::optional<OfflineRegion> region =
        OfflineRegion::fromJson(regions.at(i).toObject());
    if (region && !find(region->id)) {
      m_nextId = std::max(m_nextId, region->id + 1);
      m_regions.push_back(std::move(*region));
    }
  }
  m_nextId = std::max(m_nextId, integer(root, "nextId"));
  return true;
}

bool OfflineRegionStore::save(QString* errorString) const
{
  QJsonArray regions;
  for (const OfflineRegion& region : m_regions) {
    regions.append(region.toJson());
  }
  QJsonObject root;
  root.insert(QStringLiteral("format"),
              QLatin1String(offlineRegionsFormatName));
  root.insert(QStringLiteral("version"), offlineRegionsFormatVersion);
  root.insert(QStringLiteral("nextId"), static_cast<double>(m_nextId));
  root.insert(QStringLiteral("regions"), regions);
  QDir().mkpath(QFileInfo(m_path).absolutePath());
  QSaveFile file(m_path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(QJsonDocument(root).toJson()) < 0 || !file.commit()) {
    if (errorString) {
      *errorString = file.errorString();
    }
    return false;
  }
  return true;
}

long long OfflineRegionStore::add(OfflineRegion region)
{
  region.id = m_nextId++;
  m_regions.push_back(std::move(region));
  return m_regions.back().id;
}

OfflineRegion* OfflineRegionStore::find(long long id)
{
  for (OfflineRegion& region : m_regions) {
    if (region.id == id) {
      return &region;
    }
  }
  return nullptr;
}

const OfflineRegion* OfflineRegionStore::find(long long id) const
{
  for (const OfflineRegion& region : m_regions) {
    if (region.id == id) {
      return &region;
    }
  }
  return nullptr;
}

bool OfflineRegionStore::remove(long long id)
{
  const std::vector<OfflineRegion>::iterator found = std::find_if(
      m_regions.begin(), m_regions.end(),
      [id](const OfflineRegion& region) { return region.id == id; });
  if (found == m_regions.end()) {
    return false;
  }
  m_regions.erase(found);
  return true;
}

} // namespace ts
