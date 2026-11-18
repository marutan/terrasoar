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
 * A WMTS layer as a tile source: its tile matrices, and which of their
 * tiles make up each quadtree tile.
 */
#ifndef TS_WMTS_WMTSSOURCE_H
#define TS_WMTS_WMTSSOURCE_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

struct WmtsCapabilities;
struct WmtsTileMatrix;

/**
 * Size of a degree at the equator in metres, as WMTS defines it to turn
 * scale denominators into degrees (WMTS 1.0.0 section 6.1.1, Annex E):
 * 2π × 6 378 137 / 360.
 */
constexpr double wmtsMetresPerDegree = 111319.49079327357;

/** Size of a standardized rendering pixel, metres (WMTS 1.0.0 6.1.1). */
constexpr double wmtsPixelSizeM = 0.00028;

/**
 * One tile matrix of a source, ready for arithmetic: the corner with
 * easting (or longitude) first whatever the CRS's axis order, and the
 * size of a pixel in the CRS's units.
 *
 * @reentrant
 */
struct WmtsMatrix {
  /** Identifier, for TILEMATRIX. */
  QString identifier;
  /** Size of a pixel in CRS units (metres or degrees). */
  double pixelSize = 0.0;
  /** Easting (or longitude) of the top-left corner. */
  double originX = 0.0;
  /** Northing (or latitude) of the top-left corner. */
  double originY = 0.0;
  /** Tile width, pixels. */
  int32_t tileWidth = 256;
  /** Tile height, pixels. */
  int32_t tileHeight = 256;
  /** Number of tile columns. */
  int64_t matrixWidth = 1;
  /** Number of tile rows. */
  int64_t matrixHeight = 1;
  /** First row with tiles (the layer's limits, or 0). */
  int64_t minRow = 0;
  /** Last row with tiles (the layer's limits, or matrixHeight − 1). */
  int64_t maxRow = 0;
  /** First column with tiles (the layer's limits, or 0). */
  int64_t minColumn = 0;
  /** Last column with tiles (the layer's limits, or matrixWidth − 1). */
  int64_t maxColumn = 0;
};

/** A block of a matrix's tiles and where an extent lies in it. */
struct WmtsTileWindow {
  /** The matrix's index in WmtsSource::matrices. */
  int32_t matrix = 0;
  /** First column of the block. */
  int64_t firstColumn = 0;
  /** First row of the block. */
  int64_t firstRow = 0;
  /** Number of columns in the block. */
  int32_t columns = 0;
  /** Number of rows in the block. */
  int32_t rows = 0;
  /** Left edge of the extent, pixels from the block's left edge. */
  double left = 0.0;
  /** Top edge of the extent, pixels from the block's top edge. */
  double top = 0.0;
  /** Width of the extent, pixels. */
  double width = 0.0;
  /** Height of the extent, pixels. */
  double height = 0.0;
};

/** One tile of a source: its matrix (an index into the source's), column
 * and row. */
struct WmtsTileIndex {
  /** Index into WmtsSource::matrices. */
  int32_t matrix = 0;
  /** Column. */
  int64_t column = 0;
  /** Row. */
  int64_t row = 0;
};

/** What the user chose for a WMTS layer; empty values take defaults. */
struct WmtsChoices {
  /** The style's identifier (empty: the layer's default). */
  QString style;
  /** Image MIME type (empty: chosen as for WMS, PNG first). */
  QString format;
  /** The tile matrix set (empty: the first of usableTileMatrixSets()). */
  QString tileMatrixSet;
  /** Dimension values; unset ones take the layer's defaults. */
  std::vector<std::pair<QString, QString>> dimensions;
};

/**
 * Everything needed to request tiles of one WMTS layer and fit them to
 * the quadtree. Stored in layer definitions as the source object
 * `{"kind": "wmts", …}` (toJson()), matrices included, so a project
 * needs no capabilities request to draw.
 *
 * Tile matrix sets in EPSG:4326 (or CRS:84) are drawn as geographic
 * images, those in Web Mercator as Web Mercator images (rows evenly
 * spaced in northing, see TileImageProjection), so fitting their tiles to
 * a quadtree tile is a scale and an offset either way.
 *
 * @reentrant
 */
struct WmtsSource {
  /** Where KVP GetTile requests go; empty when urlTemplate is used. */
  QUrl url;
  /** RESTful template ({TileMatrix}, {TileRow}, …); empty for KVP. */
  QString urlTemplate;
  /** The layer's identifier. */
  QString layer;
  /** The style's identifier. */
  QString style;
  /** Image MIME type. */
  QString format = QStringLiteral("image/png");
  /** The tile matrix set's identifier. */
  QString tileMatrixSet;
  /** The set's CRS, normalised (see normalisedWmtsCrs()). */
  QString crs;
  /** The matrices, coarsest first; only those the layer has tiles in. */
  std::vector<WmtsMatrix> matrices;
  /** Dimension values (identifier, value), in the capabilities' order. */
  std::vector<std::pair<QString, QString>> dimensions;
  /** Where the layer has data; tiles outside aren't requested. */
  std::optional<GeoRect> bounds;
  /** Size of the images made for quadtree tiles, pixels. */
  int32_t tileSizePx = 256;
  /** Deepest quadtree level that gains detail (from the finest matrix). */
  int32_t maximumLevel = 18;
  /** Name for the user (the layer's title). */
  QString title;
  /** Who to credit, shown while the layer is on screen. */
  QString attribution;

