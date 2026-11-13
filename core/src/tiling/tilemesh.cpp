// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/tiling/tilemesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "ts/elevation/heightgrid.h"
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

namespace
{

/**
 * The terrain at the grid vertices of @p key from @p heights: height
 * above the ellipsoid, sea level, and the height above sea level with the
 * sea floor flattened (what is drawn), row by row.
 */
struct GridTerrain {
  std::vector<float> heightM;
  std::vector<float> seaLevelM;
  std::vector<double> landM;
};

GridTerrain sampleTerrain(TileKey key, int n, const HeightTile& heights)
{
  const GeoRect rect = key.bounds();
  GridTerrain terrain;
  const std::size_t side = static_cast<std::size_t>(n) + 1;
  const std::size_t count = side * side;
  terrain.heightM.reserve(count);
  terrain.seaLevelM.reserve(count);
  terrain.landM.reserve(count);
  for (int row = 0; row <= n; ++row) {
    const double lat = rect.northDeg - rect.heightDeg() * row / n;
    for (int column = 0; column <= n; ++column) {
      const double lon = rect.westDeg + rect.widthDeg() * column / n;
      const double height = sampleHeightM(heights, lat, lon).value_or(0.0);
      const double sea = sampleSeaLevelM(heights, lat, lon).value_or(0.0);
      terrain.heightM.push_back(static_cast<float>(height));
      terrain.seaLevelM.push_back(static_cast<float>(sea));
      terrain.landM.push_back(std::max(height - sea, 0.0));
    }
  }
  return terrain;
}

/** The drawn height of grid vertex (@p column, @p row). */
double landAt(const GridTerrain& terrain, int n, int column, int row)
{
  return terrain.landM[static_cast<std::size_t>(gridIndex(n, column, row))];
}

/**
 * The slope (east, north) of the drawn terrain at grid vertex (column,
 * row): central differences inside the grid, one-sided on its border.
 */
// Column then row, as gridIndex().
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::array<float, 2> slopeAt(const GridTerrain& terrain, TileKey key, int n,
                             int column, int row)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const GeoRect rect = key.bounds();
  const int west = std::max(column - 1, 0);
  const int east = std::min(column + 1, n);
  const int north = std::max(row - 1, 0);
  const int south = std::min(row + 1, n);
  const double lat = rect.northDeg - rect.heightDeg() * row / n;
  // Metres per degree on the ellipsoid, near enough for shading.
  const double metresPerDegree = degToRad(1.0) * wgs84::semiMajorAxisM;
  const double dxM = (east - west) * rect.widthDeg() / n * metresPerDegree *
                     std::max(std::cos(degToRad(lat)), 1e-6);
  const double dyM = (south - north) * rect.heightDeg() / n * metresPerDegree;
  const double towardsEast =
      dxM > 0.0
          ? (landAt(terrain, n, east, row) - landAt(terrain, n, west, row)) /
                dxM
          : 0.0;
  const double towardsNorth = dyM > 0.0 ? (landAt(terrain, n, column, north) -
                                           landAt(terrain, n, column, south)) /
                                              dyM
                                        : 0.0;
  return {static_cast<float>(towardsEast), static_cast<float>(towardsNorth)};
}

} // namespace

TileMesh buildTileMesh(TileKey key, int segments, const HeightTile* heights)
{
  const int n = clampSegments(segments);
  const GeoRect rect = key.bounds();
  TileMesh mesh;
  mesh.key = key;
  mesh.segments = n;
  mesh.centreEcef = key.centreEcef();
  const int vertexCount = (n + 1) * (n + 1) + 4 * n;
  mesh.vertices.reserve(static_cast<std::size_t>(vertexCount));
  const GridTerrain terrain =
      heights != nullptr ? sampleTerrain(key, n, *heights) : GridTerrain{};

  for (int row = 0; row <= n; ++row) {
    const double v = static_cast<double>(row) / n;
    const double lat = rect.northDeg - v * rect.heightDeg();
    for (int column = 0; column <= n; ++column) {
      const double u = static_cast<double>(column) / n;
      const double lon = rect.westDeg + u * rect.widthDeg();
      const Vec3 position = geodeticToEcef(Geodetic{lat, lon, 0.0});
      const Vec3 offset = position - mesh.centreEcef;
      mesh.radiusM = std::max(mesh.radiusM, length(offset));
      TileVertex vertex{toFloat3(offset),
                        {static_cast<float>(u), static_cast<float>(v)},
                        {},
                        {}};
      if (heights != nullptr) {
        const std::size_t i =
            static_cast<std::size_t>(gridIndex(n, column, row));
        vertex.terrain = {terrain.heightM[i], terrain.seaLevelM[i], 0.0F};
        vertex.slope = slopeAt(terrain, key, n, column, row);
      }
      mesh.vertices.push_back(vertex);
    }
  }

  // The skirt hangs below the chord sag and, with terrain, as far as the
  // tile's drawn terrain varies (in terrain metres, so it scales with the
  // exaggeration), plus a metre.
  float dropM = 0.0F;
  if (heights != nullptr) {
    const std::pair<std::vector<double>::const_iterator,
                    std::vector<double>::const_iterator>
        range = std::minmax_element(terrain.landM.begin(), terrain.landM.end());
    dropM = static_cast<float>(*range.second - *range.first + 1.0);
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
    TileVertex skirt{toFloat3(offset), top.uv, top.terrain, top.slope};
    skirt.terrain[2] = dropM;
    mesh.vertices.push_back(skirt);
  }
  return mesh;
}

std::vector<std::uint16_t> tileMeshStripIndices(int segments)
{
  const int n = clampSegments(segments);
  std::vector<std::uint16_t> indices;
  indices.reserve(6 * static_cast<std::size_t>(n));
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
