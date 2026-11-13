// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Triangle meshes for quadtree tiles.
 */
#ifndef TS_TILING_TILEMESH_H
#define TS_TILING_TILEMESH_H

#include <array>
#include <cstdint>
#include <vector>

#include "ts/maths/vec3.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

/** One vertex of a tile mesh, as uploaded to the GPU (40 bytes). */
struct TileVertex {
  /** Position on the ellipsoid relative to the tile centre, ECEF metres. */
  std::array<float, 3> offsetM{};
  /** Texture coordinate: u west → east, v north → south, both in [0, 1]. */
  std::array<float, 2> uv{};
  /**
   * Terrain, metres: the height above the ellipsoid, the sea level there
   * (the geoid's height above the ellipsoid), and how far below its border
   * vertex a skirt vertex hangs, in terrain metres (0 on the grid). The
   * vertex shader raises the vertex along the ellipsoid's normal by
   * sea + max(height − sea, 0) × exaggeration − drop × exaggeration, when
   * terrain is on. All 0 without heights.
   */
  std::array<float, 3> terrain{};
  /**
   * The terrain's slope above sea level (the sea floor flattened), metres
   * per metre towards east and towards north, for shading.
   */
  std::array<float, 2> slope{};
};

/**
 * The surface of one tile as a regular grid in latitude and longitude,
 * with a skirt.
 *
 * Positions are float offsets from the tile's ECEF centre, so they keep
 * their precision however large the coordinates (relative-to-centre; the
 * centre goes to the GPU as a high/low pair). Tiles touching a pole
 * collapse their polar row to the pole point, so the quadtree covers the
 * polar caps without special geometry.
 *
 * The skirt is a strip hanging down from the tile's border, deep enough to
 * cover the gap where a neighbour at another level meets this tile with a
 * different chord. Its vertices repeat the border's texture coordinates.
 *
 * Every tile with the same number of segments shares one index buffer
 * (tileMeshIndices()).
 *
 * @reentrant
 */
struct TileMesh {
  /** Default grid segments per side. */
  static constexpr int defaultSegments = 16;

  /** The tile this mesh is for. */
  TileKey key;
  /** ECEF centre the vertex offsets are relative to. */
  Vec3 centreEcef{};
  /** Distance from the centre to the farthest vertex, metres. */
  double radiusM = 0.0;
  /** Grid segments per side. */
  int segments = defaultSegments;
  /** (segments + 1)² grid vertices row by row from the north-west, then
   *  4 × segments skirt vertices. */
  std::vector<TileVertex> vertices;
};

struct HeightTile;

/**
 * Builds the mesh of @p key with @p segments grid segments per side
 * (2 to 64), with the terrain of @p heights (for @p key or an ancestor,
 * interpolated; heights above the ellipsoid with their sea level) if
 * given. The skirt then also hangs as far as the tile's terrain varies,
 * which covers the gap to a neighbour whose terrain is sampled more or
 * less finely. See docs/design/terrain.md.
 *
 * @threadsafe
 */
TileMesh buildTileMesh(TileKey key, int segments = TileMesh::defaultSegments,
                       const HeightTile* heights = nullptr);

/**
 * Returns the 16-bit triangle indices shared by every mesh with
 * @p segments segments: the grid, then the skirt. Triangles are
 * counter-clockwise seen from outside the globe.
 *
 * @threadsafe
 */
std::vector<std::uint16_t> tileMeshIndices(int segments);

/**
 * Returns triangle indices that draw a mesh with @p segments segments as
 * one column of quads, a quad per grid row from the west edge to the east
 * edge, without the skirt: 2 × segments triangles in place of
 * tileMeshIndices()'s 2 × segments² and more. On the flat map of a
 * cylindrical projection that is the same picture (see
 * Projection::isCylindrical()): the rows keep the projection's curve in
 * latitude and nothing varies along them.
 *
 * @threadsafe
 */
std::vector<std::uint16_t> tileMeshStripIndices(int segments);

/**
 * Returns the skirt depth in metres for a tile at @p level with
 * @p segments segments: twice the largest gap between the chord of one
 * grid segment and the ellipsoid, plus a metre.
 *
 * @threadsafe
 */
double tileSkirtDepthM(int level, int segments);

} // namespace ts

#endif // TS_TILING_TILEMESH_H
