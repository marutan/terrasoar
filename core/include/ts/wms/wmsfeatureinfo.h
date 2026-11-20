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
 * What a WMS answers to GetFeatureInfo: the features at a point, read
 * from GeoJSON, GML and the other XML forms servers use, HTML tables or
 * plain text.
 */
#ifndef TS_WMS_WMSFEATUREINFO_H
#define TS_WMS_WMSFEATUREINFO_H

#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QString>

namespace ts
{

/** One feature in a GetFeatureInfo answer. */
struct FeatureInfoFeature {
  /** The layer (or feature type) it belongs to, if the answer says. */
  QString layer;
  /** Its identifier, if the answer gives one. */
  QString id;
  /** Its attributes, in the answer's order: name and value as text. */
  std::vector<std::pair<QString, QString>> attributes;
};

/**
 * A GetFeatureInfo answer: its features, or, when the server answered
 * with prose rather than a table, the text.
 *
 * @reentrant
 */
struct WmsFeatureInfo {
  /** The features found. */
  std::vector<FeatureInfoFeature> features;
  /** Text of an answer that isn't a list of features (HTML, plain). */
  QString text;

  /** Returns true if nothing was found there. */
  bool isEmpty() const { return features.empty() && text.trimmed().isEmpty(); }
};

/**
 * Reads a GetFeatureInfo answer of @p contentType (the response's header,
 * or the INFO_FORMAT asked for):
 *
 * - **GeoJSON** (`application/json`, GeoServer, QGIS Server, MapServer):
 *   each feature's id and properties; nested values as compact JSON.
 * - **XML**: GML feature collections (`featureMember` and
 *   `featureMembers`: each member's simple elements, geometry left out),
 *   MapServer's `msGMLOutput` (`<x_layer><x_feature>…`), ArcGIS's
 *   `FeatureInfoResponse` (`<FIELDS name="value"…/>`) and
 *   `FeatureInfoCollection` (`<Field><FieldName>…<FieldValue>…`); other XML
 *   becomes text.
 * - **HTML**: each table with a header row becomes features (one per row,
 *   named by the headers); otherwise the page's text. Nothing is loaded
 *   from it and scripts and styles are dropped.
 * - **Plain text**: `name = value` lines become attributes (GeoServer's
 *   and MapServer's layouts: "Results for FeatureType 'x':", "Layer 'x'",
 *   "Feature 1:", lines of dashes between features); else the text.
 *
 * Returns nothing and sets @p errorString (translated) for a service
 * exception (its message passed on) or an answer that can't be read.
 */
std::optional<WmsFeatureInfo> parseFeatureInfo(const QByteArray& body,
                                               const QString& contentType,
                                               QString* errorString);

} // namespace ts

#endif // TS_WMS_WMSFEATUREINFO_H
