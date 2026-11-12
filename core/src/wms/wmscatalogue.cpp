// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/wms/wmscatalogue.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
    const bool web = entry.url.scheme() == QLatin1String("http") ||
                     entry.url.scheme() == QLatin1String("https");
    if (!entry.title.isEmpty() && entry.url.isValid() && web) {
      entries.push_back(entry);
    }
  }
  return entries;
}

} // namespace ts
