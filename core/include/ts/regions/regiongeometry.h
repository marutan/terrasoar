// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The area of an offline region and the quadtree tiles that cover it.
 */
#ifndef TS_REGIONS_REGIONGEOMETRY_H
#define TS_REGIONS_REGIONGEOMETRY_H

#include <optional>
#include <random>
#include <vector>

#include <QJsonObject>
#include <QString>

#include "ts/tiling/tilekey.h"

namespace ts
{

/** A point given by longitude and latitude, degrees. */
struct LonLat {
  /** Longitude, degrees east; may lie outside ±180° (see RegionGeometry). */
  double lonDeg = 0.0;
  /** Latitude, degrees north. */
  double latDeg = 0.0;
};

/**
 * The area of an offline region: a latitude/longitude rectangle or a
 * polygon, either of which may cross the antimeridian.
 *
 * Edges are straight lines in longitude and latitude (as the rectangle's
 * are), not geodesics. Longitudes are kept *unwrapped*: the first vertex
 * lies in [−180°, 180°) and each next one within 180° of the one before,
 * so a region across the antimeridian has longitudes past 180° (a
 * rectangle from 170° E to 170° W runs from 170° to 190°). Polygons that
 * go round a pole can't be unwrapped and are refused; a rectangle may
 * reach a pole.
 *
 * A value type. See docs/design/offline-regions.md.
 *
 * @reentrant
 */
class RegionGeometry
{
public:
  /** The kind of area. */
  enum class Kind {
    Rectangle, ///< A latitude/longitude rectangle.
    Polygon,   ///< A simple polygon (even–odd rule if it crosses itself).
  };

  /** Constructs an empty rectangle (isValid() is false). */
  RegionGeometry() = default;

  /**
   * Returns the rectangle from @p westDeg eastwards to @p eastDeg and from
   * @p southDeg to @p northDeg. When @p eastDeg is west of @p westDeg the
   * rectangle crosses the antimeridian; equal longitudes, or a span of
   * 360° or more, mean the whole way round. Latitudes are clamped to ±90°
   * and swapped if given the wrong way round.
   */
  static RegionGeometry rectangle(double westDeg, double southDeg,
                                  double eastDeg, double northDeg);

  /**
   * Returns the polygon through @p vertices (not repeating the first at
   * the end), or nothing and @p errorString (translated) if it has fewer
   * than three distinct vertices, a latitude beyond ±90°, or goes round a
   * pole.
   */
  static std::optional<RegionGeometry>
  polygon(const std::vector<LonLat>& vertices, QString* errorString);

  /** Returns the kind. */
  Kind kind() const { return m_kind; }

  /** Returns true unless empty (no area). */
  bool isValid() const;

  /**
   * Returns the vertices, unwrapped (see the class description); four
   * for a rectangle, south-west first, anticlockwise.
   */
  const std::vector<LonLat>& vertices() const { return m_vertices; }

  /**
   * Returns the bounding rectangle in unwrapped longitudes: westDeg in
   * [−180°, 180°), eastDeg up to westDeg + 360°.
   */
  const GeoRect& extent() const { return m_extent; }

  /** Returns true if (@p latDeg, @p lonDeg) is inside (any longitude). */
  bool contains(double latDeg, double lonDeg) const;

  /**
   * Returns true if the region and @p rect (longitudes within ±180°, as
   * tile bounds are) share some area; touching along an edge doesn't
   * count.
   */
  bool intersects(const GeoRect& rect) const;

  /** Returns true if @p rect lies entirely inside the region. */
  bool covers(const GeoRect& rect) const;

  /**
   * Returns the area on the WGS 84 ellipsoid in km² (approximately: on
   * the authalic sphere, between straight lines in longitude and
   * latitude).
   */
  double areaKm2() const;

  /**
   * Returns the object stored in region files:
   * `{"kind": "rectangle", "west": …, "south": …, "east": …, "north": …}`
   * or `{"kind": "polygon", "vertices": [[lon, lat], …]}`.
   */
  QJsonObject toJson() const;

  /** Reads toJson()'s object, or returns nothing and @p errorString. */
  static std::optional<RegionGeometry> fromJson(const QJsonObject& json,
                                                QString* errorString);

  /** Equality of kind and vertices. */
  bool operator==(const RegionGeometry& other) const;

private:
  bool intersectsShifted(const GeoRect& rect) const;
  bool coversShifted(const GeoRect& rect) const;
  bool containsUnwrapped(double latDeg, double lonDeg) const;
  bool edgeCrossesInterior(const GeoRect& rect) const;

  Kind m_kind = Kind::Rectangle;
  std::vector<LonLat> m_vertices;
  GeoRect m_extent;
};

/** Tiles of a region per level, as countRegionTiles() returns them. */
struct RegionTileCount {
  /** The first level counted. */
  int minimumLevel = 0;
  /** Tiles at minimumLevel, minimumLevel + 1, … */
  std::vector<long long> perLevel;

  /** Returns the total over all levels. */
  long long total() const;
};

/**
 * Returns how many tiles at levels @p minimumLevel to @p maximumLevel
 * share some area with @p region inside @p dataBounds, if given (where
 * the source has data; tiles outside aren't requested). Exact, and fast even
 * for millions of tiles: tiles wholly inside are counted without visiting their
 * descendants.
 *
 * @reentrant
 */
RegionTileCount countRegionTiles(const RegionGeometry& region, int minimumLevel,
                                 int maximumLevel,
                                 const std::optional<GeoRect>& dataBounds);

/**
 * Hands out the tiles of a region one by one, level by level from the
 * coarsest (so a download stopped part way has the whole region at the
 * coarser levels), each level in quadtree order. The same tiles
 * countRegionTiles() counts.
 *
 * @reentrant
 */
class RegionTileIterator
{
public:
  /** Iterates the tiles of @p region, see countRegionTiles(). */
  RegionTileIterator(RegionGeometry region, int minimumLevel, int maximumLevel,
                     std::optional<GeoRect> dataBounds);

  /** Returns the next tile, or nothing at the end. */
  std::optional<TileKey> next();

private:
  bool wanted(TileKey key) const;
  void startLevel();

  RegionGeometry m_region;
  int m_maximumLevel;
  std::optional<GeoRect> m_dataBounds;
  int m_level;
  std::vector<TileKey> m_stack;
};

/**
 * Returns up to @p count different tiles of @p region, picked at random
 * (from @p random) with each level weighted by its share of @p counts:
 * the tiles to fetch for a size estimate. Tiles crossing the region's
 * edge are picked less often than inner ones.
 *
 * @reentrant
 */
std::vector<TileKey> sampleRegionTiles(const RegionGeometry& region,
                                       const RegionTileCount& counts,
                                       const std::optional<GeoRect>& dataBounds,
                                       int count, std::mt19937_64& random);

} // namespace ts

#endif // TS_REGIONS_REGIONGEOMETRY_H
