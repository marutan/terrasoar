// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Builds the base pack's shoreline GeoPackage from GSHHG shapefiles.
 */
#ifndef TS_PACKAGER_SHORELINEBUILDER_H
#define TS_PACKAGER_SHORELINEBUILDER_H

#include <functional>
#include <vector>

#include <QString>

namespace ts
{
class GeoPackageFeatureWriter;
} // namespace ts

namespace ts::packager
{

/**
 * One level of detail of the shoreline: a GSHHG resolution cut to the
 * quadtree tiles of one level, shown over a range of display levels.
 */
struct ShorelineLod {
  /** GSHHG resolution letter: c, l, i, h or f. */
  QString resolution;
  /** Quadtree level the polygons and lines are cut to. */
  int storageLevel = 0;
  /** Shallowest display level served. */
  int minLevel = 0;
  /** Deepest display level served, or -1 for every deeper level. */
  int maxLevel = -1;
};

/** What to build. */
struct ShorelineOptions {
  /**
   * The GSHHS_shp directory (with c/, l/, i/, h/, f/ inside), a plain
   * path or a GDAL virtual path such as /vsizip/<zip>/GSHHS_shp.
   */
  QString shapefileDirectory;
  /** The levels of detail, coarse to fine. */
  std::vector<ShorelineLod> lods;
  /**
   * Polygons smaller than this are dropped (km², from GSHHG's own area
   * field); 0 keeps everything.
   */
  double minimumAreaKm2 = 0.0;
  /** Called with progress messages (English); may be empty. */
  std::function<void(const QString&)> progress;
};

/** Results for one level of detail. */
struct ShorelineLodStats {
  /** The GSHHG resolution. */
  QString resolution;
  /** Source polygons read, after the minimum area. */
  int sourcePolygons = 0;
  /** Polygons GEOS found invalid and repaired (source and cut). */
  int repairedPolygons = 0;
  /** Tiles with land written. */
  int landTiles = 0;
  /** Vertices written. */
  long long vertices = 0;
  /** Land area written, km² on the WGS 84 ellipsoid (rhumb-line edges). */
  double landAreaKm2 = 0.0;
  /**
   * The same total from the source polygons as read: levels 1 and 5 minus
   * 2 plus 3 minus 4, km². The pack's check compares landAreaKm2 with it:
   * cutting, merging and repairing must neither lose nor add land.
   */
  double sourceAreaKm2 = 0.0;
  /**
   * The same total by GSHHG's own area fields, for information: at coarse
   * resolutions some fields describe the full-resolution polygon.
   */
  double gshhgAreaKm2 = 0.0;
};

/**
 * Reads the GSHHG shapefiles of each level of detail, builds land
 * polygons (land minus lakes plus islands in lakes minus ponds, Antarctica
 * by its ice front), cuts them to quadtree tiles and writes them to
 * @p writer as tables `land_<resolution>`, one multipolygon per tile.
 *
 * There are no separate shoreline lines: the shore is the polygons'
 * boundary without the segments lying on their tile's edges, which are
 * all the artificial edges (tile cuts, the antimeridian, the south pole).
 *
 * Returns false and sets @p errorString on failure.
 */
bool buildShoreline(const ShorelineOptions& options,
                    GeoPackageFeatureWriter* writer,
                    std::vector<ShorelineLodStats>* stats,
                    QString* errorString);

/**
 * Returns the area of the ring @p lonLatDeg (longitude, latitude pairs,
 * degrees; closed or not) on the WGS 84 ellipsoid with rhumb-line edges,
 * km², always positive.
 */
double rhumbRingAreaKm2(const std::vector<double>& lonLatDeg);

} // namespace ts::packager

#endif // TS_PACKAGER_SHORELINEBUILDER_H
