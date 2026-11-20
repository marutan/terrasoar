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
#include "ts/xyz/xyzsource.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>

#include "ts/tiling/tilekey.h"

namespace ts
{

namespace
{

constexpr double pi = 3.14159265358979323846;
/** Web Mercator's sphere, metres. */
constexpr double mercatorRadius = 6378137.0;
/** Web Mercator's latitude limit, degrees. */
constexpr double mercatorLimitDeg = 85.0511287798066;
/** Finest zoom a source may name (rows and columns fit 32 bits). */
constexpr int32_t deepestZoom = 30;

const QRegularExpression& variablePattern()
{
  static const QRegularExpression pattern(QStringLiteral("\\{([^}]*)\\}"));
  return pattern;
}

std::optional<XyzSource> fail(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return std::nullopt;
}

/** Column of the tile at zoom @p zoom holding longitude @p lonDeg. */
// A longitude, then the number of columns.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int64_t columnAt(double lonDeg, int64_t tiles)
{
  const double x = (lonDeg + 180.0) / 360.0 * static_cast<double>(tiles);
  return std::clamp(static_cast<int64_t>(std::floor(x)), int64_t{0}, tiles - 1);
}

/** Row (from the north) of the tile holding latitude @p latDeg. */
// A latitude, then the number of rows.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int64_t rowAt(double latDeg, int64_t tiles)
{
  const double lat =
      std::clamp(latDeg, -mercatorLimitDeg, mercatorLimitDeg) * pi / 180.0;
  const double y =
      (1.0 - std::asinh(std::tan(lat)) / pi) / 2.0 * static_cast<double>(tiles);
  return std::clamp(static_cast<int64_t>(std::floor(y)), int64_t{0}, tiles - 1);
}

QStringList stringList(const QJsonValue& value)
{
  QStringList list;
  const QJsonArray array = value.toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < array.size(); ++i) {
    const QString item = array.at(i).toString().trimmed();
    if (!item.isEmpty()) {
      list.append(item);
    }
  }
  return list;
}

} // namespace

QString checkXyzTemplate(const QString& urlTemplate,
                         const QStringList& subdomains)
{
  const QString trimmed = urlTemplate.trimmed();
  if (!trimmed.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) &&
      !trimmed.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
    return QCoreApplication::translate(
        "ts::XyzSource", "The address must start with http:// or https://.");
  }
  bool z = false;
  bool x = false;
  bool y = false;
  bool s = false;
  QRegularExpressionMatchIterator matches =
      variablePattern().globalMatch(trimmed);
  while (matches.hasNext()) {
    const QString name = matches.next().captured(1).toLower();
    if (name == QLatin1String("z")) {
      z = true;
    } else if (name == QLatin1String("x")) {
      x = true;
    } else if (name == QLatin1String("y") || name == QLatin1String("-y")) {
      y = true;
    } else if (name == QLatin1String("s")) {
      s = true;
    } else {
      return QCoreApplication::translate(
                 "ts::XyzSource",
                 "The address has a variable {%1} this program doesn't "
                 "know. It understands {z}, {x}, {y}, {-y} and {s}.")
          .arg(name);
    }
  }
  if (!z || !x || !y) {
    return QCoreApplication::translate(
        "ts::XyzSource",
        "The address must contain {z}, {x} and {y} (or {-y}) where the "
        "zoom, column and row go.");
  }
  if (s && subdomains.isEmpty()) {
    return QCoreApplication::translate(
        "ts::XyzSource",
        "The address contains {s}: give the subdomains it stands for "
        "(such as a, b, c).");
  }
  return QString();
}

QString xyzFormatFor(const QString& urlTemplate)
{
  QString path = QUrl(urlTemplate, QUrl::TolerantMode).path().toLower();
  if (path.endsWith(QLatin1String(".jpg")) ||
      path.endsWith(QLatin1String(".jpeg"))) {
    return QStringLiteral("image/jpeg");
  }
  if (path.endsWith(QLatin1String(".webp"))) {
    return QStringLiteral("image/webp");
  }
  return QStringLiteral("image/png");
}

// Zoom, column, row: the order of the template's {z}/{x}/{y}.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QUrl xyzTileUrl(const XyzSource& source, int32_t zoom, int64_t column,
                int64_t row)
{
  const int64_t tiles = int64_t{1} << std::clamp(zoom, 0, deepestZoom);
  const int64_t southRow = tiles - 1 - row;
  QString filled;
  int32_t from = 0;
  QRegularExpressionMatchIterator matches =
      variablePattern().globalMatch(source.urlTemplate);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    const QString name = match.captured(1).toLower();
    QString value = match.captured(0);
    if (name == QLatin1String("z")) {
      value = QString::number(zoom);
    } else if (name == QLatin1String("x")) {
      value = QString::number(column);
    } else if (name == QLatin1String("y")) {
      value = QString::number(source.tms ? southRow : row);
    } else if (name == QLatin1String("-y")) {
      value = QString::number(southRow);
    } else if (name == QLatin1String("s") && !source.subdomains.isEmpty()) {
      // The same tile always from the same subdomain, so caches along the
      // way see it once.
      const int64_t count = source.subdomains.size();
      value = source.subdomains.at(
          static_cast<int32_t>(((column + row) % count + count) % count));
    }
    filled += source.urlTemplate.mid(
        from, static_cast<int32_t>(match.capturedStart()) - from);
    filled += value;
    from = static_cast<int32_t>(match.capturedEnd());
  }
  filled += source.urlTemplate.mid(from);
  return QUrl(filled, QUrl::TolerantMode);
}

