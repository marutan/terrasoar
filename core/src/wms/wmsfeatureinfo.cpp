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
#include "ts/wms/wmsfeatureinfo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

namespace ts
{

namespace
{

std::optional<WmsFeatureInfo> fail(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return std::nullopt;
}

/** The layer part of an id such as GeoServer's "topp:states.12". */
QString layerOfId(const QString& id)
{
  static const QRegularExpression pattern(QStringLiteral("^(.+)\\.\\d+$"));
  const QRegularExpressionMatch match = pattern.match(id);
  return match.hasMatch() ? match.captured(1) : QString();
}

// --- JSON --------------------------------------------------------------

QString jsonText(const QJsonValue& value)
{
  switch (value.type()) {
  case QJsonValue::String:
    return value.toString();
  case QJsonValue::Double: {
    const double number = value.toDouble();
    if (std::abs(number) < 1e15 && number == std::floor(number)) {
      return QString::number(static_cast<int64_t>(number));
    }
    return QString::number(number, 'g', 15);
  }
  case QJsonValue::Bool:
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  case QJsonValue::Array:
    return QString::fromUtf8(
        QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  case QJsonValue::Object:
    return QString::fromUtf8(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  case QJsonValue::Null:
  case QJsonValue::Undefined:
    break;
  }
  return QString();
}

FeatureInfoFeature jsonFeature(const QJsonObject& object)
{
  FeatureInfoFeature feature;
  feature.id = jsonText(object.value(QStringLiteral("id")));
  feature.layer = layerOfId(feature.id);
  const QJsonObject properties =
      object.value(QStringLiteral("properties")).toObject();
  for (QJsonObject::const_iterator it = properties.begin();
       it != properties.end(); ++it) {
    feature.attributes.emplace_back(it.key(), jsonText(it.value()));
  }
  return feature;
}

std::optional<WmsFeatureInfo> parseJson(const QByteArray& body,
                                        QString* errorString)
{
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(body, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WmsFeatureInfo",
                                 "The server's answer isn't valid JSON: %1")
                                 .arg(error.errorString()));
  }
  const QJsonObject root = document.object();
  WmsFeatureInfo info;
  if (root.value(QStringLiteral("type")).toString() ==
      QLatin1String("Feature")) {
    info.features.push_back(jsonFeature(root));
    return info;
  }
  const QJsonArray features = root.value(QStringLiteral("features")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < features.size(); ++i) {
    info.features.push_back(jsonFeature(features.at(i).toObject()));
  }
  return info;
}

// --- XML ---------------------------------------------------------------

/** An element, read whole. */
struct XmlNode {
  QString name;
  std::vector<std::pair<QString, QString>> attributes;
  QString text;
  std::vector<std::unique_ptr<XmlNode>> children;

  QString attribute(const char* wanted) const
  {
    for (const std::pair<QString, QString>& pair : attributes) {
      if (pair.first.compare(QLatin1String(wanted), Qt::CaseInsensitive) == 0) {
        return pair.second;
      }
    }
    return QString();
  }

