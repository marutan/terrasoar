// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Turning a tile of vector features into triangles and line segments on
 * the ellipsoid, ready for the GPU.
 */
#ifndef TS_VECTOR_VECTORMESH_H
#define TS_VECTOR_VECTORMESH_H

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "ts/maths/vec3.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

struct FeatureTile;

/**
 * One vertex on the ellipsoid: its offset from the tile centre in metres
 * (ECEF, float) and in longitude and latitude. The second places the
 * vertex on the flat map and clips draws to a tile; as an offset from the
 * centre it keeps a float's precision at any zoom, like the first.
 */
struct VectorVertex {
  /** ECEF offset from VectorTileMesh::centreEcef, metres. */
  std::array<float, 3> offsetM{};
  /**
   * Longitude and latitude minus VectorTileMesh::centreLonDeg and
   * centreLatDeg, degrees.
   */
  std::array<float, 2> lonLatOffsetDeg{};
};

static_assert(sizeof(VectorVertex) == 20, "vertex stride");

/** One straight piece of an outline or line string, drawn as one quad. */
struct VectorSegment {
  /** Start. */
  VectorVertex a;
  /** End. */
  VectorVertex b;
};

static_assert(sizeof(VectorSegment) == 40, "segment stride");

/**
 * The triangles and segments of one stored feature tile.
 *
 * @reentrant
 */
struct VectorTileMesh {
  /** The tile the features were stored for (FeatureTile::key). */
  TileKey key;
  /** Origin of the vertices' offsets: the tile's centre on the ellipsoid. */
  Vec3 centreEcef;
  /** Longitude of the tile's centre, degrees. */
  double centreLonDeg = 0.0;
  /** Latitude of the tile's centre, degrees. */
  double centreLatDeg = 0.0;

  /** Returns the longitude of @p vertex, degrees. */
  double lonDeg(const VectorVertex& vertex) const
  {
    return centreLonDeg + static_cast<double>(vertex.lonLatOffsetDeg[0]);
  }

  /** Returns the latitude of @p vertex, degrees. */
  double latDeg(const VectorVertex& vertex) const
  {
    return centreLatDeg + static_cast<double>(vertex.lonLatOffsetDeg[1]);
  }
  /** Polygon fill, three vertices per triangle, not indexed. */
  std::vector<VectorVertex> triangles;
  /**
   * Outline segments: polygon rings without the parts lying on the tile's
   * edges (cuts, the antimeridian, the poles), and line strings.
   */
  std::vector<VectorSegment> segments;

  /** Returns the bytes the vertex data takes. */
  std::size_t bytes() const
  {
    return triangles.size() * sizeof(VectorVertex) +
           segments.size() * sizeof(VectorSegment);
  }
};

/** How finely to cut. */
struct VectorMeshSettings {
  /** Fill cells per tile side (a power of two). */
  int cellsPerSide = 16;
  /** Largest cell, degrees: big tiles get more cells. */
  double maximumCellDeg = 5.625;
  /** Longest segment, as a fraction of the cell size. */
  double segmentFraction = 0.25;
  /**
   * Polygons (and holes) whose extent is below this fraction of the
   * tile's size are left out, unless they touch the tile's edge: about two
   * pixels of a tile shown 1024 pixels wide, so they would only be dots.
   */
  double minimumExtentFraction = 1.0 / 512.0;
};

/**
 * Builds VectorTileMesh objects. Polygons smaller than about a pixel are
 * left out (VectorMeshSettings::minimumExtentFraction). Polygons are cut
 * into a grid of cells
 * (cellSizeDeg()) and each piece triangulated (GEOS constrained Delaunay),
 * so no triangle spans more than one cell and its straight edges stay
 * close to the curved surface; long outline segments are split the same
 * way, and so are a polygon's edges before it is cut, so fill and outline
 * follow the same curve on the globe and in every map projection. See
 * docs/design/vector-rendering.md.
 *
 * Holds a GEOS context: each thread uses its own mesher.
 *
 * @reentrant
 */
class VectorMesher
{
public:
  /** Creates a mesher. */
  explicit VectorMesher(const VectorMeshSettings& settings = {});
  ~VectorMesher();
  VectorMesher(const VectorMesher&) = delete;
  VectorMesher& operator=(const VectorMesher&) = delete;
  /** Takes over @p other's GEOS context. */
  VectorMesher(VectorMesher&& other) noexcept;
  /** Takes over @p other's GEOS context. */
  VectorMesher& operator=(VectorMesher&& other) noexcept;

  /** Returns the cell size used for tiles of @p level, degrees. */
  double cellSizeDeg(int level) const;

  /**
   * Builds the mesh of @p tile. Returns nothing if @p cancelled became
   * true. Bad geometry is skipped.
   */
  std::optional<VectorTileMesh> build(const FeatureTile& tile,
                                      const std::atomic<bool>& cancelled);

private:
  struct Private;
  std::unique_ptr<Private> d;
};

} // namespace ts

#endif // TS_VECTOR_VECTORMESH_H
