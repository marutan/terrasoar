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
#include "ts/wmts/wmtscapabilities.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <QCoreApplication>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace ts
{

namespace
{

QString attribute(const QXmlStreamReader& reader, QLatin1String name)
{
  // Attributes are matched by local name (xlink:href is "href").
  const QXmlStreamAttributes attributes = reader.attributes();
  for (const QXmlStreamAttribute& candidate : attributes) {
    if (candidate.name() == name) {
      return candidate.value().toString();
    }
  }
  return QString();
}

QString text(QXmlStreamReader& reader)
{
  return reader.readElementText(QXmlStreamReader::IncludeChildElements)
      .trimmed();
}

bool is(const QXmlStreamReader& reader, const char* name)
{
  return reader.name() == QLatin1String(name);
}

/** Two numbers separated by white space ("1 2"), or nothing. */
std::optional<std::pair<double, double>> numberPair(const QString& value)
{
  static const QRegularExpression spaces(QStringLiteral("[\\s,]+"));
  const QStringList parts = value.split(spaces, Qt::SkipEmptyParts);
  if (parts.size() != 2) {
    return std::nullopt;
  }
  bool okFirst = false;
  bool okSecond = false;
  const double first = parts.at(0).toDouble(&okFirst);
  const double second = parts.at(1).toDouble(&okSecond);
  if (!okFirst || !okSecond) {
    return std::nullopt;
  }
  return std::make_pair(first, second);
}

int64_t integer(QXmlStreamReader& reader, bool* ok)
{
  return text(reader).toLongLong(ok);
}

/** An ows:WGS84BoundingBox: corners in longitude, latitude order. */
std::optional<GeoRect> wgs84BoundingBox(QXmlStreamReader& reader)
{
  std::optional<std::pair<double, double>> lower;
  std::optional<std::pair<double, double>> upper;
  while (reader.readNextStartElement()) {
    if (is(reader, "LowerCorner")) {
      lower = numberPair(text(reader));
    } else if (is(reader, "UpperCorner")) {
      upper = numberPair(text(reader));
    } else {
      reader.skipCurrentElement();
    }
  }
  // Written so that NaN in any of them fails too.
  if (!lower || !upper || !(lower->first <= upper->first) ||
      !(lower->second <= upper->second)) {
    return std::nullopt;
  }
  return GeoRect{std::clamp(lower->first, -180.0, 180.0),
                 std::clamp(lower->second, -90.0, 90.0),
                 std::clamp(upper->first, -180.0, 180.0),
                 std::clamp(upper->second, -90.0, 90.0)};
}

WmtsStyle parseStyle(QXmlStreamReader& reader)
{
  WmtsStyle style;
  const QString isDefault = attribute(reader, QLatin1String("isDefault"));
  style.isDefault = isDefault.trimmed() == QLatin1String("1") ||
                    isDefault.trimmed().compare(QLatin1String("true"),
                                                Qt::CaseInsensitive) == 0;
  while (reader.readNextStartElement()) {
    if (is(reader, "Identifier")) {
      style.identifier = text(reader);
    } else if (is(reader, "Title") && style.title.isEmpty()) {
      style.title = text(reader);
    } else if (is(reader, "LegendURL") && style.legendUrl.isEmpty()) {
      style.legendUrl =
          QUrl(attribute(reader, QLatin1String("href")).trimmed());
      reader.skipCurrentElement();
    } else {
      reader.skipCurrentElement();
    }
  }
  return style;
}

WmtsDimension parseDimension(QXmlStreamReader& reader)
{
  WmtsDimension dimension;
  while (reader.readNextStartElement()) {
    if (is(reader, "Identifier")) {
      dimension.identifier = text(reader);
    } else if (is(reader, "Title") && dimension.title.isEmpty()) {
      dimension.title = text(reader);
    } else if (is(reader, "UOM")) {
      dimension.unitOfMeasure = text(reader);
    } else if (is(reader, "Default")) {
      dimension.defaultValue = text(reader);
    } else if (is(reader, "Value")) {
      dimension.values.append(text(reader));
    } else {
      reader.skipCurrentElement();
    }
  }
  return dimension;
}

WmtsTileMatrixLimits parseLimits(QXmlStreamReader& reader, bool* ok)
{
  WmtsTileMatrixLimits limits;
  bool all = true;
  int32_t found = 0;
  while (reader.readNextStartElement()) {
    bool good = true;
    if (is(reader, "TileMatrix")) {
      limits.tileMatrix = text(reader);
    } else if (is(reader, "MinTileRow")) {
      limits.minRow = integer(reader, &good);
      ++found;
    } else if (is(reader, "MaxTileRow")) {
      limits.maxRow = integer(reader, &good);
      ++found;
    } else if (is(reader, "MinTileCol")) {
      limits.minColumn = integer(reader, &good);
      ++found;
    } else if (is(reader, "MaxTileCol")) {
      limits.maxColumn = integer(reader, &good);
      ++found;
    } else {
      reader.skipCurrentElement();
    }
    all = all && good;
  }
  *ok = all && found == 4 && !limits.tileMatrix.isEmpty() &&
        limits.minRow <= limits.maxRow && limits.minColumn <= limits.maxColumn;
  return limits;
}

WmtsTileMatrixSetLink parseLink(QXmlStreamReader& reader)
{
  WmtsTileMatrixSetLink link;
  while (reader.readNextStartElement()) {
    if (is(reader, "TileMatrixSet")) {
      link.tileMatrixSet = text(reader);
    } else if (is(reader, "TileMatrixSetLimits")) {
      while (reader.readNextStartElement()) {
        if (is(reader, "TileMatrixLimits")) {
          bool ok = false;
          const WmtsTileMatrixLimits limits = parseLimits(reader, &ok);
          if (ok) {
            link.limits.push_back(limits);
          }
        } else {
          reader.skipCurrentElement();
        }
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  return link;
}

/** Reads a ResourceURL; keeps it if it is a tile template. */
void parseResourceUrl(QXmlStreamReader& reader, WmtsLayer& layer)
{
  WmtsResourceUrl resource;
  resource.format = attribute(reader, QLatin1String("format")).trimmed();
  resource.resourceType =
      attribute(reader, QLatin1String("resourceType")).trimmed();
  resource.urlTemplate = attribute(reader, QLatin1String("template")).trimmed();
  reader.skipCurrentElement();
  if (resource.resourceType.compare(QLatin1String("tile"),
                                    Qt::CaseInsensitive) == 0 &&
      !resource.urlTemplate.isEmpty()) {
    layer.resourceUrls.push_back(resource);
  }
}

/**
 * Reads a layer's child that adds to a list (format, dimension, link,
 * template). Returns false if it isn't one of those.
 */
bool parseLayerListItem(QXmlStreamReader& reader, WmtsLayer& layer)
{
  if (is(reader, "Format")) {
    const QString format = text(reader);
    if (!format.isEmpty() && !layer.formats.contains(format)) {
      layer.formats.append(format);
    }
  } else if (is(reader, "Dimension")) {
    WmtsDimension dimension = parseDimension(reader);
    if (!dimension.identifier.isEmpty()) {
      layer.dimensions.push_back(std::move(dimension));
    }
  } else if (is(reader, "TileMatrixSetLink")) {
    WmtsTileMatrixSetLink link = parseLink(reader);
    if (!link.tileMatrixSet.isEmpty()) {
      layer.tileMatrixSetLinks.push_back(std::move(link));
    }
  } else if (is(reader, "ResourceURL")) {
    parseResourceUrl(reader, layer);
  } else if (is(reader, "Style")) {
    layer.styles.push_back(parseStyle(reader));
  } else {
    return false;
  }
  return true;
}

WmtsLayer parseLayer(QXmlStreamReader& reader)
{
  WmtsLayer layer;
  while (reader.readNextStartElement()) {
    if (parseLayerListItem(reader, layer)) {
      continue;
    }
    if (is(reader, "Identifier")) {
      layer.identifier = text(reader);
    } else if (is(reader, "Title") && layer.title.isEmpty()) {
      layer.title = text(reader);
    } else if (is(reader, "Abstract") && layer.abstract.isEmpty()) {
      layer.abstract = text(reader);
    } else if (is(reader, "WGS84BoundingBox")) {
      const std::optional<GeoRect> bounds = wgs84BoundingBox(reader);
      if (bounds && !layer.wgs84Bounds) {
        layer.wgs84Bounds = bounds;
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  return layer;
}

bool parseMatrix(QXmlStreamReader& reader, WmtsTileMatrix& matrix)
{
  bool good = true;
  bool hasScale = false;
  bool hasCorner = false;
  while (reader.readNextStartElement()) {
    bool ok = true;
    if (is(reader, "Identifier")) {
      matrix.identifier = text(reader);
    } else if (is(reader, "ScaleDenominator")) {
      matrix.scaleDenominator = text(reader).toDouble(&ok);
      hasScale = ok;
    } else if (is(reader, "TopLeftCorner") || is(reader, "TopLeftPoint")) {
      const std::optional<std::pair<double, double>> corner =
          numberPair(text(reader));
      ok = corner.has_value();
      if (corner) {
        matrix.topLeftFirst = corner->first;
        matrix.topLeftSecond = corner->second;
        hasCorner = true;
      }
    } else if (is(reader, "TileWidth")) {
      matrix.tileWidth = static_cast<int32_t>(integer(reader, &ok));
    } else if (is(reader, "TileHeight")) {
      matrix.tileHeight = static_cast<int32_t>(integer(reader, &ok));
    } else if (is(reader, "MatrixWidth")) {
      matrix.matrixWidth = integer(reader, &ok);
    } else if (is(reader, "MatrixHeight")) {
      matrix.matrixHeight = integer(reader, &ok);
    } else {
      reader.skipCurrentElement();
    }
    good = good && ok;
  }
  return good && hasScale && hasCorner && !matrix.identifier.isEmpty() &&
         matrix.scaleDenominator > 0.0 && matrix.tileWidth > 0 &&
         matrix.tileHeight > 0 && matrix.tileWidth <= 4096 &&
         matrix.tileHeight <= 4096 && matrix.matrixWidth > 0 &&
         matrix.matrixHeight > 0;
}

WmtsTileMatrixSet parseMatrixSet(QXmlStreamReader& reader)
{
  WmtsTileMatrixSet set;
  while (reader.readNextStartElement()) {
    if (is(reader, "Identifier")) {
      set.identifier = text(reader);
    } else if (is(reader, "SupportedCRS")) {
      set.supportedCrs = text(reader);
    } else if (is(reader, "WellKnownScaleSet")) {
      set.wellKnownScaleSet = text(reader);
    } else if (is(reader, "TileMatrix")) {
      WmtsTileMatrix matrix;
      if (parseMatrix(reader, matrix)) {
        set.matrices.push_back(matrix);
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  return set;
}

void parseContents(QXmlStreamReader& reader, WmtsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    if (is(reader, "Layer")) {
      WmtsLayer layer = parseLayer(reader);
      if (!layer.identifier.isEmpty()) {
        capabilities.layers.push_back(std::move(layer));
      }
    } else if (is(reader, "TileMatrixSet")) {
      WmtsTileMatrixSet set = parseMatrixSet(reader);
      if (!set.identifier.isEmpty() && !set.matrices.empty()) {
        capabilities.tileMatrixSets.push_back(std::move(set));
      }
    } else {
      reader.skipCurrentElement();
    }
  }
}

void parseServiceIdentification(QXmlStreamReader& reader,
                                WmtsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    if (is(reader, "Title") && capabilities.title.isEmpty()) {
      capabilities.title = text(reader);
    } else if (is(reader, "Abstract") && capabilities.abstract.isEmpty()) {
      capabilities.abstract = text(reader);
    } else if (is(reader, "Fees")) {
      capabilities.fees = text(reader);
    } else if (is(reader, "AccessConstraints")) {
      const QString constraint = text(reader);
      capabilities.accessConstraints =
          capabilities.accessConstraints.isEmpty()
              ? constraint
              : capabilities.accessConstraints + QLatin1Char(' ') + constraint;
    } else {
      reader.skipCurrentElement();
    }
  }
}

void parseServiceProvider(QXmlStreamReader& reader,
                          WmtsCapabilities& capabilities)
{
  // Only who runs the service: ServiceContact names people and is left.
  while (reader.readNextStartElement()) {
    if (is(reader, "ProviderName")) {
      capabilities.providerName = text(reader);
    } else if (is(reader, "ProviderSite")) {
      capabilities.providerSite =
          QUrl(attribute(reader, QLatin1String("href")).trimmed());
      reader.skipCurrentElement();
    } else {
      reader.skipCurrentElement();
    }
  }
}

/** Appends the text of each Value element under the current element. */
void readValues(QXmlStreamReader& reader, QStringList& values)
{
  while (reader.readNextStartElement()) {
    if (is(reader, "Value")) {
      values.append(text(reader));
    } else if (is(reader, "AllowedValues")) {
      readValues(reader, values);
    } else {
      reader.skipCurrentElement();
    }
  }
}

/** The encodings an ows:Get allows ("KVP", "RESTful", …); none if open. */
QStringList getEncodings(QXmlStreamReader& reader)
{
  QStringList encodings;
  while (reader.readNextStartElement()) {
    if (is(reader, "Constraint") &&
        attribute(reader, QLatin1String("name"))
                .compare(QLatin1String("GetEncoding"), Qt::CaseInsensitive) ==
            0) {
      readValues(reader, encodings);
    } else {
      reader.skipCurrentElement();
    }
  }
  return encodings;
}

void parseGetTile(QXmlStreamReader& reader, WmtsCapabilities& capabilities)
{
  // DCP / HTTP / Get, possibly several (one per encoding).
  while (reader.readNextStartElement()) {
    if (is(reader, "DCP") || is(reader, "HTTP")) {
      parseGetTile(reader, capabilities);
    } else if (is(reader, "Get")) {
      const QUrl url(attribute(reader, QLatin1String("href")).trimmed());
      const QStringList encodings = getEncodings(reader);
      const bool kvp =
          encodings.isEmpty() ||
          std::any_of(encodings.begin(), encodings.end(),
                      [](const QString& encoding) {
                        return encoding.compare(QLatin1String("KVP"),
                                                Qt::CaseInsensitive) == 0;
                      });
      if (kvp && capabilities.getTileKvpUrl.isEmpty() && url.isValid() &&
          !url.isEmpty()) {
        capabilities.getTileKvpUrl = url;
      }
    } else {
      reader.skipCurrentElement();
    }
  }
}

void parseOperations(QXmlStreamReader& reader, WmtsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    if (is(reader, "Operation") &&
        attribute(reader, QLatin1String("name")) == QLatin1String("GetTile")) {
      parseGetTile(reader, capabilities);
    } else {
      reader.skipCurrentElement();
    }
  }
}

QString exceptionText(QXmlStreamReader& reader)
{
  QStringList messages;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement() &&
        (is(reader, "ExceptionText") || is(reader, "ServiceException"))) {
      messages.append(text(reader));
    }
  }
  return messages.join(QLatin1Char(' ')).simplified();
}

std::optional<WmtsCapabilities> fail(QString* errorString,
                                     const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return std::nullopt;
}

} // namespace

const WmtsTileMatrix*
WmtsTileMatrixSet::findMatrix(const QString& matrixIdentifier) const
{
  for (const WmtsTileMatrix& matrix : matrices) {
    if (matrix.identifier == matrixIdentifier) {
      return &matrix;
    }
  }
  return nullptr;
}

QString WmtsLayer::defaultStyle() const
{
  for (const WmtsStyle& style : styles) {
    if (style.isDefault) {
      return style.identifier;
    }
  }
  return styles.empty() ? QString() : styles.front().identifier;
}

const WmtsTileMatrixSetLink*
WmtsLayer::findLink(const QString& tileMatrixSet) const
{
  for (const WmtsTileMatrixSetLink& link : tileMatrixSetLinks) {
    if (link.tileMatrixSet == tileMatrixSet) {
      return &link;
    }
  }
  return nullptr;
}

const WmtsLayer*
WmtsCapabilities::findLayer(const QString& layerIdentifier) const
{
  for (const WmtsLayer& layer : layers) {
    if (layer.identifier == layerIdentifier) {
      return &layer;
    }
  }
  return nullptr;
}

const WmtsTileMatrixSet*
WmtsCapabilities::findTileMatrixSet(const QString& setIdentifier) const
{
  for (const WmtsTileMatrixSet& set : tileMatrixSets) {
    if (set.identifier == setIdentifier) {
      return &set;
    }
  }
  return nullptr;
}

std::optional<WmtsCapabilities>
parseWmtsCapabilities(const QByteArray& document, QString* errorString)
{
  if (document.trimmed().isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmtsCapabilities",
                                            "The capabilities document is "
                                            "empty."));
  }
  QXmlStreamReader reader(document);
  if (!reader.readNextStartElement()) {
    return fail(errorString,
                reader.hasError()
                    ? QCoreApplication::translate(
                          "ts::WmtsCapabilities",
                          "The capabilities are not valid XML: %1 (line %2).")
                          .arg(reader.errorString())
                          .arg(reader.lineNumber())
                    : QCoreApplication::translate(
                          "ts::WmtsCapabilities",
                          "The capabilities document is empty."));
  }
  const QString root = reader.name().toString();
  if (root == QLatin1String("ExceptionReport") ||
      root == QLatin1String("ServiceExceptionReport")) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmtsCapabilities",
                                            "The server reported an error: %1")
                    .arg(exceptionText(reader)));
  }
  if (root.compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmtsCapabilities",
                    "The server sent a web page, not WMTS capabilities. Check "
                    "the address."));
  }
  if (root == QLatin1String("WMS_Capabilities") ||
      root == QLatin1String("WMT_MS_Capabilities")) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmtsCapabilities",
                    "This is a WMS service, not WMTS. Add it with Add WMS "
                    "Layer instead."));
  }
  if (root != QLatin1String("Capabilities")) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmtsCapabilities",
                    "This is not a WMTS capabilities document (it starts with "
                    "<%1>).")
                    .arg(root));
  }
  WmtsCapabilities capabilities;
  while (reader.readNextStartElement()) {
    if (is(reader, "ServiceIdentification")) {
      parseServiceIdentification(reader, capabilities);
    } else if (is(reader, "ServiceProvider")) {
      parseServiceProvider(reader, capabilities);
    } else if (is(reader, "OperationsMetadata")) {
      parseOperations(reader, capabilities);
    } else if (is(reader, "Contents")) {
      parseContents(reader, capabilities);
    } else {
      reader.skipCurrentElement();
    }
  }
  if (reader.hasError()) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmtsCapabilities",
                    "The capabilities are not valid XML: %1 (line %2).")
                    .arg(reader.errorString())
                    .arg(reader.lineNumber()));
  }
  if (capabilities.layers.empty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmtsCapabilities",
                                            "The service has no layers."));
  }
  if (capabilities.tileMatrixSets.empty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmtsCapabilities",
                                            "The service describes no tile "
                                            "matrix sets."));
  }
  return capabilities;
}

} // namespace ts
