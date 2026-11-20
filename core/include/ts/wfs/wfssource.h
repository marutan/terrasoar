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
 * A WFS feature type as a source of vector features: its requests, and
 * reading the features the server sends.
 */
#ifndef TS_WFS_WFSSOURCE_H
#define TS_WFS_WFSSOURCE_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

struct WfsCapabilities;

/**
 * Everything needed to request one WFS feature type tile by tile. Stored
 * in layer definitions as `{"kind": "wfs", …}` (toJson()).
 *
 * Features are asked for by the bounding box of a stored tile
 * (storedLevel()): tiles coarser than minimumLevel show none (the layer
 * appears when zoomed in that far), finer ones than maximumLevel use
 * their ancestor's. Each tile's features come in pages of pageSize, at
 * most maximumPages of them.
 *
 * @reentrant
 */
struct WfsSource {
  /** Where GetFeature requests go. */
  QUrl url;
  /** The service's address as the user gave it (for the catalogue). */
  QUrl service;
  /** "2.0.0" (or "1.1.0"). */
  QString version = QStringLiteral("2.0.0");
  /** The feature type's qualified name. */
  QString typeName;
  /** The output format asked for (chooseWfsOutputFormat()). */
  QString outputFormat;
  /** True if the server pages (STARTINDEX). */
  bool paging = true;
  /** Features per request. */
  int32_t pageSize = 1000;
  /** Most requests per tile. */
  int32_t maximumPages = 5;
  /** Where the feature type has features. */
  std::optional<GeoRect> bounds;
  /** Coarsest quadtree level with features. */
  int32_t minimumLevel = 0;
  /** Deepest level asked for; deeper tiles use their ancestor's. */
  int32_t maximumLevel = 8;
  /** Name for the user. */
  QString title;
  /** Who to credit. */
  QString attribution;
  /** Style rules (ts::StyleRule) applied to the features as they load. */
  QJsonArray styleRules;

  /** Returns the level of the tile serving @p level, or −1 for none. */
  int32_t storedLevel(int32_t level) const;

  /** Returns a string identifying the features (address, type, format). */
  QString id() const;

  /** Returns the source object for a layer definition. */
  QJsonObject toJson() const;

  /**
   * Reads a source object; nothing (and @p errorString, translated) if
   * the address or the type is missing.
   */
  static std::optional<WfsSource> fromJson(const QJsonObject& json,
                                           QString* errorString);
};

/**
 * Returns the format to ask for of @p offered: GeoJSON first (always
 * longitude first, RFC 7946), then GML 3.2, 3.1 and 2; empty if none of
 * them is offered.
 */
QString chooseWfsOutputFormat(const QStringList& offered);

/**
 * Returns the first quadtree level whose tile covers @p bounds' larger
 * side (so the whole feature type is asked for in a tile or two), 0 for
 * none.
 */
int32_t wfsMinimumLevel(const std::optional<GeoRect>& bounds);

/**
 * Returns the GetFeature request for the features in @p rect, from the
 * @p startIndex-th: TYPENAMES, BBOX and SRSNAME in
 * urn:ogc:def:crs:EPSG::4326 (latitude first, as WFS 2.0 requires),
 * OUTPUTFORMAT, COUNT and STARTINDEX (TYPENAME and MAXFEATURES for 1.1.0).
 */
QUrl getFeatureUrl(const WfsSource& source, const GeoRect& rect,
                   int64_t startIndex);

/**
 * Returns a source for feature type @p typeName of @p capabilities;
 * nothing (and @p errorString, translated) if there is no such type, no
 * GetFeature address or no output format Terra Soar reads.
 */
std::optional<WfsSource> wfsSourceFor(const WfsCapabilities& capabilities,
                                      const QString& typeName,
                                      QString* errorString);

/** One page of a GetFeature answer. */
struct WfsPage {
  /** Its features (a multi-polygon becomes one feature per polygon). */
  std::vector<Feature> features;
  /** Features the server says it sent (numberReturned, else counted). */
  int64_t returned = 0;
  /** Features the server says match in all, if it says. */
  std::optional<int64_t> matched;
};

/**
 * Reads a GetFeature answer of @p contentType (else by its first bytes):
 *
 * - GeoJSON: longitude first whatever its "crs" says (GeoServer names
 *   urn:ogc:def:crs:EPSG::4326 and still writes longitude first).
 * - GML 3.2, 3.1 and 2 (`member`, `featureMember`, `featureMembers`):
 *   Point, LineString, Curve, Polygon, Surface, LinearRing and their
 *   Multi forms; `pos`, `posList` (with srsDimension) and GML 2
 *   `coordinates`. Axis order from the nearest srsName: latitude first
 *   for urn:ogc:def:crs:EPSG::4326 and the OGC URL, longitude first for
 *   EPSG:4326, the epsg.xml#4326 form and CRS84; without one, latitude
 *   first (as asked for).
 *
 * Attributes are the features' simple properties. Returns nothing and
 * sets @p errorString (translated) for an exception report or an answer
 * that can't be read.
 */
std::optional<WfsPage> parseWfsFeatures(const QByteArray& body,
                                        const QString& contentType,
                                        QString* errorString);

} // namespace ts

#endif // TS_WFS_WFSSOURCE_H
