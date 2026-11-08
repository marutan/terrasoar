// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/layers/layerdefinition.h"

#include <array>
#include <utility>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

namespace ts
{

namespace
{

constexpr std::array<std::pair<LayerType, const char*>, 3> typeNames = {{
    {LayerType::Imagery, "imagery"},
    {LayerType::Vector, "vector"},
    {LayerType::Elevation, "elevation"},
}};

constexpr std::array<std::pair<LayerOrigin, const char*>, 3> originNames = {{
    {LayerOrigin::Builtin, "builtin"},
    {LayerOrigin::Catalogue, "catalogue"},
    {LayerOrigin::User, "user"},
}};

/** Names of the built-in definitions, for lupdate; translated on load. */
constexpr std::array<const char*, 2> builtinNames = {
    QT_TRANSLATE_NOOP("ts::BuiltinLayers", "Checkerboard"),
    QT_TRANSLATE_NOOP("ts::BuiltinLayers", "White, 50 %"),
};

/** The built-in definitions, in the same JSON as catalogues and projects. */
const char* const builtinJson = R"([
  { "id": "builtin:checkerboard", "origin": "builtin", "type": "imagery",
    "name": "Checkerboard",
    "source": { "kind": "checkerboard", "tileSize": 256 } },
  { "id": "builtin:solid-white-50", "origin": "builtin", "type": "imagery",
    "name": "White, 50 %",
    "source": { "kind": "solid", "color": "#80ffffff", "tileSize": 64 } }
])";

std::optional<LayerDefinition> fail(QString* errorString,
                                    const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return std::nullopt;
}

} // namespace

QString layerTypeName(LayerType type)
{
  for (const std::pair<LayerType, const char*>& entry : typeNames) {
    if (entry.first == type) {
      return QLatin1String(entry.second);
    }
  }
  return QString();
}

QString layerOriginName(LayerOrigin origin)
{
  for (const std::pair<LayerOrigin, const char*>& entry : originNames) {
    if (entry.first == origin) {
      return QLatin1String(entry.second);
    }
  }
  return QString();
}

QJsonObject LayerDefinition::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("id"), id);
  json.insert(QStringLiteral("origin"), layerOriginName(origin));
  json.insert(QStringLiteral("type"), layerTypeName(type));
  json.insert(QStringLiteral("name"), name);
  json.insert(QStringLiteral("source"), source);
  return json;
}

std::optional<LayerDefinition>
LayerDefinition::fromJson(const QJsonObject& json, QString* errorString)
{
  LayerDefinition definition;
  definition.id = json.value(QStringLiteral("id")).toString();
  definition.name = json.value(QStringLiteral("name")).toString();
  definition.source = json.value(QStringLiteral("source")).toObject();
  if (definition.id.isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::LayerDefinition",
                                            "A layer definition has no id."));
  }
  const QString type = json.value(QStringLiteral("type")).toString();
  bool typeFound = false;
  for (const std::pair<LayerType, const char*>& entry : typeNames) {
    if (type == QLatin1String(entry.second)) {
      definition.type = entry.first;
      typeFound = true;
    }
  }
  if (!typeFound) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::LayerDefinition",
                    "Layer definition %1 has an unknown type \"%2\".")
                    .arg(definition.id, type));
  }
  const QString origin = json.value(QStringLiteral("origin")).toString();
  bool originFound = false;
  for (const std::pair<LayerOrigin, const char*>& entry : originNames) {
    if (origin == QLatin1String(entry.second)) {
      definition.origin = entry.first;
      originFound = true;
    }
  }
  if (!originFound) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::LayerDefinition",
                    "Layer definition %1 has an unknown origin \"%2\".")
                    .arg(definition.id, origin));
  }
  if (definition.source.value(QStringLiteral("kind")).toString().isEmpty()) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::LayerDefinition",
                                 "Layer definition %1 has no source kind.")
                                 .arg(definition.id));
  }
  return definition;
}

bool LayerDefinition::operator==(const LayerDefinition& other) const
{
  return id == other.id && origin == other.origin && type == other.type &&
         name == other.name && source == other.source;
}

std::vector<LayerDefinition> builtinLayerDefinitions()
{
  static_cast<void>(builtinNames);
  std::vector<LayerDefinition> definitions;
  const QJsonArray array =
      QJsonDocument::fromJson(QByteArray(builtinJson)).array();
  definitions.reserve(static_cast<std::size_t>(array.size()));
  // An index loop: QJsonArray's iterator yields QJsonValueConstRef on
  // Qt 6 and QJsonValue on Qt 5, so no range-for spelling suits both
  // (clang-tidy wants no conversion, clazy a reference).
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < array.size(); ++i) {
    std::optional<LayerDefinition> definition =
        LayerDefinition::fromJson(array.at(i).toObject(), nullptr);
    if (definition) {
      definition->name = QCoreApplication::translate(
          "ts::BuiltinLayers", definition->name.toUtf8().constData());
      definitions.push_back(std::move(*definition));
    }
  }
  return definitions;
}

} // namespace ts
