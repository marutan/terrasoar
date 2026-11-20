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
#include "ts/orbits/elementsource.h"

#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrlQuery>

#include "ts/network/httpcache.h"
#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

/** A group: CelesTrak's id, its name and category for translation. */
struct GroupEntry {
  const char* id;
  const char* name;
  const char* category;
};

/** CelesTrak's groups (the current-data page's), in its order. */
const std::vector<GroupEntry>& groupTable()
{
  // clang-format off
  static const std::vector<GroupEntry> table{
    {"stations", QT_TRANSLATE_NOOP("ts::Celestrak", "Space stations"), QT_TRANSLATE_NOOP("ts::Celestrak", "Special interest")},
    {"visual", QT_TRANSLATE_NOOP("ts::Celestrak", "100 (or so) brightest"), QT_TRANSLATE_NOOP("ts::Celestrak", "Special interest")},
    {"active", QT_TRANSLATE_NOOP("ts::Celestrak", "Active satellites"), QT_TRANSLATE_NOOP("ts::Celestrak", "Special interest")},
    {"last-30-days", QT_TRANSLATE_NOOP("ts::Celestrak", "Launched in the last 30 days"), QT_TRANSLATE_NOOP("ts::Celestrak", "Special interest")},
    {"analyst", QT_TRANSLATE_NOOP("ts::Celestrak", "Analyst satellites"), QT_TRANSLATE_NOOP("ts::Celestrak", "Special interest")},
    {"weather", QT_TRANSLATE_NOOP("ts::Celestrak", "Weather"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"noaa", QT_TRANSLATE_NOOP("ts::Celestrak", "NOAA"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"goes", QT_TRANSLATE_NOOP("ts::Celestrak", "GOES"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"resource", QT_TRANSLATE_NOOP("ts::Celestrak", "Earth resources"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"sarsat", QT_TRANSLATE_NOOP("ts::Celestrak", "Search and rescue (SARSAT)"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"dmc", QT_TRANSLATE_NOOP("ts::Celestrak", "Disaster Monitoring"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"tdrss", QT_TRANSLATE_NOOP("ts::Celestrak", "Tracking and Data Relay (TDRSS)"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"argos", QT_TRANSLATE_NOOP("ts::Celestrak", "ARGOS Data Collection System"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"planet", QT_TRANSLATE_NOOP("ts::Celestrak", "Planet"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"spire", QT_TRANSLATE_NOOP("ts::Celestrak", "Spire"), QT_TRANSLATE_NOOP("ts::Celestrak", "Weather and Earth resources")},
    {"geo", QT_TRANSLATE_NOOP("ts::Celestrak", "Active geosynchronous"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"intelsat", QT_TRANSLATE_NOOP("ts::Celestrak", "Intelsat"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"ses", QT_TRANSLATE_NOOP("ts::Celestrak", "SES"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"eutelsat", QT_TRANSLATE_NOOP("ts::Celestrak", "Eutelsat"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"iridium-NEXT", QT_TRANSLATE_NOOP("ts::Celestrak", "Iridium NEXT"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"starlink", QT_TRANSLATE_NOOP("ts::Celestrak", "Starlink"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"oneweb", QT_TRANSLATE_NOOP("ts::Celestrak", "OneWeb"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"kuiper", QT_TRANSLATE_NOOP("ts::Celestrak", "Kuiper"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"qianfan", QT_TRANSLATE_NOOP("ts::Celestrak", "Qianfan"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"orbcomm", QT_TRANSLATE_NOOP("ts::Celestrak", "Orbcomm"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"globalstar", QT_TRANSLATE_NOOP("ts::Celestrak", "Globalstar"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"swarm", QT_TRANSLATE_NOOP("ts::Celestrak", "Swarm"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"amateur", QT_TRANSLATE_NOOP("ts::Celestrak", "Amateur radio"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"satnogs", QT_TRANSLATE_NOOP("ts::Celestrak", "SatNOGS"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"x-comm", QT_TRANSLATE_NOOP("ts::Celestrak", "Experimental communications"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"other-comm", QT_TRANSLATE_NOOP("ts::Celestrak", "Other communications"), QT_TRANSLATE_NOOP("ts::Celestrak", "Communications")},
    {"gnss", QT_TRANSLATE_NOOP("ts::Celestrak", "GNSS"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"gps-ops", QT_TRANSLATE_NOOP("ts::Celestrak", "GPS operational"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"glo-ops", QT_TRANSLATE_NOOP("ts::Celestrak", "GLONASS operational"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"galileo", QT_TRANSLATE_NOOP("ts::Celestrak", "Galileo"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"beidou", QT_TRANSLATE_NOOP("ts::Celestrak", "Beidou"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"sbas", QT_TRANSLATE_NOOP("ts::Celestrak", "Satellite-based augmentation (SBAS)"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"nnss", QT_TRANSLATE_NOOP("ts::Celestrak", "Navy Navigation Satellite System"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"musson", QT_TRANSLATE_NOOP("ts::Celestrak", "Russian LEO navigation"), QT_TRANSLATE_NOOP("ts::Celestrak", "Navigation")},
    {"science", QT_TRANSLATE_NOOP("ts::Celestrak", "Space and Earth science"), QT_TRANSLATE_NOOP("ts::Celestrak", "Scientific")},
    {"geodetic", QT_TRANSLATE_NOOP("ts::Celestrak", "Geodetic"), QT_TRANSLATE_NOOP("ts::Celestrak", "Scientific")},
    {"engineering", QT_TRANSLATE_NOOP("ts::Celestrak", "Engineering"), QT_TRANSLATE_NOOP("ts::Celestrak", "Scientific")},
    {"education", QT_TRANSLATE_NOOP("ts::Celestrak", "Education"), QT_TRANSLATE_NOOP("ts::Celestrak", "Scientific")},
    {"military", QT_TRANSLATE_NOOP("ts::Celestrak", "Miscellaneous military"), QT_TRANSLATE_NOOP("ts::Celestrak", "Miscellaneous")},
    {"radar", QT_TRANSLATE_NOOP("ts::Celestrak", "Radar calibration"), QT_TRANSLATE_NOOP("ts::Celestrak", "Miscellaneous")},
    {"cubesat", QT_TRANSLATE_NOOP("ts::Celestrak", "CubeSats"), QT_TRANSLATE_NOOP("ts::Celestrak", "Miscellaneous")},
    {"other", QT_TRANSLATE_NOOP("ts::Celestrak", "Other satellites"), QT_TRANSLATE_NOOP("ts::Celestrak", "Miscellaneous")},
  };
  // clang-format on
  return table;
}

/** One of the table's texts, translated. */
QString tableText(const char* text)
{
  // ts-notr: the table's strings are marked with QT_TRANSLATE_NOOP.
  return QCoreApplication::translate("ts::Celestrak", text);
}

int64_t nowS(const ElementDownloadSettings& settings)
{
  return settings.clock ? settings.clock() : QDateTime::currentSecsSinceEpoch();
}

/** The cache files of @p url: its data and what is known about it. */
QString cacheBase(const QUrl& url, const ElementDownloadSettings& settings)
{
  if (settings.cacheDirectory.isEmpty()) {
    return QString();
  }
  const QByteArray hash = QCryptographicHash::hash(
      url.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha1);
  return QDir(settings.cacheDirectory)
      .filePath(QString::fromLatin1(hash.toHex()));
}

/** What the cache keeps beside the data. */
struct CacheEntry {
  QByteArray data;
  int64_t downloadedS = 0;
  QByteArray etag;
  QByteArray lastModified;
};

std::optional<CacheEntry> readCache(const QString& base)
{
  if (base.isEmpty()) {
    return std::nullopt;
  }
  QFile data(base + QStringLiteral(".elements"));
  QFile meta(base + QStringLiteral(".json"));
  if (!data.open(QIODevice::ReadOnly) || !meta.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  const QJsonObject json = QJsonDocument::fromJson(meta.readAll()).object();
  CacheEntry entry;
  entry.data = data.readAll();
  entry.downloadedS =
      static_cast<int64_t>(json.value(QLatin1String("downloaded")).toDouble());
  entry.etag = json.value(QLatin1String("etag")).toString().toLatin1();
  entry.lastModified =
      json.value(QLatin1String("lastModified")).toString().toLatin1();
  return entry;
}

void writeMeta(const QString& base, const QUrl& url, const CacheEntry& entry)
{
  QJsonObject json;
  json.insert(QStringLiteral("url"), url.toString());
  json.insert(QStringLiteral("downloaded"),
              static_cast<double>(entry.downloadedS));
  json.insert(QStringLiteral("etag"), QString::fromLatin1(entry.etag));
  json.insert(QStringLiteral("lastModified"),
              QString::fromLatin1(entry.lastModified));
  QSaveFile meta(base + QStringLiteral(".json"));
  if (meta.open(QIODevice::WriteOnly)) {
    meta.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    meta.commit();
  }
}

void writeCache(const QString& base, const QUrl& url, const CacheEntry& entry)
{
  if (base.isEmpty()) {
    return;
  }
  QDir().mkpath(QFileInfo(base).absolutePath());
  QSaveFile data(base + QStringLiteral(".elements"));
  if (!data.open(QIODevice::WriteOnly)) {
    return;
  }
  data.write(entry.data);
  if (data.commit()) {
    writeMeta(base, url, entry);
  }
}

ElementDownload fromCache(const CacheEntry& entry, ElementOrigin origin)
{
  ElementDownload download;
  download.sets = parseElementSets(entry.data);
  download.origin = origin;
  download.downloadedS = entry.downloadedS;
  return download;
}

QString responseProblem(const HttpResponse& response)
{
  if (response.error != NetworkError::None) {
    return QCoreApplication::translate("ts::ElementSource",
                                       "Cannot reach the server: %1")
        .arg(response.errorString);
  }
  return QCoreApplication::translate("ts::ElementSource",
                                     "The server answered with HTTP %1.")
      .arg(response.status);
}

} // namespace

std::vector<CelestrakGroup> celestrakGroups()
{
  std::vector<CelestrakGroup> groups;
  for (const GroupEntry& entry : groupTable()) {
    groups.push_back(CelestrakGroup{QString::fromLatin1(entry.id),
                                    tableText(entry.name),
                                    tableText(entry.category)});
  }
  return groups;
}

QString celestrakGroupName(const QString& id)
{
  for (const GroupEntry& entry : groupTable()) {
    if (id == QLatin1String(entry.id)) {
      return tableText(entry.name);
    }
  }
  return id;
}

QUrl celestrakBaseUrl()
{
  return QUrl(QStringLiteral("https://celestrak.org/NORAD/elements/gp.php"));
}

namespace
{

QUrl celestrakQuery(const QString& key, const QString& value,
                    ElementFormat format, const QUrl& base)
{
  QUrl url = base.isEmpty() ? celestrakBaseUrl() : base;
  QUrlQuery query;
  query.addQueryItem(key, value);
  query.addQueryItem(QStringLiteral("FORMAT"), format == ElementFormat::Tle
                                                   ? QStringLiteral("tle")
                                                   : QStringLiteral("json"));
  url.setQuery(query);
  return url;
}

} // namespace

QUrl celestrakGroupUrl(const QString& group, ElementFormat format,
                       const QUrl& base)
{
  return celestrakQuery(QStringLiteral("GROUP"), group, format, base);
}

QUrl celestrakSatelliteUrl(int32_t catalogueNumber, ElementFormat format,
                           const QUrl& base)
{
  return celestrakQuery(QStringLiteral("CATNR"),
                        QString::number(catalogueNumber), format, base);
}

std::optional<ElementDownload>
cachedElementSets(const QUrl& url, const ElementDownloadSettings& settings)
{
  const std::optional<CacheEntry> entry = readCache(cacheBase(url, settings));
  if (!entry) {
    return std::nullopt;
  }
  return fromCache(*entry, ElementOrigin::StaleCache);
}

ElementDownload downloadElementSets(NetworkClient& client, const QUrl& url,
                                    const ElementDownloadSettings& settings,
                                    bool force,
                                    const std::atomic<bool>& cancelled)
{
  const QString base = cacheBase(url, settings);
  std::optional<CacheEntry> cached = readCache(base);
  const int64_t now = nowS(settings);
  if (cached && !force && now - cached->downloadedS < settings.minimumAgeS &&
      now >= cached->downloadedS) {
    return fromCache(*cached, ElementOrigin::Cache);
  }
  HttpRequest request;
  request.url = url;
  if (cached && !force) {
    request.headers = revalidationHeaders(cached->etag, cached->lastModified);
  }
  const HttpResponse response = client.fetch(request, cancelled);
  if (response.error == NetworkError::None && response.status == 304 &&
      cached) {
    cached->downloadedS = now;
    writeMeta(base, url, *cached);
    return fromCache(*cached, ElementOrigin::Server);
  }
  QString problem;
  if (response.error == NetworkError::None && response.status >= 200 &&
      response.status < 300) {
    ElementDownload download;
    download.sets = parseElementSets(response.body);
    if (download.ok()) {
      CacheEntry entry;
      entry.data = response.body;
      entry.downloadedS = now;
      entry.etag = response.header("etag");
      entry.lastModified = response.header("last-modified");
      writeCache(base, url, entry);
      download.origin = ElementOrigin::Server;
      download.downloadedS = now;
      return download;
    }
    problem =
        QCoreApplication::translate(
            "ts::ElementSource", "The server's answer has no element sets: %1")
            .arg(QString::fromUtf8(response.body.left(200)).simplified());
  } else {
    problem = responseProblem(response);
  }
  ElementDownload download;
  if (cached) {
    download = fromCache(*cached, ElementOrigin::StaleCache);
  }
  download.errorString = problem;
  return download;
}

} // namespace ts
