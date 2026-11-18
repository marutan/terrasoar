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
 * What a WMTS server offers, read from its capabilities document.
 */
#ifndef TS_WMTS_WMTSCAPABILITIES_H
#define TS_WMTS_WMTSCAPABILITIES_H

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

/**
 * One tile matrix: a grid of equal tiles at one scale (WMTS 1.0.0
 * section 6.1). Corner coordinates are kept in the CRS's own axis order,
 * as the document gives them; WmtsTileMatrixSet::crs says which that is.
 *
 * @reentrant
 */
struct WmtsTileMatrix {
  /** Identifier, for the TILEMATRIX parameter. */
  QString identifier;
  /** Scale denominator (standardized 0.28 mm pixels). */
  double scaleDenominator = 0.0;
  /** First axis of the top-left corner, in the CRS's axis order. */
  double topLeftFirst = 0.0;
  /** Second axis of the top-left corner, in the CRS's axis order. */
  double topLeftSecond = 0.0;
  /** Tile width, pixels. */
  int32_t tileWidth = 256;
  /** Tile height, pixels. */
  int32_t tileHeight = 256;
  /** Number of tile columns. */
  int64_t matrixWidth = 1;
  /** Number of tile rows. */
  int64_t matrixHeight = 1;
};

/**
 * A tile matrix set: the CRS and the matrices, coarsest usually first
 * (the order in the document, kept).
 *
 * @reentrant
 */
struct WmtsTileMatrixSet {
  /** Identifier, for the TILEMATRIXSET parameter. */
  QString identifier;
  /** The SupportedCRS as the document writes it (often a URN). */
  QString supportedCrs;
  /** The well-known scale set, if it names one (a URN), or empty. */
  QString wellKnownScaleSet;
  /** The matrices, in document order. */
  std::vector<WmtsTileMatrix> matrices;

  /** Returns the matrix with @p identifier, or null. */
  const WmtsTileMatrix* findMatrix(const QString& identifier) const;
};

/** Where a layer has tiles within one matrix (TileMatrixLimits). */
struct WmtsTileMatrixLimits {
  /** The matrix's identifier. */
  QString tileMatrix;
  /** First row with tiles. */
  int64_t minRow = 0;
  /** Last row with tiles. */
  int64_t maxRow = 0;
  /** First column with tiles. */
  int64_t minColumn = 0;
  /** Last column with tiles. */
  int64_t maxColumn = 0;
};

/**
 * A layer's link to a tile matrix set, with the limits within it. Without
 * limits every tile of every matrix may exist; with limits, only the
 * matrices listed do, within their rows and columns.
 */
struct WmtsTileMatrixSetLink {
  /** The set's identifier. */
  QString tileMatrixSet;
  /** Limits per matrix; empty for none. */
  std::vector<WmtsTileMatrixLimits> limits;
};

/** A style a layer can be drawn in. */
struct WmtsStyle {
  /** Identifier, for the STYLE parameter. */
  QString identifier;
  /** Title, for the user. */
  QString title;
  /** True for the layer's default style. */
  bool isDefault = false;
  /** A legend image, if the server has one. */
  QUrl legendUrl;
};

/** A dimension of a layer other than space (TIME, elevation, …). */
struct WmtsDimension {
  /** Identifier, as a request parameter or template variable. */
  QString identifier;
  /** Title, for the user. */
  QString title;
  /** Unit of measure, if stated. */
  QString unitOfMeasure;
  /** The value used when none is chosen. */
  QString defaultValue;
  /** The allowed values (single values, lists or ranges as written). */
  QStringList values;
};

/** A RESTful address template for a layer's tiles (ResourceURL). */
struct WmtsResourceUrl {
  /** Image format of the tiles at this address. */
  QString format;
  /** "tile" for tiles ("FeatureInfo" ones are ignored). */
  QString resourceType;
  /** The template, with {TileMatrix}, {TileRow}, … to fill in. */
  QString urlTemplate;
};

/**
 * A layer of a WMTS service.
 *
 * @reentrant
 */
struct WmtsLayer {
  /** Identifier, for the LAYER parameter. */
  QString identifier;
  /** Title, for the user. */
  QString title;
  /** Description. */
  QString abstract;
  /** Where it has data, longitude and latitude; nothing if not given. */
  std::optional<GeoRect> wgs84Bounds;
  /** Its styles. */
  std::vector<WmtsStyle> styles;
  /** Image formats it can be requested in. */
  QStringList formats;
  /** Its dimensions. */
  std::vector<WmtsDimension> dimensions;
  /** The tile matrix sets it can be requested in. */
  std::vector<WmtsTileMatrixSetLink> tileMatrixSetLinks;
  /** RESTful templates (tiles only). */
  std::vector<WmtsResourceUrl> resourceUrls;

  /** Returns the default style's identifier (the first if none says). */
  QString defaultStyle() const;

  /** Returns the link to @p tileMatrixSet, or null. */
  const WmtsTileMatrixSetLink* findLink(const QString& tileMatrixSet) const;
};

/**
 * The parts of a WMTS 1.0.0 capabilities document Terra Soar uses.
 *
 * @reentrant
 */
struct WmtsCapabilities {
  /** The service's title. */
  QString title;
  /** The service's description. */
  QString abstract;
  /** Fees, as the server states them. */
  QString fees;
  /** Access constraints, as the server states them. */
  QString accessConstraints;
  /** Who runs the service (ServiceProvider/ProviderName). */
  QString providerName;
  /** The provider's web site, if given. */
  QUrl providerSite;
  /**
   * Where KVP GetTile requests go; empty if the service only offers
   * RESTful access.
   */
  QUrl getTileKvpUrl;
  /** The layers. */
  std::vector<WmtsLayer> layers;
  /** The tile matrix sets. */
  std::vector<WmtsTileMatrixSet> tileMatrixSets;

  /** Returns the layer with @p identifier, or null. */
  const WmtsLayer* findLayer(const QString& identifier) const;

  /** Returns the tile matrix set with @p identifier, or null. */
  const WmtsTileMatrixSet* findTileMatrixSet(const QString& identifier) const;
};

/**
 * Reads a WMTS 1.0.0 capabilities document, from a KVP GetCapabilities
 * request or a RESTful WMTSCapabilities.xml alike. Returns nothing and
 * sets @p errorString (translated) if it isn't one: XML errors, an HTML
 * page, a WMS document, or an OWS ExceptionReport, whose message is
 * passed on.
 *
 * Elements are matched by local name, so namespace prefixes don't matter.
 *
 * @reentrant
 */
std::optional<WmtsCapabilities>
parseWmtsCapabilities(const QByteArray& document, QString* errorString);

} // namespace ts

#endif // TS_WMTS_WMTSCAPABILITIES_H
