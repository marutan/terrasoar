// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Keys and bounds of the EPSG:4326 tile quadtree.
 */
#ifndef TS_TILING_TILEKEY_H
#define TS_TILING_TILEKEY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <QString>

#include "ts/geo/geodetic.h"
#include "ts/maths/vec3.h"

namespace ts
{

/**
 * A latitude/longitude rectangle in degrees, west to east and south to
 * north. Tile rectangles never cross the antimeridian.
 *
 * @reentrant
 */
struct GeoRect {
  /** Western edge, degrees. */
  double westDeg = 0.0;
  /** Southern edge, degrees. */
  double southDeg = 0.0;
  /** Eastern edge, degrees. */
  double eastDeg = 0.0;
  /** Northern edge, degrees. */
  double northDeg = 0.0;

  /** Returns the width in degrees of longitude. */
  constexpr double widthDeg() const { return eastDeg - westDeg; }
  /** Returns the height in degrees of latitude. */
  constexpr double heightDeg() const { return northDeg - southDeg; }
  /** Returns the centre latitude. */
  constexpr double centreLatDeg() const { return 0.5 * (southDeg + northDeg); }
  /** Returns the centre longitude. */
  constexpr double centreLonDeg() const { return 0.5 * (westDeg + eastDeg); }

  /** Returns true if (@p latDeg, @p lonDeg) is inside or on the edge. */
  constexpr bool contains(double latDeg, double lonDeg) const
  {
    return latDeg >= southDeg && latDeg <= northDeg && lonDeg >= westDeg &&
           lonDeg <= eastDeg;
  }
};

/**
 * One tile of the geographic (EPSG:4326) quadtree.
 *
 * Level 0 has two tiles of 180° × 180°: x = 0 covers the western
 * hemisphere (−180° to 0°), x = 1 the eastern. Each level halves the tile
 * size, so level n has 2^(n+1) columns and 2^n rows. Rows count from the
 * north (y = 0 touches the north pole), columns from the antimeridian
 * eastwards. Tiles in the top and bottom rows touch a pole.
 *
 * A value type: compare, hash and sort freely.
 *
 * @reentrant
 */
struct TileKey {
  /** Deepest level supported (tiles of about 1.2 m at the equator). */
  static constexpr int maximumLevel = 24;

  /** Zoom level, 0 to maximumLevel. */
  int level = 0;
  /** Column, 0 to 2^(level+1) − 1, from 180° W eastwards. */
  int x = 0;
  /** Row, 0 to 2^level − 1, from the north pole southwards. */
  int y = 0;

  /** Returns the number of columns at @p level. */
  static constexpr std::int64_t columns(int level)
  {
    return std::int64_t{2} << level;
  }

  /** Returns the number of rows at @p level. */
  static constexpr std::int64_t rows(int level)
  {
    return std::int64_t{1} << level;
  }

  /** Returns the tile size in degrees at @p level (both directions). */
  static constexpr double sizeDeg(int level)
  {
    return 180.0 / static_cast<double>(rows(level));
  }

  /** Returns the two level-0 tiles. */
  static constexpr std::array<TileKey, 2> roots()
  {
    return {TileKey{0, 0, 0}, TileKey{0, 1, 0}};
  }

  /**
   * Returns the tile at @p level containing @p position (its height is
   * ignored).
   * Longitude is normalised; points on an edge belong to the tile east
   * or south of it (except at 180° E and the south pole).
   */
  static TileKey containing(int level, const Geodetic& position);

  /** Returns true if level, x and y are in range. */
  constexpr bool isValid() const
  {
    return level >= 0 && level <= maximumLevel && x >= 0 && y >= 0 &&
           x < columns(level) && y < rows(level);
  }

  /** Returns the tile's rectangle. */
  GeoRect bounds() const;

  /** Returns the parent (one level up); a root returns itself. */
  TileKey parent() const;

  /**
   * Returns child @p index (0 north-west, 1 north-east, 2 south-west,
   * 3 south-east), one level down.
   */
  TileKey child(int index) const;

  /** Returns true if @p other is this tile or lies inside it. */
  bool contains(TileKey other) const;

  /** Returns true if the tile touches the north or south pole. */
  constexpr bool touchesPole() const { return y == 0 || y == rows(level) - 1; }

  /** Returns the ECEF centre of the tile's rectangle on the ellipsoid. */
  Vec3 centreEcef() const;

  /** Returns "level/x/y". */
  QString toString() const;

  /** Returns a unique 64-bit packing of the key (for hashing). */
  constexpr std::uint64_t packed() const
  {
    return (static_cast<std::uint64_t>(level) << 58U) |
           (static_cast<std::uint64_t>(x) << 29U) |
           static_cast<std::uint64_t>(y);
  }

  /** Equality. */
  constexpr bool operator==(TileKey other) const
  {
    return level == other.level && x == other.x && y == other.y;
  }
  /** Inequality. */
  constexpr bool operator!=(TileKey other) const { return !(*this == other); }
  /** Ordering by level, then row, then column. */
  constexpr bool operator<(TileKey other) const
  {
    if (level != other.level) {
      return level < other.level;
    }
    return y != other.y ? y < other.y : x < other.x;
  }
};

/** Hash for unordered containers. */
struct TileKeyHash {
  /** Returns the hash of @p key. */
  std::size_t operator()(TileKey key) const
  {
    return std::hash<std::uint64_t>()(key.packed());
  }
};

} // namespace ts

#endif // TS_TILING_TILEKEY_H
