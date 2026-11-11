// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/tiling/tilemesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "ts/geo/geodetic.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

int clampSegments(int segments)
{
  return std::clamp(segments, 2, 64);
}

/** Index of grid vertex (column, row). */
int gridIndex(int segments, int column, int row)
{
  return row * (segments + 1) + column;
}

/**
 * The border vertices in order around the tile (north edge west → east,
 * east edge north → south, south edge east → west, west edge south →
 * north), as (column, row) pairs; 4 × segments of them.
 */
std::vector<std::array<int, 2>> borderCells(int segments)
{
  std::vector<std::array<int, 2>> cells;
  const int count = 4 * segments;
  cells.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < segments; ++i) {
    cells.push_back({i, 0});
  }
  for (int i = 0; i < segments; ++i) {
    cells.push_back({segments, i});
  }
  for (int i = segments; i > 0; --i) {
    cells.push_back({i, segments});
  }
  for (int i = segments; i > 0; --i) {
    cells.push_back({0, i});
  }
  return cells;
}

void pushTriangle(std::vector<std::uint16_t>& indices, int a, int b, int c)
{
  indices.push_back(static_cast<std::uint16_t>(a));
  indices.push_back(static_cast<std::uint16_t>(b));
  indices.push_back(static_cast<std::uint16_t>(c));
}

std::array<float, 3> toFloat3(const Vec3& v)
{
  return {static_cast<float>(v.x), static_cast<float>(v.y),
          static_cast<float>(v.z)};
}

} // namespace

double tileSkirtDepthM(int level, int segments)
{
  const double segmentRad = degToRad(TileKey::sizeDeg(level)) /
                            static_cast<double>(clampSegments(segments));
  const double sagM =
      wgs84::semiMajorAxisM * (1.0 - std::cos(0.5 * segmentRad));
  return 2.0 * sagM + 1.0;
}

TileMesh buildTileMesh(TileKey key, int segments)
{
  const int n = clampSegments(segments);
  const GeoRect rect = key.bounds();
  TileMesh mesh;
  mesh.key = key;
  mesh.segments = n;
  mesh.centreEcef = key.centreEcef();
  const int vertexCount = (n + 1) * (n + 1) + 4 * n;
  mesh.vertices.reserve(static_cast<std::size_t>(vertexCount));

  for (int row = 0; row <= n; ++row) {
    const double v = static_cast<double>(row) / n;
    const double lat = rect.northDeg - v * rect.heightDeg();
    for (int column = 0; column <= n; ++column) {
      const double u = static_cast<double>(column) / n;
      const double lon = rect.westDeg + u * rect.widthDeg();
      const Vec3 position = geodeticToEcef(Geodetic{lat, lon, 0.0});
      const Vec3 offset = position - mesh.centreEcef;
      mesh.radiusM = std::max(mesh.radiusM, length(offset));
      mesh.vertices.push_back(TileVertex{
          toFloat3(offset), {static_cast<float>(u), static_cast<float>(v)}});
    }
  }

  const double depthM = tileSkirtDepthM(key.level, n);
  for (const std::array<int, 2>& cell : borderCells(n)) {
    const TileVertex& top =
        mesh.vertices[static_cast<std::size_t>(gridIndex(n, cell[0], cell[1]))];
    const double lat = rect.northDeg - rect.heightDeg() * cell[1] / n;
    const double lon = rect.westDeg + rect.widthDeg() * cell[0] / n;
    const Vec3 down = -surfaceNormalEcef(lat, lon) * depthM;
    const Vec3 offset = Vec3{static_cast<double>(top.offsetM[0]),
                             static_cast<double>(top.offsetM[1]),
                             static_cast<double>(top.offsetM[2])} +
                        down;
    mesh.radiusM = std::max(mesh.radiusM, length(offset));
    mesh.vertices.push_back(TileVertex{toFloat3(offset), top.uv});
  }
  return mesh;
}

std::vector<std::uint16_t> tileMeshStripIndices(int segments)
{
  const int n = clampSegments(segments);
  std::vector<std::uint16_t> indices;
  indices.reserve(static_cast<std::size_t>(6 * n));
  for (int row = 0; row < n; ++row) {
    const int nw = gridIndex(n, 0, row);
    const int ne = gridIndex(n, n, row);
    const int sw = gridIndex(n, 0, row + 1);
    const int se = gridIndex(n, n, row + 1);
    pushTriangle(indices, nw, sw, ne);
    pushTriangle(indices, ne, sw, se);
  }
  return indices;
}

std::vector<std::uint16_t> tileMeshIndices(int segments)
{
  const int n = clampSegments(segments);
  std::vector<std::uint16_t> indices;
  const int indexCount = 6 * n * n + 6 * 4 * n;
  indices.reserve(static_cast<std::size_t>(indexCount));
  // Rows run north to south and columns west to east, so seen from
  // outside (north up, east right) (row, column) → (row, column + 1) →
  // (row + 1, column) is clockwise; list the triangles the other way.
  for (int row = 0; row < n; ++row) {
    for (int column = 0; column < n; ++column) {
      const int nw = gridIndex(n, column, row);
      const int ne = gridIndex(n, column + 1, row);
      const int sw = gridIndex(n, column, row + 1);
      const int se = gridIndex(n, column + 1, row + 1);
      pushTriangle(indices, nw, sw, ne);
      pushTriangle(indices, ne, sw, se);
    }
  }
  const std::vector<std::array<int, 2>> border = borderCells(n);
  const int skirtStart = (n + 1) * (n + 1);
  const int count = static_cast<int>(border.size());
  for (int i = 0; i < count; ++i) {
    const int next = (i + 1) % count;
    const int a = gridIndex(n, border[static_cast<std::size_t>(i)][0],
                            border[static_cast<std::size_t>(i)][1]);
    const int b = gridIndex(n, border[static_cast<std::size_t>(next)][0],
                            border[static_cast<std::size_t>(next)][1]);
    const int a2 = skirtStart + i;
    const int b2 = skirtStart + next;
    // The border runs clockwise seen from above; seen from outside the
    // tile, looking at its side, a is on the right and b on the left, so
    // (a, b, a2) and (b, b2, a2) are counter-clockwise: the skirt faces out.
    pushTriangle(indices, a, b, a2);
    pushTriangle(indices, b, b2, a2);
  }
  return indices;
}

} // namespace ts
