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
#include "ts/wfs/wfssource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocale>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include "ts/wfs/wfscapabilities.h"

namespace ts
{

namespace
{

/** The CRS asked for, latitude first. */
constexpr const char* epsg4326Urn = "urn:ogc:def:crs:EPSG::4326";

/** Deepest level a source may ask for. */
constexpr int32_t deepestLevel = 16;

QString shortest(double value)
{
  return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

template <typename T>
std::optional<T> failWith(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return std::nullopt;
}

// --- GeoJSON -----------------------------------------------------------

std::optional<GeoPoint> jsonPoint(const QJsonValue& value)
{
  const QJsonArray pair = value.toArray();
  if (pair.size() < 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble()) {
    return std::nullopt;
  }
  return GeoPoint{pair.at(1).toDouble(), pair.at(0).toDouble()};
}

std::vector<GeoPoint> jsonLine(const QJsonValue& value)
{
  std::vector<GeoPoint> line;
  const QJsonArray points = value.toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < points.size(); ++i) {
    const std::optional<GeoPoint> point = jsonPoint(points.at(i));
    if (point) {
      line.push_back(*point);
    }
  }
  return line;
}

std::vector<std::vector<GeoPoint>> jsonLines(const QJsonValue& value)
{
  std::vector<std::vector<GeoPoint>> lines;
  const QJsonArray array = value.toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < array.size(); ++i) {
    std::vector<GeoPoint> line = jsonLine(array.at(i));
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

/** Adds @p geometry's features (with @p base's id and attributes). */
void addJsonGeometry(const QJsonObject& geometry, const Feature& base,
                     std::vector<Feature>& features)
{
  const QString type = geometry.value(QStringLiteral("type")).toString();
  const QJsonValue coordinates = geometry.value(QStringLiteral("coordinates"));
  Feature feature = base;
  if (type == QLatin1String("Point")) {
    const std::optional<GeoPoint> point = jsonPoint(coordinates);
    if (point) {
      feature.type = Feature::Type::Points;
      feature.parts = {{*point}};
      features.push_back(std::move(feature));
    }
  } else if (type == QLatin1String("MultiPoint")) {
    feature.type = Feature::Type::Points;
    for (const GeoPoint& point : jsonLine(coordinates)) {
      feature.parts.push_back({point});
    }
    if (!feature.parts.empty()) {
      features.push_back(std::move(feature));
    }
  } else if (type == QLatin1String("LineString")) {
    feature.type = Feature::Type::LineStrings;
    std::vector<GeoPoint> line = jsonLine(coordinates);
    if (line.size() >= 2) {
      feature.parts.push_back(std::move(line));
      features.push_back(std::move(feature));
    }
  } else if (type == QLatin1String("MultiLineString")) {
    feature.type = Feature::Type::LineStrings;
    feature.parts = jsonLines(coordinates);
    if (!feature.parts.empty()) {
      features.push_back(std::move(feature));
    }
  } else if (type == QLatin1String("Polygon")) {
    feature.type = Feature::Type::Polygon;
    feature.parts = jsonLines(coordinates);
    if (!feature.parts.empty()) {
      features.push_back(std::move(feature));
    }
  } else if (type == QLatin1String("MultiPolygon")) {
    const QJsonArray polygons = coordinates.toArray();
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int32_t i = 0; i < polygons.size(); ++i) {
      Feature part = base;
      part.type = Feature::Type::Polygon;
      part.parts = jsonLines(polygons.at(i));
      if (!part.parts.empty()) {
        features.push_back(std::move(part));
      }
    }
  } else if (type == QLatin1String("GeometryCollection")) {
    const QJsonArray geometries =
        geometry.value(QStringLiteral("geometries")).toArray();
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int32_t i = 0; i < geometries.size(); ++i) {
      addJsonGeometry(geometries.at(i).toObject(), base, features);
    }
  }
}

std::optional<WfsPage> parseJson(const QByteArray& body, QString* errorString)
{
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(body, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return failWith<WfsPage>(errorString, QCoreApplication::translate(
                                              "ts::WfsSource",
                                              "The server's answer isn't valid "
                                              "GeoJSON: %1")
                                              .arg(error.errorString()));
  }
  const QJsonObject root = document.object();
  WfsPage page;
  const QJsonArray features = root.value(QStringLiteral("features")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < features.size(); ++i) {
    const QJsonObject object = features.at(i).toObject();
    Feature base;
    const QJsonValue id = object.value(QStringLiteral("id"));
    base.id =
        id.isDouble() ? QString::number(id.toDouble(), 'g', 17) : id.toString();
    base.attributes =
        object.value(QStringLiteral("properties")).toObject().toVariantMap();
    addJsonGeometry(object.value(QStringLiteral("geometry")).toObject(), base,
                    page.features);
  }
  const QJsonValue returned = root.value(QStringLiteral("numberReturned"));
  page.returned = returned.isDouble()
                      ? static_cast<int64_t>(returned.toDouble())
                      : features.size();
  const QJsonValue matched = root.value(QStringLiteral("numberMatched"));
  if (matched.isDouble()) {
    page.matched = static_cast<int64_t>(matched.toDouble());
  } else {
    const QJsonValue total = root.value(QStringLiteral("totalFeatures"));
    if (total.isDouble()) {
      page.matched = static_cast<int64_t>(total.toDouble());
    }
  }
  return page;
}

// --- GML ---------------------------------------------------------------

struct XmlNode {
  QString name;
  std::vector<std::pair<QString, QString>> attributes;
  QString text;
  std::vector<std::unique_ptr<XmlNode>> children;

