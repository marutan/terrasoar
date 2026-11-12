// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A WMS layer as a tile source: what to request and how.
 */
#ifndef TS_WMS_WMSSOURCE_H
#define TS_WMS_WMSSOURCE_H

#include <optional>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

struct WmsCapabilities;
struct WmsLayer;

/**
 * Everything needed to request tiles of one or more WMS layers: one
 * GetMap per quadtree tile. Stored in layer definitions as the source
 * object `{"kind": "wms", …}` (toJson()), so a project needs no
 * capabilities request to draw.
 *
 * @reentrant
 */
struct WmsSource {
  /** Where GetMap requests go (from the capabilities). */
  QUrl url;
  /** "1.1.1" or "1.3.0". */
  QString version = QStringLiteral("1.3.0");
  /** The layers, bottom first, as for LAYERS. */
  QStringList layers;
  /** A style per layer (empty for the default), as for STYLES. */
  QStringList styles;
  /** Image MIME type, as for FORMAT. */
  QString format = QStringLiteral("image/png");
  /** True to ask for a transparent background (overlays). */
  bool transparent = true;
  /**
   * The CRS to request in, as the server writes it: geographic
   * (EPSG:4326, CRS:84) when offered, else Web Mercator (EPSG:3857 and
   * its older codes), reprojected when drawn.
   */
  QString crs = QStringLiteral("EPSG:4326");
  /** Image size per tile, pixels. */
  int tileSizePx = 256;
  /** Deepest quadtree level to request. */
  int maximumLevel = 18;
  /** Where the layers have data; tiles outside aren't requested. */
  std::optional<GeoRect> bounds;
  /** Name for the user (the layer's title). */
  QString title;
  /** Who to credit, shown while the layer is on screen. */
  QString attribution;

  /**
   * Returns a string identifying the tiles: two sources with the same id
   * request the same images.
   */
  QString id() const;

  /** Returns how the images map to tiles (from the CRS). */
  TileImageProjection imageProjection() const;

  /** Returns the source object for a layer definition. */
  QJsonObject toJson() const;

  /**
   * Reads a source object. Returns nothing and sets @p errorString
   * (translated) if something needed is missing or makes no sense.
   */
  static std::optional<WmsSource> fromJson(const QJsonObject& json,
                                           QString* errorString);
};

/** Returns true for CRS:84 and EPSG:4326 (any case). */
bool isGeographicCrs(const QString& crs);

/**
 * Returns true for Web Mercator: EPSG:3857 and the codes servers used
 * before it existed (EPSG:900913, EPSG:3785, EPSG:102100, EPSG:102113).
 */
bool isWebMercatorCrs(const QString& crs);

/**
 * Returns the CRS to request a layer offered in @p offered in, for WMS
 * @p version: geographic if possible (CRS:84 first in 1.3.0, whose
 * EPSG:4326 has latitude first), else Web Mercator; empty if neither.
 * The code is returned in its usual upper-case spelling, whatever the
 * server wrote.
 */
QString chooseCrs(const QStringList& offered, const QString& version);

/**
 * Returns the image format to ask for among @p offered: PNG for
 * overlays (it can be transparent), JPEG for an @p opaque layer (smaller),
 * falling back to the other and then to GIF; empty if none of them.
 */
QString chooseFormat(const QStringList& offered, bool opaque);

/** Returns true if images of @p format can be transparent (PNG, GIF). */
bool isTransparentFormat(const QString& format);

/**
 * Returns the BBOX parameter for @p rect requested in @p crs with WMS
 * @p version, in the axis order the version and CRS require:
 *
 * - 1.3.0 with EPSG:4326: minLat,minLon,maxLat,maxLon (the EPSG order).
 * - 1.3.0 with CRS:84, and 1.1.1 with EPSG:4326: minLon,minLat,maxLon,maxLat.
 * - Web Mercator, either version: minX,minY,maxX,maxY in metres, @p rect
 *   clipped to the Mercator limit first.
 *
 * Numbers are written in the shortest form that reads back exactly.
 * Empty if @p crs is neither geographic nor Web Mercator.
 */
QString bboxParameter(const QString& version, const QString& crs,
                      const GeoRect& rect);

/**
 * Returns @p rect clipped to the latitudes Web Mercator covers
 * (±85.051 129°), or nothing if nothing is left.
 */
std::optional<GeoRect> webMercatorCoverage(const GeoRect& rect);

/**
 * Returns the GetMap URL for tile @p key of @p source, or an empty URL
 * if the source has nothing there (outside its bounds or, for Web
 * Mercator, beyond the poles' limit). Query items already in the
 * source's URL (such as MapServer's map=) are kept, except WMS
 * parameters, which are replaced.
 */
QUrl getMapUrl(const WmsSource& source, TileKey key);

/**
 * Returns the GetCapabilities URL for the service at @p serviceUrl (any
 * query items besides WMS parameters kept), asking for @p version; the
 * server answers in that version or the nearest it has.
 */
QUrl capabilitiesUrl(const QUrl& serviceUrl,
                     const QString& version = QStringLiteral("1.3.0"));

/**
 * Returns a source drawing @p layerNames (bottom first) of
 * @p capabilities with @p styles (empty: defaults) in @p format (empty:
 * chooseFormat()). Returns nothing and sets @p errorString (translated)
 * if a layer doesn't exist, can't be drawn in a usable CRS, or can't be
 * drawn in tiles (fixed size, no subsets), or if there are more layers
 * than the server allows in one request.
 */
std::optional<WmsSource> wmsSourceFor(const WmsCapabilities& capabilities,
                                      const QStringList& layerNames,
                                      const QStringList& styles,
                                      const QString& format,
                                      QString* errorString);

} // namespace ts

#endif // TS_WMS_WMSSOURCE_H