  /** All text inside, simplified. */
  QString allText() const
  {
    QString all = text;
    for (const std::unique_ptr<XmlNode>& child : children) {
      all += QLatin1Char(' ') + child->allText();
    }
    return all.simplified();
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

bool isGeometryName(const QString& name)
{
  return name.compare(QLatin1String("boundedBy"), Qt::CaseInsensitive) == 0 ||
         name.contains(QLatin1String("geom"), Qt::CaseInsensitive) ||
         name.compare(QLatin1String("shape"), Qt::CaseInsensitive) == 0;
}

/** A GML feature: its simple elements (geometry and nested ones left out). */
FeatureInfoFeature gmlFeature(const XmlNode& node, const QString& layer)
{
  FeatureInfoFeature feature;
  feature.layer = layer.isEmpty() ? node.name : layer;
  feature.id = node.attribute("fid");
  if (feature.id.isEmpty()) {
    feature.id = node.attribute("id");
  }
  for (const std::unique_ptr<XmlNode>& child : node.children) {
    if (isGeometryName(child->name) || !child->children.empty() ||
        child->name.compare(QLatin1String("name"), Qt::CaseInsensitive) == 0) {
      continue;
    }
    feature.attributes.emplace_back(child->name, child->text.trimmed());
  }
  return feature;
}

/** ArcGIS's <FeatureInfo><Field><FieldName/><FieldValue/></Field>…. */
FeatureInfoFeature arcgisFields(const XmlNode& node, const QString& layer)
{
  FeatureInfoFeature feature;
  feature.layer = layer;
  for (const std::unique_ptr<XmlNode>& field : node.children) {
    QString name;
    QString value;
    for (const std::unique_ptr<XmlNode>& part : field->children) {
      if (part->name == QLatin1String("FieldName")) {
        name = part->allText();
      } else if (part->name == QLatin1String("FieldValue")) {
        value = part->allText();
      }
    }
    if (!name.isEmpty()) {
      feature.attributes.emplace_back(name, value);
    }
  }
  return feature;
}

/** The layer @p node names for what is inside it, else @p layer. */
QString layerWithin(const XmlNode& node, const QString& layer)
{
  QString named = node.attribute("layername");
  if (!named.isEmpty()) {
    return named;
  }
  if (!node.name.endsWith(QLatin1String("_layer"))) {
    return layer;
  }
  // MapServer: <x_layer><gml:name>x</gml:name>….
  QString inner = node.name.left(node.name.size() - 6);
  for (const std::unique_ptr<XmlNode>& child : node.children) {
    if (child->name == QLatin1String("name")) {
      inner = child->text.trimmed();
    }
  }
  return inner;
}

bool isMemberList(const QString& name)
{
  return name == QLatin1String("featureMember") ||
         name == QLatin1String("featureMembers") ||
         name == QLatin1String("member");
}

void collectFeatures(const XmlNode& node, const QString& layer,
                     std::vector<FeatureInfoFeature>& features)
{
  if (node.name.compare(QLatin1String("FIELDS"), Qt::CaseInsensitive) == 0) {
    // ArcGIS: <FIELDS name="value" …/>.
    features.push_back(FeatureInfoFeature{layer, QString(), node.attributes});
  } else if (node.name == QLatin1String("FeatureInfo")) {
    features.push_back(arcgisFields(node, layer));
  } else if (isMemberList(node.name)) {
    // GML: each member is a feature.
    for (const std::unique_ptr<XmlNode>& member : node.children) {
      features.push_back(gmlFeature(*member, QString()));
    }
  } else if (node.name.endsWith(QLatin1String("_feature"))) {
    features.push_back(gmlFeature(node, layer));
  } else {
    const QString inner = layerWithin(node, layer);
    for (const std::unique_ptr<XmlNode>& child : node.children) {
      collectFeatures(*child, inner, features);
    }
  }
}

std::optional<WmsFeatureInfo> parseXml(const QByteArray& body,
                                       QString* errorString)
{
  QXmlStreamReader reader(body);
  while (!reader.atEnd() && !reader.isStartElement()) {
    reader.readNext();
  }
  if (!reader.isStartElement()) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WmsFeatureInfo",
                                 "The server's answer isn't valid XML."));
  }
  const std::unique_ptr<XmlNode> root = readElement(reader);
  if (reader.hasError()) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WmsFeatureInfo",
                                 "The server's answer isn't valid XML: %1")
                                 .arg(reader.errorString()));
  }
  if (root->name.contains(QLatin1String("ExceptionReport"))) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmsFeatureInfo",
                                            "The server reported an error: %1")
                    .arg(root->allText()));
  }
  WmsFeatureInfo info;
  collectFeatures(*root, QString(), info.features);
  if (info.features.empty() &&
      !root->name.contains(QLatin1String("FeatureCollection")) &&
      root->name != QLatin1String("msGMLOutput") &&
      root->name != QLatin1String("FeatureInfoResponse")) {
    info.text = root->allText();
  }
  return info;
}

// --- HTML --------------------------------------------------------------

QString decodeEntities(const QString& text)
{
  static const QRegularExpression entity(
      QStringLiteral("&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);"));
  QString decoded;
  int32_t from = 0;
  QRegularExpressionMatchIterator matches = entity.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    const QString name = match.captured(1);
    QString replacement = match.captured(0);
    if (name.startsWith(QLatin1String("#x"))) {
      bool ok = false;
      const char32_t code = name.mid(2).toUInt(&ok, 16);
      if (ok) {
        replacement = QString::fromUcs4(&code, 1);
      }
    } else if (name.startsWith(QLatin1Char('#'))) {
      bool ok = false;
      const char32_t code = name.mid(1).toUInt(&ok, 10);
      if (ok) {
        replacement = QString::fromUcs4(&code, 1);
      }
    } else if (name == QLatin1String("amp")) {
      replacement = QStringLiteral("&");
    } else if (name == QLatin1String("lt")) {
      replacement = QStringLiteral("<");
    } else if (name == QLatin1String("gt")) {
      replacement = QStringLiteral(">");
    } else if (name == QLatin1String("quot")) {
      replacement = QStringLiteral("\"");
    } else if (name == QLatin1String("apos")) {
      replacement = QStringLiteral("'");
    } else if (name == QLatin1String("nbsp")) {
      replacement = QStringLiteral(" ");
    } else {
      // The named entities servers use for units and punctuation.
      static const std::array<std::pair<const char*, char32_t>, 16> named = {
          {{"deg", 0xB0},
           {"copy", 0xA9},
           {"reg", 0xAE},
           {"euro", 0x20AC},
           {"pound", 0xA3},
           {"ndash", 0x2013},
           {"mdash", 0x2014},
           {"hellip", 0x2026},
           {"laquo", 0xAB},
           {"raquo", 0xBB},
           {"plusmn", 0xB1},
           {"micro", 0xB5},
           {"sup2", 0xB2},
           {"sup3", 0xB3},
           {"times", 0xD7},
           {"middot", 0xB7}}};
      for (const std::pair<const char*, char32_t>& entry : named) {
        if (name == QLatin1String(entry.first)) {
          replacement = QString::fromUcs4(&entry.second, 1);
        }
      }
    }
    decoded +=
        text.mid(from, static_cast<int32_t>(match.capturedStart()) - from);
    decoded += replacement;
    from = static_cast<int32_t>(match.capturedEnd());
  }
  decoded += text.mid(from);
  return decoded;
}