QUrl xyzServiceUrl(const QString& urlTemplate, const QStringList& subdomains)
{
  QString address = urlTemplate.trimmed();
  address.replace(QLatin1String("{s}"),
                  subdomains.isEmpty() ? QString() : subdomains.first(),
                  Qt::CaseInsensitive);
  const QUrl url(address, QUrl::TolerantMode);
  QUrl service;
  service.setScheme(url.scheme());
  service.setHost(url.host());
  service.setPort(url.port());
  service.setPath(QStringLiteral("/"));
  return service;
}

QString XyzSource::id() const
{
  return QStringLiteral("xyz:%1|%2|%3|%4|%5")
      .arg(urlTemplate, tms ? QStringLiteral("tms") : QStringLiteral("xyz"))
      .arg(minimumZoom)
      .arg(maximumZoom)
      .arg(tileSizePx);
}

QJsonObject XyzSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), QStringLiteral("xyz"));
  json.insert(QStringLiteral("template"), urlTemplate);
  if (!subdomains.isEmpty()) {
    json.insert(QStringLiteral("subdomains"),
                QJsonArray::fromStringList(subdomains));
  }
  if (tms) {
    json.insert(QStringLiteral("tms"), true);
  }
  json.insert(QStringLiteral("minZoom"), minimumZoom);
  json.insert(QStringLiteral("maxZoom"), maximumZoom);
  json.insert(QStringLiteral("tileSize"), tileSizePx);
  json.insert(QStringLiteral("format"), format);
  if (bounds) {
    json.insert(QStringLiteral("bounds"),
                QJsonArray{bounds->westDeg, bounds->southDeg, bounds->eastDeg,
                           bounds->northDeg});
  }
  if (!service.isEmpty()) {
    json.insert(QStringLiteral("service"),
                service.toString(QUrl::FullyEncoded));
  }
  json.insert(QStringLiteral("title"), title);
  json.insert(QStringLiteral("attribution"), attribution);
  return json;
}

std::optional<XyzSource> XyzSource::fromJson(const QJsonObject& json,
                                             QString* errorString)
{
  if (json.value(QStringLiteral("kind")).toString() != QLatin1String("xyz")) {
    return fail(errorString,
                QCoreApplication::translate("ts::XyzSource",
                                            "This is not an XYZ source."));
  }
  XyzSource source;
  source.urlTemplate =
      json.value(QStringLiteral("template")).toString().trimmed();
  source.subdomains = stringList(json.value(QStringLiteral("subdomains")));
  const QString problem =
      checkXyzTemplate(source.urlTemplate, source.subdomains);
  if (!problem.isEmpty()) {
    return fail(errorString, problem);
  }
  source.tms = json.value(QStringLiteral("tms")).toBool(false);
  source.minimumZoom = json.value(QStringLiteral("minZoom")).toInt(0);
  source.maximumZoom = json.value(QStringLiteral("maxZoom")).toInt(19);
  if (source.minimumZoom < 0 || source.maximumZoom > deepestZoom ||
      source.minimumZoom > source.maximumZoom) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::XyzSource",
                    "The zoom levels must be from 0 to %1, the first no "
                    "deeper than the last.")
                    .arg(deepestZoom));
  }
  source.tileSizePx = json.value(QStringLiteral("tileSize")).toInt(256);
  if (source.tileSizePx < 64 || source.tileSizePx > 1024) {
    return fail(
        errorString,
        QCoreApplication::translate(
            "ts::XyzSource", "The tile size must be from 64 to 1024 pixels."));
  }
  source.format = json.value(QStringLiteral("format"))
                      .toString(xyzFormatFor(source.urlTemplate));
  const QJsonArray boundsArray = json.value(QStringLiteral("bounds")).toArray();
  if (boundsArray.size() == 4) {
    const GeoRect rect{
        boundsArray.at(0).toDouble(), boundsArray.at(1).toDouble(),
        boundsArray.at(2).toDouble(), boundsArray.at(3).toDouble()};
    if (rect.westDeg < rect.eastDeg && rect.southDeg < rect.northDeg) {
      source.bounds = rect;
    }
  }
  source.service = QUrl(json.value(QStringLiteral("service")).toString());
  source.title = json.value(QStringLiteral("title")).toString();
  source.attribution = json.value(QStringLiteral("attribution")).toString();
  return source;
}

