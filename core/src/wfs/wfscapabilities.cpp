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
#include "ts/wfs/wfscapabilities.h"

#include <algorithm>
#include <cstdint>

#include <QCoreApplication>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace ts
{

namespace
{

std::optional<WfsCapabilities> fail(QString* errorString,
                                    const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return std::nullopt;
}

QString text(QXmlStreamReader& reader)
{
  return reader.readElementText(QXmlStreamReader::IncludeChildElements)
      .simplified();
}

QString href(const QXmlStreamReader& reader)
{
  const QXmlStreamAttributes attributes = reader.attributes();
  for (const QXmlStreamAttribute& attribute : attributes) {
    if (attribute.name() == QLatin1String("href")) {
      return attribute.value().toString().trimmed();
    }
  }
  return QString();
}

/** Reads "lon lat" (OWS corners are always longitude first in WGS84). */
bool corner(const QString& value, double* lon, double* lat)
{
  const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  bool okLon = false;
  bool okLat = false;
  if (parts.size() < 2) {
    return false;
  }
  *lon = parts.at(0).toDouble(&okLon);
  *lat = parts.at(1).toDouble(&okLat);
  return okLon && okLat;
}

std::optional<GeoRect> wgs84Box(QXmlStreamReader& reader)
{
  double west = 0.0;
  double south = 0.0;
  double east = 0.0;
  double north = 0.0;
  bool lower = false;
  bool upper = false;
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("LowerCorner")) {
      lower = corner(text(reader), &west, &south);
    } else if (reader.name() == QLatin1String("UpperCorner")) {
      upper = corner(text(reader), &east, &north);
    } else {
      reader.skipCurrentElement();
    }
  }
  if (!lower || !upper || !(west <= east) || !(south <= north)) {
    return std::nullopt;
  }
  return GeoRect{std::max(west, -180.0), std::max(south, -90.0),
                 std::min(east, 180.0), std::min(north, 90.0)};
}

/** The allowed values of an OWS Parameter or Constraint. */
QStringList allowedValues(QXmlStreamReader& reader, QString* defaultValue)
{
  QStringList values;
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("AllowedValues")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("Value")) {
          values.append(text(reader));
        } else {
          reader.skipCurrentElement();
        }
      }
    } else if (reader.name() == QLatin1String("Value")) {
      values.append(text(reader)); // WFS 1.1.0
    } else if (reader.name() == QLatin1String("DefaultValue") &&
               defaultValue != nullptr) {
      *defaultValue = text(reader);
    } else {
      reader.skipCurrentElement();
    }
  }
  return values;
}

void readOperation(QXmlStreamReader& reader, WfsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("DCP")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("HTTP")) {
          while (reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("Get") &&
                capabilities.getFeatureUrl.isEmpty()) {
              capabilities.getFeatureUrl = QUrl(href(reader));
            }
            reader.skipCurrentElement();
          }
        } else {
          reader.skipCurrentElement();
        }
      }
    } else if (name == QLatin1String("Parameter")) {
      const QString parameter =
          reader.attributes().value(QLatin1String("name")).toString();
      const QStringList values = allowedValues(reader, nullptr);
      if (parameter.compare(QLatin1String("outputFormat"),
                            Qt::CaseInsensitive) == 0) {
        capabilities.outputFormats = values;
      }
    } else {
      reader.skipCurrentElement();
    }
  }
}

void readOperationsMetadata(QXmlStreamReader& reader,
                            WfsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("Operation")) {
      if (reader.attributes().value(QLatin1String("name")) ==
          QLatin1String("GetFeature")) {
        readOperation(reader, capabilities);
      } else {
        reader.skipCurrentElement();
      }
    } else if (name == QLatin1String("Constraint")) {
      const QString constraint =
          reader.attributes().value(QLatin1String("name")).toString();
      QString value;
      const QStringList values = allowedValues(reader, &value);
      if (value.isEmpty() && !values.isEmpty()) {
        value = values.first();
      }
      if (constraint == QLatin1String("ImplementsResultPaging")) {
        capabilities.resultPaging =
            value.compare(QLatin1String("TRUE"), Qt::CaseInsensitive) == 0;
      } else if (constraint == QLatin1String("CountDefault")) {
        capabilities.countDefault = value.toLongLong();
      }
    } else {
      reader.skipCurrentElement();
    }
  }
}