  QString attribute(const char* wanted) const
  {
    for (const std::pair<QString, QString>& pair : attributes) {
      if (pair.first == QLatin1String(wanted)) {
        return pair.second;
      }
    }
    return QString();
  }

  const XmlNode* child(const char* wanted) const
  {
    for (const std::unique_ptr<XmlNode>& node : children) {
      if (node->name == QLatin1String(wanted)) {
        return node.get();
      }
    }
    return nullptr;
  }
};

std::unique_ptr<XmlNode> readElement(QXmlStreamReader& reader)
{
  std::unique_ptr<XmlNode> node = std::make_unique<XmlNode>();
  node->name = reader.name().toString();
  const QXmlStreamAttributes attributes = reader.attributes();
  for (const QXmlStreamAttribute& attribute : attributes) {
    node->attributes.emplace_back(attribute.name().toString(),
                                  attribute.value().toString());
  }
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement()) {
      node->children.push_back(readElement(reader));
    } else if (reader.isCharacters()) {
      node->text += reader.text();
    } else if (reader.isEndElement()) {
      break;
    }
  }
  return node;
}

/** True if @p srsName writes latitude first. */
bool latitudeFirst(const QString& srsName, bool fallback)
{
  const QString lower = srsName.trimmed().toLower();
  if (lower.isEmpty()) {
    return fallback;
  }
  if (lower.contains(QLatin1String("crs84")) ||
      lower.startsWith(QLatin1String("epsg:")) ||
      lower.contains(QLatin1String("epsg.xml#"))) {
    return false;
  }
  return lower.contains(QLatin1String("4326"));
}

/** Reads a pos, posList or GML 2 coordinates element's points. */
std::vector<GeoPoint> readPoints(const XmlNode& node, bool latFirst,
                                 int32_t dimension)
{
  std::vector<GeoPoint> points;
  const std::function<void(double, double)> add = [&](double a, double b) {
    points.push_back(latFirst ? GeoPoint{a, b} : GeoPoint{b, a});
  };
  if (node.name == QLatin1String("coordinates")) {
    QString cs = node.attribute("cs");
    QString ts = node.attribute("ts");
    if (cs.isEmpty()) {
      cs = QStringLiteral(",");
    }
    if (ts.isEmpty()) {
      ts = QStringLiteral(" ");
    }
    const QStringList tuples =
        node.text.simplified().split(ts, Qt::SkipEmptyParts);
    for (const QString& tuple : tuples) {
      const QStringList values = tuple.split(cs);
      if (values.size() >= 2) {
        // GML 2 coordinates are x, y: longitude first unless the CRS's
        // axes say otherwise.
        add(values.at(0).toDouble(), values.at(1).toDouble());
      }
    }
    return points;
  }
  const int32_t own = node.attribute("srsDimension").toInt();
  const int32_t step = own >= 2 ? own : std::max(dimension, 2);
  const QStringList values =
      node.text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
  for (int32_t i = 0; i + 1 < values.size(); i += step) {
    add(values.at(i).toDouble(), values.at(i + 1).toDouble());
  }
  return points;
}

