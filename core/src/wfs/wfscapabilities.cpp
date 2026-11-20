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

/** A corner of a WGS 84 box. */
struct Corner {
  double lonDeg = 0.0;
  double latDeg = 0.0;
};

/** Reads "lon lat" (OWS corners are always longitude first in WGS84). */
std::optional<Corner> corner(const QString& value)
{
  const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  bool okLon = false;
  bool okLat = false;
  if (parts.size() < 2) {
    return std::nullopt;
  }
  const Corner read{parts.at(0).toDouble(&okLon), parts.at(1).toDouble(&okLat)};
  if (!okLon || !okLat) {
    return std::nullopt;
  }
  return read;
}

std::optional<GeoRect> wgs84Box(QXmlStreamReader& reader)
{
  std::optional<Corner> lower;
  std::optional<Corner> upper;
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("LowerCorner")) {
      lower = corner(text(reader));
    } else if (reader.name() == QLatin1String("UpperCorner")) {
      upper = corner(text(reader));
    } else {
      reader.skipCurrentElement();
    }
  }
  if (!lower || !upper || !(lower->lonDeg <= upper->lonDeg) ||
      !(lower->latDeg <= upper->latDeg)) {
    return std::nullopt;
  }
  return GeoRect{std::max(lower->lonDeg, -180.0),
                 std::max(lower->latDeg, -90.0), std::min(upper->lonDeg, 180.0),
                 std::min(upper->latDeg, 90.0)};
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

/** The GET address in a DCP element (empty if none). */
QUrl dcpGet(QXmlStreamReader& reader)
{
  QUrl get;
  while (reader.readNextStartElement()) {
    if (reader.name() != QLatin1String("HTTP")) {
      reader.skipCurrentElement();
      continue;
    }
    while (reader.readNextStartElement()) {
      if (reader.name() == QLatin1String("Get") && get.isEmpty()) {
        get = QUrl(href(reader));
      }
      reader.skipCurrentElement();
    }
  }
  return get;
}

void readOperation(QXmlStreamReader& reader, WfsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("DCP")) {
      const QUrl get = dcpGet(reader);
      if (capabilities.getFeatureUrl.isEmpty()) {
        capabilities.getFeatureUrl = get;
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

namespace
{

/** Why the document's root @p root isn't a WFS's, or empty if it is. */
QString rootProblem(const QString& root, QXmlStreamReader& reader)
{
  if (root.contains(QLatin1String("ExceptionReport"))) {
    return QCoreApplication::translate("ts::WfsCapabilities",
                                       "The server reported an error: %1")
        .arg(text(reader));
  }
  if (root == QLatin1String("WMS_Capabilities") ||
      root == QLatin1String("WMT_MS_Capabilities") ||
      root == QLatin1String("Capabilities")) {
    return QCoreApplication::translate(
        "ts::WfsCapabilities",
        "This is a map service (WMS or WMTS), not a WFS: add it "
        "with Add WMS Layer or Add WMTS Layer.");
  }
  if (root != QLatin1String("WFS_Capabilities")) {
    return QCoreApplication::translate(
        "ts::WfsCapabilities",
        "The server's answer isn't a WFS capabilities document.");
  }
  return QString();
}

void readServiceProvider(QXmlStreamReader& reader,
                         WfsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("ProviderName")) {
      capabilities.providerName = text(reader);
    } else {
      reader.skipCurrentElement();
    }
  }
}

void readFeatureTypeList(QXmlStreamReader& reader,
                         WfsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("FeatureType")) {
      readFeatureType(reader, capabilities);
    } else {
      reader.skipCurrentElement();
    }
  }
}

} // namespace

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
  const QString problem = rootProblem(reader.name().toString(), reader);
  if (!problem.isEmpty()) {
    return fail(errorString, problem);
  }
  WfsCapabilities capabilities;
  capabilities.version =
      reader.attributes().value(QLatin1String("version")).toString();
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("ServiceIdentification")) {
      readServiceIdentification(reader, capabilities);
    } else if (name == QLatin1String("ServiceProvider")) {
      readServiceProvider(reader, capabilities);
    } else if (name == QLatin1String("OperationsMetadata")) {
      readOperationsMetadata(reader, capabilities);
    } else if (name == QLatin1String("FeatureTypeList")) {
      readFeatureTypeList(reader, capabilities);
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
