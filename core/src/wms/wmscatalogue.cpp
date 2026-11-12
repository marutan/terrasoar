// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/wms/wmscatalogue.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace ts
{

std::optional<std::vector<WmsCatalogueEntry>>
parseWmsCatalogue(const QByteArray& json, QString* errorString)
{
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  const QJsonObject root = document.object();
  if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
      root.value(QStringLiteral("format")).toString() !=
          QLatin1String(wmsCatalogueFormatName)) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
          "ts::WmsCatalogue", "This is not a WMS catalogue.");
    }
    return std::nullopt;
  }
  if (root.value(QStringLiteral("version")).toInt(0) >
      wmsCatalogueFormatVersion) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
          "ts::WmsCatalogue",
          "The WMS catalogue is of a newer format than this program reads.");
    }
    return std::nullopt;
  }
  std::vector<WmsCatalogueEntry> entries;
  const QJsonArray services = root.value(QStringLiteral("services")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < services.size(); ++i) {
    const QJsonObject service = services.at(i).toObject();
    WmsCatalogueEntry entry;
    entry.title = service.value(QStringLiteral("title")).toString().trimmed();
    entry.url = QUrl(service.value(QStringLiteral("url")).toString().trimmed());
    entry.description =
        service.value(QStringLiteral("description")).toString().trimmed();
    entry.attribution =
        service.value(QStringLiteral("attribution")).toString().trimmed();
    entry.termsUrl =
        QUrl(service.value(QStringLiteral("terms")).toString().trimmed());
    const QJsonValue bulk = service.value(QStringLiteral("allowBulkDownload"));
    if (bulk.isBool()) {
      entry.allowBulkDownload = bulk.toBool();
    }
    const bool web = entry.url.scheme() == QLatin1String("http") ||
                     entry.url.scheme() == QLatin1String("https");
    if (!entry.title.isEmpty() && entry.url.isValid() && web) {
      entries.push_back(entry);
    }
  }
  return entries;
}

namespace
{

QString hostOf(const QUrl& url)
{
  return url.host().toLower();
}

QString pathOf(const QUrl& url)
{
  QString path = url.path();
  while (path.endsWith(QLatin1Char('/'))) {
    path.chop(1);
  }
  return path;
}

} // namespace

const WmsCatalogueEntry*
findWmsCatalogueEntry(const QUrl& url,
                      const std::vector<WmsCatalogueEntry>& catalogue)
{
  const QString host = hostOf(url);
  const QString path = pathOf(url);
  if (host.isEmpty()) {
    return nullptr;
  }
  const WmsCatalogueEntry* sameHost = nullptr;
  for (const WmsCatalogueEntry& entry : catalogue) {
    if (hostOf(entry.url) != host) {
      continue;
    }
    if (pathOf(entry.url) == path) {
      return &entry;
    }
    if (!sameHost) {
      sameHost = &entry;
    }
  }
  return sameHost;
}

BulkDownloadPolicy
bulkDownloadPolicy(const QUrl& url,
                   const std::vector<WmsCatalogueEntry>& catalogue)
{
  const WmsCatalogueEntry* entry = findWmsCatalogueEntry(url, catalogue);
  if (!entry || !entry->allowBulkDownload) {
    return BulkDownloadPolicy::Unknown;
  }
  if (!*entry->allowBulkDownload) {
    return BulkDownloadPolicy::Forbidden;
  }
  const bool samePath = pathOf(entry->url) == pathOf(url);
  return samePath ? BulkDownloadPolicy::Allowed : BulkDownloadPolicy::Unknown;
}

} // namespace ts