/** The points of a ring or line: its posList, pos list or coordinates. */
std::vector<GeoPoint> readLine(const XmlNode& node, bool latFirst,
                               int32_t dimension)
{
  std::vector<GeoPoint> points;
  for (const std::unique_ptr<XmlNode>& child : node.children) {
    if (child->name == QLatin1String("posList") ||
        child->name == QLatin1String("coordinates") ||
        child->name == QLatin1String("pos")) {
      const std::vector<GeoPoint> part =
          readPoints(*child, latFirst, dimension);
      points.insert(points.end(), part.begin(), part.end());
    } else if (child->name == QLatin1String("pointProperty") ||
               child->name == QLatin1String("Point")) {
      const std::vector<GeoPoint> part = readLine(*child, latFirst, dimension);
      points.insert(points.end(), part.begin(), part.end());
    } else if (child->name == QLatin1String("segments") ||
               child->name == QLatin1String("LineStringSegment")) {
      // A Curve: its segments' points, one after the other.
      const std::vector<GeoPoint> part = readLine(*child, latFirst, dimension);
      for (const GeoPoint& point : part) {
        if (points.empty() || points.back().lonDeg != point.lonDeg ||
            points.back().latDeg != point.latDeg) {
          points.push_back(point);
        }
      }
    }
  }
  return points;
}

bool isGeometry(const QString& name)
{
  static const std::array<const char*, 16> names = {
      "Point",      "MultiPoint",   "LineString",     "MultiLineString",
      "Curve",      "MultiCurve",   "CompositeCurve", "Polygon",
      "Surface",    "MultiSurface", "MultiPolygon",   "CompositeSurface",
      "LinearRing", "Ring",         "MultiGeometry",  "OrientableCurve"};
  return std::any_of(names.begin(), names.end(), [&name](const char* known) {
    return name == QLatin1String(known);
  });
}

/** Adds the geometry @p node as features based on @p base. */
void addGmlGeometry(const XmlNode& node, bool latFirst, int32_t dimension,
                    const Feature& base, std::vector<Feature>& features)
{
  const QString srs = node.attribute("srsName");
  const bool order = srs.isEmpty() ? latFirst : latitudeFirst(srs, latFirst);
  const int32_t ownDimension = node.attribute("srsDimension").toInt();
  const int32_t dim = ownDimension >= 2 ? ownDimension : dimension;
  const QString& name = node.name;
  Feature feature = base;
  if (name == QLatin1String("Point")) {
    const std::vector<GeoPoint> points = readLine(node, order, dim);
    if (!points.empty()) {
      feature.type = Feature::Type::Points;
      feature.parts = {{points.front()}};
      features.push_back(std::move(feature));
    }
    return;
  }
  if (name == QLatin1String("LineString") || name == QLatin1String("Curve") ||
      name == QLatin1String("LinearRing")) {
    std::vector<GeoPoint> line = readLine(node, order, dim);
    if (line.size() >= 2) {
      feature.type = Feature::Type::LineStrings;
      feature.parts.push_back(std::move(line));
      features.push_back(std::move(feature));
    }
    return;
  }
  if (name == QLatin1String("Polygon") || name == QLatin1String("Surface")) {
    // Rings: exterior (outerBoundaryIs) first, then interiors; a Surface's
    // PolygonPatch has them inside.
    feature.type = Feature::Type::Polygon;
    const std::function<void(const XmlNode&)> rings = [&](const XmlNode& n) {
      for (const std::unique_ptr<XmlNode>& child : n.children) {
        const bool outer = child->name == QLatin1String("exterior") ||
                           child->name == QLatin1String("outerBoundaryIs");
        const bool inner = child->name == QLatin1String("interior") ||
                           child->name == QLatin1String("innerBoundaryIs");
        if (outer || inner) {
          for (const std::unique_ptr<XmlNode>& ring : child->children) {
            std::vector<GeoPoint> points = readLine(*ring, order, dim);
            if (points.size() >= 3) {
              if (outer) {
                feature.parts.insert(feature.parts.begin(), std::move(points));
              } else {
                feature.parts.push_back(std::move(points));
              }
            }
          }
        } else if (child->name == QLatin1String("patches") ||
                   child->name == QLatin1String("PolygonPatch")) {
          rings(*child);
        }
      }
    };
    rings(node);
    if (!feature.parts.empty()) {
      features.push_back(std::move(feature));
    }
    return;
  }
  // Multi geometries and composites: each member.
  for (const std::unique_ptr<XmlNode>& child : node.children) {
    if (isGeometry(child->name)) {
      addGmlGeometry(*child, order, dim, base, features);
    } else {
      for (const std::unique_ptr<XmlNode>& member : child->children) {
        if (isGeometry(member->name)) {
          addGmlGeometry(*member, order, dim, base, features);
        }
      }
    }
  }
  // Points of a MultiPoint come one feature each; join them.
  (void)name;
}

