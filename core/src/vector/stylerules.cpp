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
#include "ts/vector/stylerules.h"

#include <algorithm>
#include <cstdint>

#include <QColor>
#include <QVariant>

#include "ts/vector/vectorstyle.h"

namespace ts
{

namespace
{

std::optional<QColor> colourIn(const QJsonObject& json, const char* key)
{
  const QColor colour =
      colorFromRgbaHex(json.value(QLatin1String(key)).toString());
  if (!colour.isValid()) {
    return std::nullopt;
  }
  return colour;
}

std::optional<double> numberIn(const QJsonObject& json, const char* key)
{
  const QJsonValue value = json.value(QLatin1String(key));
  if (!value.isDouble() || !(value.toDouble() >= 0.0)) {
    return std::nullopt;
  }
  return value.toDouble();
}

/** A CSS-style colour ("#rgb", "#rrggbb", a name) with @p opacity. */
std::optional<QColor> cssColour(const QVariant& value, const QVariant& opacity)
{
  if (!value.isValid()) {
    return std::nullopt;
  }
  QColor colour(value.toString().trimmed());
  if (!colour.isValid()) {
    return std::nullopt;
  }
  bool ok = false;
  const double alpha = opacity.toDouble(&ok);
  if (opacity.isValid() && ok) {
    colour.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
  }
  return colour;
}

} // namespace

bool StyleRule::matches(const Feature& feature) const
{
  if (attribute.isEmpty()) {
    return true;
  }
  const QVariantMap::const_iterator found = feature.attributes.find(attribute);
  if (found == feature.attributes.end()) {
    return false;
  }
  if (equals && found.value().toString() != *equals) {
    return false;
  }
  if (minimum || maximum) {
    bool ok = false;
    const double number = found.value().toDouble(&ok);
    if (!ok || (minimum && number < *minimum) ||
        (maximum && number >= *maximum)) {
      return false;
    }
  }
  return true;
}

QJsonObject StyleRule::toJson() const
{
  QJsonObject json;
  if (!attribute.isEmpty()) {
    json.insert(QStringLiteral("attribute"), attribute);
  }
  if (equals) {
    json.insert(QStringLiteral("equals"), *equals);
  }
  if (minimum) {
    json.insert(QStringLiteral("min"), *minimum);
  }
  if (maximum) {
    json.insert(QStringLiteral("max"), *maximum);
  }
  if (style.fill) {
    json.insert(QStringLiteral("fill"), colorToRgbaHex(*style.fill));
  }
  if (style.outline) {
    json.insert(QStringLiteral("outline"), colorToRgbaHex(*style.outline));
  }
  if (style.outlineWidthPx) {
    json.insert(QStringLiteral("outlineWidthPx"), *style.outlineWidthPx);
  }
  if (style.pointSizePx) {
    json.insert(QStringLiteral("pointSizePx"), *style.pointSizePx);
  }
  return json;
}

std::optional<StyleRule> StyleRule::fromJson(const QJsonObject& json)
{
  StyleRule rule;
  rule.attribute = json.value(QStringLiteral("attribute")).toString();
  const QJsonValue equals = json.value(QStringLiteral("equals"));
  if (equals.isString()) {
    rule.equals = equals.toString();
  } else if (equals.isDouble()) {
    rule.equals = QVariant(equals.toDouble()).toString();
  } else if (equals.isBool()) {
    rule.equals =
        equals.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  }
  if (json.value(QStringLiteral("min")).isDouble()) {
    rule.minimum = json.value(QStringLiteral("min")).toDouble();
  }
  if (json.value(QStringLiteral("max")).isDouble()) {
    rule.maximum = json.value(QStringLiteral("max")).toDouble();
  }
  rule.style.fill = colourIn(json, "fill");
  rule.style.outline = colourIn(json, "outline");
  rule.style.outlineWidthPx = numberIn(json, "outlineWidthPx");
  rule.style.pointSizePx = numberIn(json, "pointSizePx");
  if (rule.style.isEmpty()) {
    return std::nullopt;
  }
  return rule;
}

std::vector<StyleRule> styleRulesFromJson(const QJsonArray& json)
{
  std::vector<StyleRule> rules;
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < json.size(); ++i) {
    const std::optional<StyleRule> rule =
        StyleRule::fromJson(json.at(i).toObject());
    if (rule) {
      rules.push_back(*rule);
    }
  }
  return rules;
}

std::optional<FeatureStyle> ownStyle(const Feature& feature)
{
  const QVariantMap& a = feature.attributes;
  FeatureStyle style;
  if (feature.type == Feature::Type::Points) {
    style.outline =
        cssColour(a.value(QStringLiteral("marker-color")), QVariant());
    const QString size = a.value(QStringLiteral("marker-size")).toString();
    if (size == QLatin1String("small")) {
      style.pointSizePx = 5.0;
    } else if (size == QLatin1String("medium")) {
      style.pointSizePx = 8.0;
    } else if (size == QLatin1String("large")) {
      style.pointSizePx = 12.0;
    }
  } else {
    style.outline = cssColour(a.value(QStringLiteral("stroke")),
                              a.value(QStringLiteral("stroke-opacity")));
    bool ok = false;
    const double width = a.value(QStringLiteral("stroke-width")).toDouble(&ok);
    if (ok && width >= 0.0) {
      style.outlineWidthPx = width;
    }
    if (feature.type == Feature::Type::Polygon) {
      style.fill = cssColour(a.value(QStringLiteral("fill")),
                             a.value(QStringLiteral("fill-opacity")));
    }
  }
  if (style.isEmpty()) {
    return std::nullopt;
  }
  return style;
}

void applyStyleRules(std::vector<Feature>& features,
                     const std::vector<StyleRule>& rules)
{
  for (Feature& feature : features) {
    feature.style.reset();
    for (const StyleRule& rule : rules) {
      if (rule.matches(feature)) {
        feature.style = rule.style;
        break;
      }
    }
    if (!feature.style) {
      feature.style = ownStyle(feature);
    }
  }
}

} // namespace ts