  /**
   * Returns a string identifying the tiles: two sources with the same id
   * request the same images. Includes every dimension value.
   */
  QString id() const;

  /** Returns how the images made for quadtree tiles map to them. */
  TileImageProjection imageProjection() const;

  /** Returns the source object for a layer definition. */
  QJsonObject toJson() const;

  /**
   * Reads a source object. Returns nothing and sets @p errorString
   * (translated) if something needed is missing or makes no sense.
   */
  static std::optional<WmtsSource> fromJson(const QJsonObject& json,
                                            QString* errorString);
};

/**
 * Returns @p supportedCrs (a URN, an OGC URL or a code) as "EPSG:n" or
 * "CRS:84", or empty if it isn't one of those forms. Every way of writing
 * WGS 84 longitude-latitude (urn:ogc:def:crs:OGC:1.3:CRS84, …) becomes
 * "CRS:84"; Web Mercator's other codes (EPSG:900913, EPSG:102100, …)
 * become "EPSG:3857".
 */
QString normalisedWmtsCrs(const QString& supportedCrs);

/** Returns true if WMTS sources in @p crs (normalised) can be drawn. */
bool isSupportedWmtsCrs(const QString& crs);

/**
 * Returns the size of a pixel of @p matrix in units of @p crs
 * (normalised): degrees for geographic CRSs, metres otherwise.
 */
double wmtsPixelSize(const WmtsTileMatrix& matrix, const QString& crs);

/**
 * Returns @p matrix ready for arithmetic in @p crs (normalised): the
 * pixel size, and the corner with easting or longitude first. For
 * EPSG:4326, whose axis order is latitude first, a corner written
 * longitude first anyway (a common mistake) is recognised, as the first
 * value can't be a latitude.
 */
WmtsMatrix wmtsMatrix(const WmtsTileMatrix& matrix, const QString& crs);

/**
 * Returns the extent of quadtree tile @p key in @p source's CRS units:
 * {west, south, east, north}. For Web Mercator, the tile's rectangle
 * clipped to the projection's limit; nothing if nothing is left.
 */
std::optional<GeoRect> wmtsExtent(const WmtsSource& source, TileKey key);

/**
 * Returns the index of the matrix to draw quadtree tile @p key from: the
 * coarsest whose pixels are at least as fine as the tile's (the finest if
 * none is), so nothing is upsampled that needn't be.
 */
int32_t wmtsMatrixFor(const WmtsSource& source, TileKey key);

/**
 * Returns the block of tiles of matrix @p matrix covering quadtree tile
 * @p key, and where the tile lies in it. Some tiles of the block may not
 * exist (beyond the matrix's edge or the layer's limits, see
 * wmtsTileExists()); they are left transparent. Nothing if none of them
 * exist, the tile is outside the source's bounds, or it would take more
 * than 16 tiles (a layer without matrices as coarse as the tile).
 */
std::optional<WmtsTileWindow> wmtsTileWindow(const WmtsSource& source,
                                             TileKey key, int32_t matrix);

/**
 * Returns true if @p tile exists: inside its matrix and the layer's
 * limits.
 */
bool wmtsTileExists(const WmtsSource& source, const WmtsTileIndex& tile);

/**
 * Returns the address of @p tile: the RESTful template filled in, or a KVP
 * GetTile request. Template variables are matched without regard to case;
 * dimension values fill their variables or become parameters.
 */
QUrl getTileUrl(const WmtsSource& source, const WmtsTileIndex& tile);

/**
 * Returns the address of the capabilities of the service at
 * @p serviceUrl: unchanged if it already names a capabilities document
 * (ends in .xml, or asks for GetCapabilities), else a KVP
 * GetCapabilities request (other query items kept).
 */
QUrl wmtsCapabilitiesUrl(const QUrl& serviceUrl);

/**
 * Returns the tile matrix sets of layer @p layerId that can be drawn,
 * preferred first: geographic, then Web Mercator; within each, the one
 * reaching finest.
 */
QStringList usableTileMatrixSets(const WmtsCapabilities& capabilities,
                                 const QString& layerId);

/**
 * Returns a source drawing layer @p layerId of @p capabilities as
 * @p choices say: the style, or the layer's default; the format, or as for
 * WMS (PNG first, else the first offered); the tile matrix set, or the
 * first of usableTileMatrixSets(); dimension values, unset ones taking
 * their defaults. RESTful access is used when the layer has a template for
 * the format (the one with every dimension's variable, if several), else
 * KVP. Returns nothing and sets @p errorString (translated) if the layer
 * doesn't exist, has no usable tile matrix set, or can't be requested in
 * that format or style.
 */
std::optional<WmtsSource> wmtsSourceFor(const WmtsCapabilities& capabilities,
                                        const QString& layerId,
                                        const WmtsChoices& choices,
                                        QString* errorString);

} // namespace ts

#endif // TS_WMTS_WMTSSOURCE_H