/** The geometry element inside a property, or null. */
const XmlNode* geometryIn(const XmlNode& property)
{
  for (const std::unique_ptr<XmlNode>& child : property.children) {
    if (isGeometry(child->name)) {
      return child.get();
    }
  }
  return nullptr;
}

void addGmlFeature(const XmlNode& node, bool latFirst, WfsPage& page)
{
  Feature base;
  base.id = node.attribute("id");
  if (base.id.isEmpty()) {
    base.id = node.attribute("fid");
  }
  std::vector<const XmlNode*> geometries;
  for (const std::unique_ptr<XmlNode>& property : node.children) {
    if (property->name == QLatin1String("boundedBy")) {
      continue;
    }
    const XmlNode* geometry = geometryIn(*property);
    if (geometry != nullptr) {
      geometries.push_back(geometry);
    } else if (property->children.empty()) {
      base.attributes.insert(property->name, property->text.trimmed());
    }
  }
  for (const XmlNode* geometry : geometries) {
    addGmlGeometry(*geometry, latFirst, 2, base, page.features);
  }
}

std::optional<WfsPage> parseGml(const QByteArray& body, QString* errorString)
{
  QXmlStreamReader reader(body);
  while (!reader.atEnd() && !reader.isStartElement()) {
    reader.readNext();
  }
  if (!reader.isStartElement()) {
    return failWith<WfsPage>(
        errorString,
        QCoreApplication::translate(
            "ts::WfsSource", "The server's answer isn't GML or GeoJSON."));
  }
  const std::unique_ptr<XmlNode> root = readElement(reader);
  if (reader.hasError()) {
    return failWith<WfsPage>(
        errorString, QCoreApplication::translate(
                         "ts::WfsSource", "The server's answer is damaged: %1")
                         .arg(reader.errorString()));
  }
  if (root->name.contains(QLatin1String("ExceptionReport"))) {
    QStringList messages;
    const std::function<void(const XmlNode&)> collect = [&](const XmlNode& n) {
      if (n.name == QLatin1String("ExceptionText") ||
          n.name == QLatin1String("ServiceException")) {
        messages.append(n.text.simplified());
      }
      for (const std::unique_ptr<XmlNode>& child : n.children) {
        collect(*child);
      }
    };
    collect(*root);
    return failWith<WfsPage>(
        errorString, QCoreApplication::translate(
                         "ts::WfsSource", "The server reported an error: %1")
                         .arg(messages.join(QLatin1Char(' '))));
  }
  WfsPage page;
  // The collection's own srsName, if any, is the default for its members.
  bool latFirst = true;
  if (const XmlNode* bounded = root->child("boundedBy")) {
    for (const std::unique_ptr<XmlNode>& envelope : bounded->children) {
      latFirst = latitudeFirst(envelope->attribute("srsName"), latFirst);
    }
  }
  int64_t count = 0;
  for (const std::unique_ptr<XmlNode>& child : root->children) {
    if (child->name == QLatin1String("member") ||
        child->name == QLatin1String("featureMember") ||
        child->name == QLatin1String("featureMembers")) {
      for (const std::unique_ptr<XmlNode>& feature : child->children) {
        if (feature->name == QLatin1String("FeatureCollection") ||
            feature->name == QLatin1String("AdditionalObjects")) {
          continue;
        }
        addGmlFeature(*feature, latFirst, page);
        ++count;
      }
    }
  }
  const QString returned = root->attribute("numberReturned");
  bool ok = false;
  const int64_t number = returned.toLongLong(&ok);
  page.returned = ok ? number : count;
  const int64_t matched = root->attribute("numberMatched").toLongLong(&ok);
  if (ok) {
    page.matched = matched;
  }
  return page;
}

} // namespace

int32_t WfsSource::storedLevel(int32_t level) const
{
  if (level < minimumLevel) {
    return -1;
  }
  return std::min(level, std::max(maximumLevel, minimumLevel));
}

QString WfsSource::id() const
{
  return QStringLiteral("wfs:%1|%2|%3|%4|%5")
      .arg(url.toString(QUrl::FullyEncoded), version, typeName, outputFormat,
           QString::fromUtf8(
               QJsonDocument(styleRules).toJson(QJsonDocument::Compact)));
}