WmtsSource XyzSource::toWmtsSource() const
{
  WmtsSource wmts;
  wmts.urlTemplate = urlTemplate;
  wmts.service =
      service.isEmpty() ? xyzServiceUrl(urlTemplate, subdomains) : service;
  wmts.layer = QStringLiteral("xyz");
  wmts.tileMatrixSet = QStringLiteral("GoogleMapsCompatible");
  wmts.crs = QStringLiteral("EPSG:3857");
  wmts.format = format;
  wmts.bounds = bounds;
  wmts.title = title;
  wmts.attribution = attribution;
  const int32_t first = std::clamp(minimumZoom, 0, deepestZoom);
  const int32_t last = std::clamp(maximumZoom, first, deepestZoom);
  const double halfWorld = pi * mercatorRadius;
  for (int32_t z = first; z <= last; ++z) {
    const int64_t tiles = int64_t{1} << z;
    WmtsMatrix matrix;
    matrix.identifier = QString::number(z);
    matrix.pixelSize =
        2.0 * halfWorld /
        (static_cast<double>(tileSizePx) * static_cast<double>(tiles));
    matrix.originX = -halfWorld;
    matrix.originY = halfWorld;
    matrix.tileWidth = tileSizePx;
    matrix.tileHeight = tileSizePx;
    matrix.matrixWidth = tiles;
    matrix.matrixHeight = tiles;
    matrix.minColumn = 0;
    matrix.maxColumn = tiles - 1;
    matrix.minRow = 0;
    matrix.maxRow = tiles - 1;
    if (bounds) {
      matrix.minColumn = columnAt(bounds->westDeg, tiles);
      matrix.maxColumn = columnAt(bounds->eastDeg, tiles);
      matrix.minRow = rowAt(bounds->northDeg, tiles);
      matrix.maxRow = rowAt(bounds->southDeg, tiles);
    }
    wmts.matrices.push_back(matrix);
  }
  // The quadtree's level n has 180° / 2ⁿ / 256 pixels; zoom z has
  // 360° / 2^z / tileSizePx at the equator.
  const double level =
      std::ceil(static_cast<double>(last) +
                std::log2(static_cast<double>(tileSizePx) / 512.0) - 0.01);
  wmts.maximumLevel =
      std::clamp(static_cast<int32_t>(level), 0, TileKey::maximumLevel);
  return wmts;
}

XyzSource XyzCatalogueEntry::source() const
{
  XyzSource xyz;
  xyz.urlTemplate = urlTemplate;
  xyz.subdomains = subdomains;
  xyz.tms = tms;
  xyz.minimumZoom = minimumZoom;
  xyz.maximumZoom = maximumZoom;
  xyz.format = xyzFormatFor(urlTemplate);
  xyz.title = title;
  xyz.attribution = attribution;
  xyz.service = xyzServiceUrl(urlTemplate, subdomains);
  return xyz;
}

WmsCatalogueEntry XyzCatalogueEntry::serviceEntry() const
{
  WmsCatalogueEntry entry;
  entry.title = title;
  entry.url = xyzServiceUrl(urlTemplate, subdomains);
  entry.description = description;
  entry.attribution = attribution;
  entry.termsUrl = termsUrl;
  entry.allowBulkDownload = allowBulkDownload;
  return entry;
}

std::optional<std::vector<XyzCatalogueEntry>>
parseXyzCatalogue(const QByteArray& json, QString* errorString)
{
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  const QJsonObject root = document.object();
  if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
      root.value(QStringLiteral("format")).toString() !=
          QLatin1String(xyzCatalogueFormatName)) {
    if (errorString != nullptr) {
      *errorString = QCoreApplication::translate(
          "ts::XyzSource", "This is not an XYZ catalogue.");
    }
    return std::nullopt;
  }
  if (root.value(QStringLiteral("version")).toInt(0) >
      xyzCatalogueFormatVersion) {
    if (errorString != nullptr) {
      *errorString = QCoreApplication::translate(
          "ts::XyzSource",
          "The XYZ catalogue is of a newer format than this program reads.");
    }
    return std::nullopt;
  }
  std::vector<XyzCatalogueEntry> entries;
  const QJsonArray services = root.value(QStringLiteral("services")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < services.size(); ++i) {
    const QJsonObject service = services.at(i).toObject();
    XyzCatalogueEntry entry;
    entry.title = service.value(QStringLiteral("title")).toString().trimmed();
    entry.urlTemplate =
        service.value(QStringLiteral("template")).toString().trimmed();
    entry.subdomains = stringList(service.value(QStringLiteral("subdomains")));
    entry.tms = service.value(QStringLiteral("tms")).toBool(false);
    entry.minimumZoom = std::clamp(
        service.value(QStringLiteral("minZoom")).toInt(0), 0, deepestZoom);
    entry.maximumZoom =
        std::clamp(service.value(QStringLiteral("maxZoom")).toInt(19),
                   entry.minimumZoom, deepestZoom);
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
    if (!entry.title.isEmpty() &&
        checkXyzTemplate(entry.urlTemplate, entry.subdomains).isEmpty()) {
      entries.push_back(entry);
    }
  }
  return entries;
}

} // namespace ts