/** The text of an HTML fragment: tags dropped, entities decoded. */
QString htmlText(const QString& html, bool keepLines)
{
  QString text = html;
  if (keepLines) {
    static const QRegularExpression breaks(
        QStringLiteral("<(br|/p|/tr|/div|/li|/h[1-6]|/table)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(breaks, QStringLiteral("\n"));
  }
  static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
  text.replace(tag, QStringLiteral(" "));
  text = decodeEntities(text);
  if (!keepLines) {
    return text.simplified();
  }
  QStringList lines;
  const QStringList raw = text.split(QLatin1Char('\n'));
  for (const QString& line : raw) {
    const QString simple = line.simplified();
    if (!simple.isEmpty()) {
      lines.append(simple);
    }
  }
  return lines.join(QLatin1Char('\n'));
}

/** The rows of a table's HTML: whether each is a header row, its cells. */
std::vector<std::pair<bool, QStringList>> tableRows(const QString& content)
{
  static const QRegularExpression row(
      QStringLiteral("<tr\\b[^>]*>(.*?)</tr\\s*>"),
      QRegularExpression::CaseInsensitiveOption |
          QRegularExpression::DotMatchesEverythingOption);
  static const QRegularExpression cell(
      QStringLiteral("<(t[hd])\\b[^>]*>(.*?)</t[hd]\\s*>"),
      QRegularExpression::CaseInsensitiveOption |
          QRegularExpression::DotMatchesEverythingOption);
  std::vector<std::pair<bool, QStringList>> rows;
  QRegularExpressionMatchIterator rowMatches = row.globalMatch(content);
  while (rowMatches.hasNext()) {
    const QString rowHtml = rowMatches.next().captured(1);
    QStringList cells;
    bool header = true;
    QRegularExpressionMatchIterator cellMatches = cell.globalMatch(rowHtml);
    while (cellMatches.hasNext()) {
      const QRegularExpressionMatch match = cellMatches.next();
      header = header && match.captured(1).toLower() == QLatin1String("th");
      cells.append(htmlText(match.captured(2), false));
    }
    if (!cells.isEmpty()) {
      rows.emplace_back(header, cells);
    }
  }
  return rows;
}

/** Adds a table's features: one per row under a header, or name–value. */
void addTableFeatures(const QString& content,
                      std::vector<FeatureInfoFeature>& features)
{
  static const QRegularExpression caption(
      QStringLiteral("<caption\\b[^>]*>(.*?)</caption\\s*>"),
      QRegularExpression::CaseInsensitiveOption |
          QRegularExpression::DotMatchesEverythingOption);
  const QString layer = htmlText(caption.match(content).captured(1), false);
  const std::vector<std::pair<bool, QStringList>> rows = tableRows(content);
  if (rows.empty()) {
    return;
  }
  if (rows.front().first && rows.size() > 1) {
    const QStringList& headers = rows.front().second;
    for (std::size_t r = 1; r < rows.size(); ++r) {
      FeatureInfoFeature feature;
      feature.layer = layer;
      const QStringList& values = rows.at(r).second;
      for (int32_t c = 0; c < values.size(); ++c) {
        feature.attributes.emplace_back(
            headers.value(c, QString::number(c + 1)), values.at(c));
      }
      features.push_back(feature);
    }
    return;
  }
  const bool pairs = std::all_of(rows.begin(), rows.end(),
                                 [](const std::pair<bool, QStringList>& r) {
                                   return r.second.size() == 2;
                                 });
  if (pairs) {
    FeatureInfoFeature feature;
    feature.layer = layer;
    for (const std::pair<bool, QStringList>& r : rows) {
      feature.attributes.emplace_back(r.second.at(0), r.second.at(1));
    }
    features.push_back(feature);
  }
}

std::optional<WmsFeatureInfo> parseHtml(const QByteArray& body)
{
  QString html = QString::fromUtf8(body);
  static const QRegularExpression hidden(
      QStringLiteral("<(script|style|head)\\b[^>]*>.*?</\\1\\s*>"),
      QRegularExpression::CaseInsensitiveOption |
          QRegularExpression::DotMatchesEverythingOption);
  html.replace(hidden, QString());
  static const QRegularExpression comment(
      QStringLiteral("<!--.*?-->"),
      QRegularExpression::DotMatchesEverythingOption);
  html.replace(comment, QString());
  static const QRegularExpression table(
      QStringLiteral("<table\\b[^>]*>(.*?)</table\\s*>"),
      QRegularExpression::CaseInsensitiveOption |
          QRegularExpression::DotMatchesEverythingOption);
  WmsFeatureInfo info;
  QRegularExpressionMatchIterator tables = table.globalMatch(html);
  while (tables.hasNext()) {
    addTableFeatures(tables.next().captured(1), info.features);
  }
  if (info.features.empty()) {
    info.text = htmlText(html, true);
  }
  return info;
}

// --- Plain text --------------------------------------------------------

/** Reads plain-text answers line by line. */
class PlainReader
{
public:
  /** Takes one line. */
  void read(const QString& raw)
  {
    static const QRegularExpression layerLine(
        QStringLiteral("^\\s*(?:Results for FeatureType|Layer)\\s+'([^']+)'"));
    static const QRegularExpression featureLine(
        QStringLiteral("^\\s*Feature\\s+(\\S+?):?\\s*$"));
    static const QRegularExpression rule(QStringLiteral("^\\s*[-=]{3,}\\s*$"));
    const QString line = raw.trimmed();
    QRegularExpressionMatch match = layerLine.match(line);
    if (match.hasMatch()) {
      finish();
      m_layer = match.captured(1);
      return;
    }
    match = featureLine.match(line);
    if (match.hasMatch()) {
      finish();
      m_current = FeatureInfoFeature{m_layer, match.captured(1), {}};
      return;
    }
    if (rule.match(line).hasMatch()) {
      finish();
      return;
    }
    attribute(line);
  }

  /** Ends the feature being read. */
  void finish()
  {
    if (m_current && !m_current->attributes.empty()) {
      m_features.push_back(*m_current);
    }
    m_current.reset();
  }

  /** Returns the features read. */
  std::vector<FeatureInfoFeature>& features() { return m_features; }

private:
  void attribute(const QString& line)
  {
    static const QRegularExpression pair(
        QStringLiteral("^\\s*([^=]+?)\\s*=\\s*(.*?)\\s*$"));
    const QRegularExpressionMatch match = pair.match(line);
    if (!match.hasMatch()) {
      return;
    }
    QString value = match.captured(2);
    if (value.size() >= 2 && value.startsWith(QLatin1Char('\'')) &&
        value.endsWith(QLatin1Char('\''))) {
      value = value.mid(1, value.size() - 2);
    }
    if (value.startsWith(QLatin1String("[GEOMETRY"))) {
      return;
    }
    if (!m_current) {
      m_current = FeatureInfoFeature{m_layer, QString(), {}};
    }
    m_current->attributes.emplace_back(match.captured(1), value);
  }

  QString m_layer;
  std::optional<FeatureInfoFeature> m_current;
  std::vector<FeatureInfoFeature> m_features;
};

std::optional<WmsFeatureInfo> parsePlain(const QByteArray& body)
{
  const QString text = QString::fromUtf8(body);
  PlainReader reader;
  const QStringList lines = text.split(QLatin1Char('\n'));
  for (const QString& line : lines) {
    reader.read(line);
  }
  reader.finish();
  WmsFeatureInfo info;
  info.features = std::move(reader.features());
  if (info.features.empty()) {
    const QString trimmed = text.trimmed();
    // "no features were found", "Search returned no results": nothing.
    if (!trimmed.contains(QLatin1String("no features"), Qt::CaseInsensitive) &&
        !trimmed.contains(QLatin1String("no results"), Qt::CaseInsensitive)) {
      info.text = trimmed;
    }
  }
  return info;
}

} // namespace

std::optional<WmsFeatureInfo> parseFeatureInfo(const QByteArray& body,
                                               const QString& contentType,
                                               QString* errorString)
{
  const QString type = contentType.toLower();
  const QByteArray start = body.trimmed().left(256).toLower();
  if (type.contains(QLatin1String("json")) ||
      (type.isEmpty() && start.startsWith('{'))) {
    return parseJson(body, errorString);
  }
  if (type.contains(QLatin1String("html")) || start.contains("<html") ||
      start.contains("<!doctype html")) {
    return parseHtml(body);
  }
  if (type.contains(QLatin1String("xml")) ||
      type.contains(QLatin1String("gml")) || start.startsWith('<')) {
    return parseXml(body, errorString);
  }
  return parsePlain(body);
}

} // namespace ts