QJsonObject WfsSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), QStringLiteral("wfs"));
  json.insert(QStringLiteral("url"), url.toString(QUrl::FullyEncoded));
  if (!service.isEmpty()) {
    json.insert(QStringLiteral("service"),
                service.toString(QUrl::FullyEncoded));
  }
  json.insert(QStringLiteral("version"), version);
  json.insert(QStringLiteral("typeName"), typeName);
  json.insert(QStringLiteral("outputFormat"), outputFormat);
  json.insert(QStringLiteral("paging"), paging);
  json.insert(QStringLiteral("pageSize"), pageSize);
  json.insert(QStringLiteral("maximumPages"), maximumPages);
  if (bounds) {
    json.insert(QStringLiteral("bounds"),
                QJsonArray{bounds->westDeg, bounds->southDeg, bounds->eastDeg,
                           bounds->northDeg});
  }
  json.insert(QStringLiteral("minimumLevel"), minimumLevel);
  json.insert(QStringLiteral("maximumLevel"), maximumLevel);
  json.insert(QStringLiteral("title"), title);
  json.insert(QStringLiteral("attribution"), attribution);
  if (!styleRules.isEmpty()) {
    json.insert(QStringLiteral("styleRules"), styleRules);
  }
  return json;
}

std::optional<WfsSource> WfsSource::fromJson(const QJsonObject& json,
                                             QString* errorString)
{
  WfsSource source;
  source.url = QUrl(json.value(QStringLiteral("url")).toString());
  source.typeName = json.value(QStringLiteral("typeName")).toString();
  if (json.value(QStringLiteral("kind")).toString() != QLatin1String("wfs") ||
      !source.url.isValid() ||
      (source.url.scheme() != QLatin1String("http") &&
       source.url.scheme() != QLatin1String("https")) ||
      source.typeName.isEmpty()) {
    return failWith<WfsSource>(
        errorString,
        QCoreApplication::translate(
            "ts::WfsSource",
            "The WFS layer has no valid address or feature type."));
  }
  source.service = QUrl(json.value(QStringLiteral("service")).toString());
  source.version =
      json.value(QStringLiteral("version")).toString(source.version);
  source.outputFormat = json.value(QStringLiteral("outputFormat")).toString();
  source.paging = json.value(QStringLiteral("paging")).toBool(true);
  source.pageSize = std::clamp(
      json.value(QStringLiteral("pageSize")).toInt(source.pageSize), 10, 10000);
  source.maximumPages = std::clamp(
      json.value(QStringLiteral("maximumPages")).toInt(source.maximumPages), 1,
      50);
  const QJsonArray bounds = json.value(QStringLiteral("bounds")).toArray();
  if (bounds.size() == 4) {
    const GeoRect rect{bounds.at(0).toDouble(), bounds.at(1).toDouble(),
                       bounds.at(2).toDouble(), bounds.at(3).toDouble()};
    if (rect.westDeg <= rect.eastDeg && rect.southDeg <= rect.northDeg) {
      source.bounds = rect;
    }
  }
  source.minimumLevel = std::clamp(
      json.value(QStringLiteral("minimumLevel")).toInt(0), 0, deepestLevel);
  source.maximumLevel = std::clamp(
      json.value(QStringLiteral("maximumLevel")).toInt(source.minimumLevel + 4),
      source.minimumLevel, deepestLevel);
  source.title = json.value(QStringLiteral("title")).toString();
  source.attribution = json.value(QStringLiteral("attribution")).toString();
  source.styleRules = json.value(QStringLiteral("styleRules")).toArray();
  return source;
}

QString chooseWfsOutputFormat(const QStringList& offered)
{
  static const std::array<const char*, 11> preferred = {
      "application/json",
      "application/geo+json",
      "application/json; subtype=geojson",
      "json",
      "geojson",
      "application/gml+xml; version=3.2",
      "text/xml; subtype=gml/3.2.1",
      "text/xml; subtype=gml/3.2",
      "gml32",
      "text/xml; subtype=gml/3.1.1",
      "gml3"};
  for (const char* format : preferred) {
    for (const QString& candidate : offered) {
      if (candidate.trimmed().compare(QLatin1String(format),
                                      Qt::CaseInsensitive) == 0) {
        return candidate.trimmed();
      }
    }
  }
  for (const QString& candidate : offered) {
    const QString lower = candidate.toLower();
    if (lower.contains(QLatin1String("gml")) &&
        !lower.contains(QLatin1String("zip"))) {
      return candidate.trimmed();
    }
  }
  return QString();
}

