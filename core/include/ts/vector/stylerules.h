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
/**
 * @file
 * Per-feature styles: rules choosing a style by a feature's attributes,
 * and styles features carry themselves.
 */
#ifndef TS_VECTOR_STYLERULES_H
#define TS_VECTOR_STYLERULES_H

#include <optional>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * One rule: features whose attribute matches get its style. Written as
 * `{"attribute": "class", "equals": "motorway", "outline": "#e8503aff",
 * "outlineWidthPx": 3}`, or with `"min"` and `"max"` (numbers, either may
 * be left out) instead of `"equals"`. Without `"attribute"` it matches
 * every feature (a default at the end).
 *
 * @reentrant
 */
struct StyleRule {
  /** The attribute looked at; empty for every feature. */
  QString attribute;
  /** The value it must have (compared as text), if set. */
  std::optional<QString> equals;
  /** Lowest number it may have (inclusive), if set. */
  std::optional<double> minimum;
  /** Highest number it may have (exclusive), if set. */
  std::optional<double> maximum;
  /** What matching features look like. */
  FeatureStyle style;

  /** Returns true if @p feature matches. */
  bool matches(const Feature& feature) const;

  /** Returns the rule as JSON. */
  QJsonObject toJson() const;

  /** Reads a rule; nothing if it sets no style. */
  static std::optional<StyleRule> fromJson(const QJsonObject& json);
};

/** Reads a list of rules, skipping those that can't be used. */
std::vector<StyleRule> styleRulesFromJson(const QJsonArray& json);

/**
 * Returns the style a feature gives itself in its attributes, in the
 * simplestyle names GeoJSON files use (`stroke`, `stroke-width`,
 * `stroke-opacity`, `fill`, `fill-opacity`, `marker-color`,
 * `marker-size`), or nothing.
 */
std::optional<FeatureStyle> ownStyle(const Feature& feature);

/**
 * Gives each of @p features its style: the first matching rule's, else
 * its own (ownStyle()), else none (its layer's).
 */
void applyStyleRules(std::vector<Feature>& features,
                     const std::vector<StyleRule>& rules);

} // namespace ts

#endif // TS_VECTOR_STYLERULES_H