void readFeatureType(QXmlStreamReader& reader, WfsCapabilities& capabilities)
{
  WfsFeatureType type;
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("Name")) {
      type.name = text(reader);
    } else if (name == QLatin1String("Title")) {
      type.title = text(reader);
    } else if (name == QLatin1String("Abstract")) {
      type.abstract = text(reader);
    } else if (name == QLatin1String("DefaultCRS") ||
               name == QLatin1String("DefaultSRS")) {
      type.defaultCrs = text(reader);
    } else if (name == QLatin1String("OtherCRS") ||
               name == QLatin1String("OtherSRS")) {
      type.otherCrs.append(text(reader));
    } else if (name == QLatin1String("WGS84BoundingBox")) {
      if (!type.wgs84Bounds) {
        type.wgs84Bounds = wgs84Box(reader);
      } else {
        reader.skipCurrentElement();
      }
    } else if (name == QLatin1String("OutputFormats")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("Format")) {
          type.outputFormats.append(text(reader));
        } else {
          reader.skipCurrentElement();
        }
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  if (!type.name.isEmpty()) {
    capabilities.featureTypes.push_back(type);
  }
}

void readServiceIdentification(QXmlStreamReader& reader,
                               WfsCapabilities& capabilities)
{
  QStringList constraints;
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("Title")) {
      capabilities.title = text(reader);
    } else if (name == QLatin1String("Abstract")) {
      capabilities.abstract = text(reader);
    } else if (name == QLatin1String("Fees")) {
      capabilities.fees = text(reader);
    } else if (name == QLatin1String("AccessConstraints")) {
      constraints.append(text(reader));
    } else {
      reader.skipCurrentElement();
    }
  }
  constraints.removeAll(QString());
  capabilities.accessConstraints = constraints.join(QStringLiteral("; "));
}

} // namespace

const WfsFeatureType*
WfsCapabilities::findFeatureType(const QString& name) const
{
  for (const WfsFeatureType& type : featureTypes) {
    if (type.name == name) {
      return &type;
    }
  }
  return nullptr;
}

std::optional<WfsCapabilities> parseWfsCapabilities(const QByteArray& xml,
                                                    QString* errorString)
{
  const QByteArray start = xml.left(512).trimmed().toLower();
  if (start.startsWith("<!doctype html") || start.startsWith("<html")) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WfsCapabilities",
                    "The address gave a web page, not a WFS. Check the "
                    "service's address."));
  }
  QXmlStreamReader reader(xml);
  if (!reader.readNextStartElement()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WfsCapabilities",
                                            "The server's answer isn't XML."));
  }
  const QString root = reader.name().toString();
  if (root.contains(QLatin1String("ExceptionReport"))) {
    return fail(errorString,
                QCoreApplication::translate("ts::WfsCapabilities",
                                            "The server reported an error: %1")
                    .arg(text(reader)));
  }
  if (root == QLatin1String("WMS_Capabilities") ||
      root == QLatin1String("WMT_MS_Capabilities") ||
      root == QLatin1String("Capabilities")) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WfsCapabilities",
                    "This is a map service (WMS or WMTS), not a WFS: add it "
                    "with Add WMS Layer or Add WMTS Layer."));
  }
  if (root != QLatin1String("WFS_Capabilities")) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WfsCapabilities",
                    "The server's answer isn't a WFS capabilities document."));
  }
  WfsCapabilities capabilities;
  capabilities.version =
      reader.attributes().value(QLatin1String("version")).toString();
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("ServiceIdentification")) {
      readServiceIdentification(reader, capabilities);
    } else if (name == QLatin1String("ServiceProvider")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("ProviderName")) {
          capabilities.providerName = text(reader);
        } else {
          reader.skipCurrentElement();
        }
      }
    } else if (name == QLatin1String("OperationsMetadata")) {
      readOperationsMetadata(reader, capabilities);
    } else if (name == QLatin1String("FeatureTypeList")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("FeatureType")) {
          readFeatureType(reader, capabilities);
        } else {
          reader.skipCurrentElement();
        }
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  if (reader.hasError()) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WfsCapabilities",
                                 "The capabilities document is damaged: %1")
                                 .arg(reader.errorString()));
  }
  return capabilities;
}

QUrl wfsCapabilitiesUrl(const QUrl& serviceUrl)
{
  QUrlQuery query(serviceUrl);
  for (const char* name : {"SERVICE", "REQUEST", "VERSION", "ACCEPTVERSIONS"}) {
    const QList<QPair<QString, QString>> items = query.queryItems();
    for (const QPair<QString, QString>& item : items) {
      if (item.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) {
        query.removeAllQueryItems(item.first);
      }
    }
  }
  query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WFS"));
  query.addQueryItem(QStringLiteral("REQUEST"),
                     QStringLiteral("GetCapabilities"));
  query.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("2.0.0"));
  QUrl url = serviceUrl;
  url.setQuery(query);
  return url;
}

} // namespace ts