int32_t wfsMinimumLevel(const std::optional<GeoRect>& bounds)
{
  if (!bounds) {
    return 0;
  }
  const double side = std::max(bounds->widthDeg(), bounds->heightDeg());
  if (!(side > 0.0)) {
    return 10;
  }
  const double level = std::floor(std::log2(180.0 / side));
  return std::clamp(static_cast<int32_t>(level), 0, 12);
}

QUrl getFeatureUrl(const WfsSource& source, const GeoRect& rect,
                   int64_t startIndex)
{
  QUrlQuery query(source.url);
  const bool v2 = source.version.startsWith(QLatin1String("2"));
  query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WFS"));
  query.addQueryItem(QStringLiteral("VERSION"), source.version);
  query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetFeature"));
  query.addQueryItem(v2 ? QStringLiteral("TYPENAMES")
                        : QStringLiteral("TYPENAME"),
                     source.typeName);
  const QString crs = QString::fromLatin1(epsg4326Urn);
  query.addQueryItem(QStringLiteral("SRSNAME"), crs);
  // Latitude first: the axis order of urn:ogc:def:crs:EPSG::4326.
  query.addQueryItem(
      QStringLiteral("BBOX"),
      QStringList{shortest(rect.southDeg), shortest(rect.westDeg),
                  shortest(rect.northDeg), shortest(rect.eastDeg), crs}
          .join(QLatin1Char(',')));
  if (!source.outputFormat.isEmpty()) {
    query.addQueryItem(QStringLiteral("OUTPUTFORMAT"), source.outputFormat);
  }
  query.addQueryItem(v2 ? QStringLiteral("COUNT")
                        : QStringLiteral("MAXFEATURES"),
                     QString::number(source.pageSize));
  if (source.paging && v2) {
    query.addQueryItem(QStringLiteral("STARTINDEX"),
                       QString::number(startIndex));
  }
  QUrl url = source.url;
  url.setQuery(query);
  return url;
}

std::optional<WfsSource> wfsSourceFor(const WfsCapabilities& capabilities,
                                      const QString& typeName,
                                      QString* errorString)
{
  const WfsFeatureType* type = capabilities.findFeatureType(typeName);
  if (type == nullptr) {
    return failWith<WfsSource>(
        errorString, QCoreApplication::translate(
                         "ts::WfsSource", "The server has no feature type %1.")
                         .arg(typeName));
  }
  if (capabilities.getFeatureUrl.isEmpty()) {
    return failWith<WfsSource>(
        errorString,
        QCoreApplication::translate(
            "ts::WfsSource", "The server gives no address for GetFeature."));
  }
  WfsSource source;
  source.url = capabilities.getFeatureUrl;
  source.version = capabilities.version.startsWith(QLatin1String("1."))
                       ? QStringLiteral("1.1.0")
                       : QStringLiteral("2.0.0");
  source.typeName = type->name;
  source.outputFormat = chooseWfsOutputFormat(type->outputFormats.isEmpty()
                                                  ? capabilities.outputFormats
                                                  : type->outputFormats);
  if (source.outputFormat.isEmpty() &&
      (capabilities.outputFormats.isEmpty() && type->outputFormats.isEmpty())) {
    // Nothing listed: the server's default, GML 3.2 for WFS 2.0.
    source.outputFormat = QString();
  } else if (source.outputFormat.isEmpty()) {
    return failWith<WfsSource>(
        errorString,
        QCoreApplication::translate(
            "ts::WfsSource", "The server offers no format Terra Soar reads "
                             "(GeoJSON or GML)."));
  }
  source.paging = capabilities.resultPaging;
  if (capabilities.countDefault > 0) {
    source.pageSize = static_cast<int32_t>(
        std::min<int64_t>(capabilities.countDefault, source.pageSize));
  }
  source.bounds = type->wgs84Bounds;
  source.minimumLevel = wfsMinimumLevel(source.bounds);
  source.maximumLevel = std::min(source.minimumLevel + 4, deepestLevel);
  source.title = type->title.isEmpty() ? type->name : type->title;
  source.attribution = capabilities.providerName.isEmpty()
                           ? capabilities.title
                           : capabilities.providerName;
  return source;
}

std::optional<WfsPage> parseWfsFeatures(const QByteArray& body,
                                        const QString& contentType,
                                        QString* errorString)
{
  const QString type = contentType.toLower();
  const QByteArray start = body.trimmed().left(16);
  if (type.contains(QLatin1String("json")) || start.startsWith('{')) {
    return parseJson(body, errorString);
  }
  return parseGml(body, errorString);
}

} // namespace ts
