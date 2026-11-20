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
 * What a WMS server offers, read from its GetCapabilities document.
 */
#ifndef TS_WMS_WMSCAPABILITIES_H
#define TS_WMS_WMSCAPABILITIES_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ts/tiling/tilekey.h"

namespace ts
{

/** A style a layer can be drawn in. */
struct WmsStyle {
  /** Name, for the STYLES parameter. */
  QString name;
  /** Title, for the user. */
  QString title;
  /** A legend image, if the server has one. */
  QUrl legendUrl;
};

/** A BoundingBox as the document gives it, in the axis order of its CRS. */
struct WmsBoundingBox {
  /** The CRS (or SRS) code. */
  QString crs;
  /** First axis minimum. */
  double minX = 0.0;
  /** Second axis minimum. */
  double minY = 0.0;
  /** First axis maximum. */
  double maxX = 0.0;
  /** Second axis maximum. */
  double maxY = 0.0;
};

/**
 * A dimension of a layer (WMS 1.3.0 Dimension; 1.1.1 Dimension with its
 * Extent): TIME, ELEVATION or a named one.
 *
 * @reentrant
 */
struct WmsDimension {
  /** Name, lower case as WMS compares it ("time", "elevation", …). */
  QString name;
  /** Units ("ISO8601" for time). */
  QString units;
  /** The value used when the request gives none. */
  QString defaultValue;
  /**
   * The allowed values as written: a comma-separated list of values and
   * start/end/resolution intervals (see TimeExtent for time).
   */
  QString extent;
  /** True if the server accepts several values at once. */
  bool multipleValues = false;
  /** True if the server answers with the nearest value it has. */
  bool nearestValue = false;
  /** True if "current" is allowed (the latest data). */
  bool current = false;

  /** Returns true for the TIME dimension. */
  bool isTime() const { return name == QLatin1String("time"); }
};

/**
 * A layer, with what it inherits from its parents already applied (WMS
 * 1.3.0 section 7.2.4.8, the same in 1.1.1): CRS, styles and
 * authority are added to the parent's; bounding boxes, scale limits,
 * attribution and the attributes replace the parent's when given.
 *
 * @reentrant
 */
struct WmsLayer {
  /** Name for GetMap; empty for a layer that only groups others. */
  QString name;
  /** Title, for the user. */
  QString title;
  /** Description. */
  QString abstract;
  /** The CRS it can be drawn in, as written (EPSG:4326, CRS:84, …). */
  QStringList crs;
  /** Where it has data, longitude and latitude; nothing if not given. */
  std::optional<GeoRect> geographicBounds;
  /** Its bounding boxes, by CRS. */
  std::vector<WmsBoundingBox> boundingBoxes;
  /** Its styles (the first is the default). */
  std::vector<WmsStyle> styles;
  /** True if it covers everything below it (no transparent pixels). */
  bool opaque = false;
  /** True if GetFeatureInfo works on it. */
  bool queryable = false;
  /** True if the server can't draw part of it (whole extent only). */
  bool noSubsets = false;
  /** Fixed image width the server requires, or 0. */
  int32_t fixedWidth = 0;
  /** Fixed image height the server requires, or 0. */
  int32_t fixedHeight = 0;
  /** Drawn only at scales coarser than 1 : this (0: no limit). */
  double minScaleDenominator = 0.0;
  /** Drawn only at scales finer than 1 : this (0: no limit). */
  double maxScaleDenominator = 0.0;
  /**
   * Its dimensions (inherited: a child's of the same name replaces its
   * parent's).
   */
  std::vector<WmsDimension> dimensions;
  /** Who to credit, as text. */
  QString attributionTitle;
  /** Who to credit, as a link. */
  QUrl attributionUrl;
  /** The layers under this one. */
  std::vector<WmsLayer> children;

  /**
   * Returns true if it can be drawn in @p code (codes compared without
   * regard to case).
   */
  bool supportsCrs(const QString& code) const;

  /** Returns the TIME dimension, or null if it has none. */
  const WmsDimension* timeDimension() const;
};

/**
 * The parts of a WMS capabilities document Terra Soar uses.
 *
 * @reentrant
 */
struct WmsCapabilities {
  /** "1.1.1" or "1.3.0" (or what the server said). */
  QString version;
  /** The service's title. */
  QString title;
  /** The service's description. */
  QString abstract;
  /** Fees, as the server states them ("none" when free). */
  QString fees;
  /** Access constraints, as the server states them. */
  QString accessConstraints;
  /** Largest image width the server draws, or 0 if not stated. */
  int32_t maxWidth = 0;
  /** Largest image height the server draws, or 0 if not stated. */
  int32_t maxHeight = 0;
  /** Most layers in one GetMap, or 0 if not stated. */
  int32_t layerLimit = 0;
  /** Where GetMap requests go (HTTP GET). */
  QUrl getMapUrl;
  /** Image formats GetMap can return. */
  QStringList getMapFormats;
  /** Where GetFeatureInfo requests go (HTTP GET); empty without one. */
  QUrl getFeatureInfoUrl;
  /** Formats GetFeatureInfo can answer in. */
  QStringList getFeatureInfoFormats;
  /** The top-level layers (usually one, containing the rest). */
  std::vector<WmsLayer> layers;

  /** Returns the layer named @p name, or null. */
  const WmsLayer* findLayer(const QString& name) const;

  /** Returns every layer with a name, depth first. */
  std::vector<const WmsLayer*> namedLayers() const;
};

/**
 * Reads a GetCapabilities document of WMS 1.1.0, 1.1.1 or 1.3.0. Returns
 * nothing and sets @p errorString (translated) if it isn't one: XML
 * errors, an HTML page, or a ServiceExceptionReport, whose message is
 * passed on.
 *
 * Accepts what servers commonly get wrong: lower-case CRS codes, several
 * SRS codes in one element (1.1.0 style), empty elements, a DTD with an
 * internal subset, and a GetMap URL without a trailing "?".
 *
 * @reentrant
 */
std::optional<WmsCapabilities> parseWmsCapabilities(const QByteArray& document,
                                                    QString* errorString);

} // namespace ts

#endif // TS_WMS_WMSCAPABILITIES_H
