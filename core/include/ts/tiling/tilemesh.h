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

/** One vertex of a tile mesh, as uploaded to the GPU (20 bytes). */
struct TileVertex {
  /** Position relative to the tile centre, ECEF metres. */
  std::array<float, 3> offsetM{};
  /** Texture coordinate: u west → east, v north → south, both in [0, 1]. */
  std::array<float, 2> uv{};
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

/**
 * Builds the mesh of @p key with @p segments grid segments per side
 * (2 to 64).
 *
 * @threadsafe
 */
TileMesh buildTileMesh(TileKey key, int segments = TileMesh::defaultSegments);

/**
 * Returns the 16-bit triangle indices shared by every mesh with
 * @p segments segments: the grid, then the skirt. Triangles are
 * counter-clockwise seen from outside the globe.
 *
 * @threadsafe
 */
std::vector<std::uint16_t> tileMeshIndices(int segments);

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
