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

#include "ts/vector/vectormesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>

#define GEOS_USE_ONLY_R_API
#include <geos_c.h>

#include "ts/deps/geoscontext.h"
#include "ts/geo/geodetic.h"
#include "ts/providers/tileprovider.h"

namespace ts
{

namespace
{

/** Tolerance for "on the tile's edge", degrees. */
constexpr double edgeEpsilonDeg = 1e-9;

/** A longitude/latitude rectangle, degrees. */
struct Rect {
  double west = 0.0;
  double south = 0.0;
  double east = 0.0;
  double north = 0.0;

  double widthDeg() const { return east - west; }
};

/** The origin of a mesh's vertices. */
struct Centre {
  Vec3 ecef;
  double lonDeg = 0.0;
  double latDeg = 0.0;
};

/** Destroys a GEOS geometry with its context. */
struct GeomDeleter {
  GEOSContextHandle_t context = nullptr;
  void operator()(GEOSGeometry* geometry) const
  {
    if (geometry != nullptr) {
      GEOSGeom_destroy_r(context, geometry);
    }
  }
};
using GeomPtr = std::unique_ptr<GEOSGeometry, GeomDeleter>;

bool near(double value, double edge)
{
  return std::abs(value - edge) < edgeEpsilonDeg;
}

/** True if a → b lies along one of @p tile's edges. */
bool onTileEdge(GeoPoint a, GeoPoint b, const Rect& tile)
{
  return (near(a.lonDeg, tile.west) && near(b.lonDeg, tile.west)) ||
         (near(a.lonDeg, tile.east) && near(b.lonDeg, tile.east)) ||
         (near(a.latDeg, tile.south) && near(b.latDeg, tile.south)) ||
         (near(a.latDeg, tile.north) && near(b.latDeg, tile.north));
}

/** True if @p ring is smaller than @p limitDeg both ways and does not
 * touch @p tile's edges. */
bool isTiny(const std::vector<GeoPoint>& ring, double limitDeg,
            const Rect& tile)
{
  if (ring.empty()) {
    return true;
  }
  double west = ring.front().lonDeg;
  double east = west;
  double south = ring.front().latDeg;
  double north = south;
  for (const GeoPoint& point : ring) {
    west = std::min(west, point.lonDeg);
    east = std::max(east, point.lonDeg);
    south = std::min(south, point.latDeg);
    north = std::max(north, point.latDeg);
  }
  const bool touchesEdge = near(west, tile.west) || near(east, tile.east) ||
                           near(south, tile.south) || near(north, tile.north);
  return !touchesEdge && east - west < limitDeg && north - south < limitDeg;
}

} // namespace

/** The GEOS context and settings. */
namespace
{

/** One style's triangles, lines and points, joined into runs at the end. */
struct Bucket {
  std::vector<VectorVertex> triangles;
  std::vector<VectorSegment> lines;
  std::vector<VectorSegment> points;
};

/** What meshing a tile's features needs to know about the tile. */
struct MeshFrame {
  Centre centre;
  Rect rect;
  double cellDeg = 0.0;
  double segmentDeg = 0.0;
  double minimumDeg = 0.0;
};

} // namespace

struct VectorMesher::Private {
  /**
   * Meshes the lines and polygons of @p feature into @p bucket; false if
   * cancelled.
   */
  bool meshShape(const Feature& feature, const MeshFrame& frame,
                 const std::atomic<bool>& cancelled, Bucket& bucket) const;

  /** Points as dots: segments of no length, drawn as round caps. */
  static void addPoints(const Feature& feature, const MeshFrame& frame,
                        Bucket& bucket);

  VectorMeshSettings settings;
  GeosContext geos;

  GEOSContextHandle_t context() const
  {
    return static_cast<GEOSContextHandle_t>(geos.get());
  }

  GeomPtr wrap(GEOSGeometry* geometry) const
  {
    return GeomPtr(geometry, GeomDeleter{context()});
  }

  /** The vertex of a point, relative to @p centre. */
  static VectorVertex vertex(double lonDeg, double latDeg, const Centre& centre)
  {
    const Vec3 ecef = geodeticToEcef(Geodetic{latDeg, lonDeg, 0.0});
    VectorVertex v;
    v.offsetM = {static_cast<float>(ecef.x - centre.ecef.x),
                 static_cast<float>(ecef.y - centre.ecef.y),
                 static_cast<float>(ecef.z - centre.ecef.z)};
    v.lonLatOffsetDeg = {static_cast<float>(lonDeg - centre.lonDeg),
                         static_cast<float>(latDeg - centre.latDeg)};
    return v;
  }

  /** A GEOS polygon from @p parts (outer ring first), made valid; null
   * if empty. */
  GeomPtr polygon(const std::vector<const std::vector<GeoPoint>*>& parts) const
  {
    GEOSContextHandle_t c = context();
    std::vector<GEOSGeometry*> rings;
    for (const std::vector<GeoPoint>* pointer : parts) {
      const std::vector<GeoPoint>& part = *pointer;
      if (part.size() < 4) {
        continue;
      }
      GEOSCoordSequence* sequence =
          GEOSCoordSeq_create_r(c, static_cast<uint32_t>(part.size()), 2);
      for (std::size_t i = 0; i < part.size(); ++i) {
        GEOSCoordSeq_setXY_r(c, sequence, static_cast<uint32_t>(i),
                             part[i].lonDeg, part[i].latDeg);
      }
      GEOSGeometry* ring = GEOSGeom_createLinearRing_r(c, sequence);
      if (ring != nullptr) {
        rings.push_back(ring);
      }
    }
    if (rings.empty()) {
      return wrap(nullptr);
    }
    GeomPtr result =
        wrap(GEOSGeom_createPolygon_r(c, rings.front(), rings.data() + 1,
                                      static_cast<uint32_t>(rings.size() - 1)));
    if (result && GEOSisValid_r(c, result.get()) != 1) {
      result = wrap(GEOSMakeValid_r(c, result.get()));
    }
    return result;
  }

  /** Appends the triangles of @p piece (a polygonal geometry). */
  void triangulate(const GEOSGeometry* piece, const Centre& centre,
                   std::vector<VectorVertex>* triangles) const
  {
    GEOSContextHandle_t c = context();
    const GeomPtr result =
        wrap(GEOSConstrainedDelaunayTriangulation_r(c, piece));
    if (!result) {
      return;
    }
    const int32_t count = GEOSGetNumGeometries_r(c, result.get());
    for (int32_t i = 0; i < count; ++i) {
      const GEOSGeometry* triangle = GEOSGetGeometryN_r(c, result.get(), i);
      const GEOSGeometry* ring = GEOSGetExteriorRing_r(c, triangle);
      const GEOSCoordSequence* sequence =
          ring ? GEOSGeom_getCoordSeq_r(c, ring) : nullptr;
      uint32_t size = 0;
      if (sequence == nullptr ||
          GEOSCoordSeq_getSize_r(c, sequence, &size) == 0 || size < 3) {
        continue;
      }
      for (uint32_t k = 0; k < 3; ++k) {
        double x = 0.0;
        double y = 0.0;
        GEOSCoordSeq_getXY_r(c, sequence, k, &x, &y);
        triangles->push_back(vertex(x, y, centre));
      }
    }
  }

  /**
   * Cuts @p geometry (inside @p rect) into quadrants until they are no
   * wider than @p cellDeg, then triangulates the pieces.
   */
  bool cut(const GEOSGeometry* geometry, const Rect& rect, double cellDeg,
           const Centre& centre, const std::atomic<bool>& cancelled,
           std::vector<VectorVertex>* triangles) const
  {
    if (cancelled.load()) {
      return false;
    }
    if (rect.widthDeg() <= cellDeg * 1.000001) {
      triangulate(geometry, centre, triangles);
      return true;
    }
    GEOSContextHandle_t c = context();
    const double midLon = 0.5 * (rect.west + rect.east);
    const double midLat = 0.5 * (rect.south + rect.north);
    const std::array<Rect, 4> quadrants = {
        Rect{rect.west, midLat, midLon, rect.north},
        Rect{midLon, midLat, rect.east, rect.north},
        Rect{rect.west, rect.south, midLon, midLat},
        Rect{midLon, rect.south, rect.east, midLat}};
    for (const Rect& quadrant : quadrants) {
      const GeomPtr piece =
          wrap(GEOSClipByRect_r(c, geometry, quadrant.west, quadrant.south,
                                quadrant.east, quadrant.north));
      if (!piece || GEOSisEmpty_r(c, piece.get()) != 0) {
        continue;
      }
      double area = 0.0;
      if (GEOSArea_r(c, piece.get(), &area) == 0 || area <= 0.0) {
        continue;
      }
      if (!cut(piece.get(), quadrant, cellDeg, centre, cancelled, triangles)) {
        return false;
      }
    }
    return true;
  }

  /** Appends @p a → @p b as segments no longer than @p maximumDeg. */
  static void addSegments(GeoPoint a, GeoPoint b, double maximumDeg,
                          const Centre& centre,
                          std::vector<VectorSegment>* segments)
  {
    const double span =
        std::max(std::abs(b.lonDeg - a.lonDeg), std::abs(b.latDeg - a.latDeg));
    const int32_t pieces =
        std::max(1, static_cast<int32_t>(std::ceil(span / maximumDeg)));
    VectorVertex previous = vertex(a.lonDeg, a.latDeg, centre);
    for (int32_t i = 1; i <= pieces; ++i) {
      const double t = static_cast<double>(i) / pieces;
      const VectorVertex next =
          vertex(a.lonDeg + t * (b.lonDeg - a.lonDeg),
                 a.latDeg + t * (b.latDeg - a.latDeg), centre);
      segments->push_back(VectorSegment{previous, next});
      previous = next;
    }
  }
};

VectorMesher::VectorMesher(const VectorMeshSettings& settings)
    : d(std::make_unique<Private>())
{
  d->settings = settings;
  d->settings.cellsPerSide = std::max(1, d->settings.cellsPerSide);
}

VectorMesher::~VectorMesher() = default;
VectorMesher::VectorMesher(VectorMesher&&) noexcept = default;
VectorMesher& VectorMesher::operator=(VectorMesher&&) noexcept = default;

double VectorMesher::cellSizeDeg(int32_t level) const
{
  // A power-of-two division of the tile, at most maximumCellDeg.
  double cell = TileKey::sizeDeg(level) / d->settings.cellsPerSide;
  while (cell > d->settings.maximumCellDeg) {
    cell *= 0.5;
  }
  return cell;
}

namespace
{

/**
 * Returns @p ring with every edge split into pieces no longer than
 * @p maximumDeg (in longitude or latitude), the way addSegments() splits
 * outlines. An edge is a straight line in longitude and latitude; on the
 * ellipsoid and on the flat maps that is a curve, which the fill's
 * straight triangle edges would otherwise cut short where the outline
 * follows it.
 */
std::vector<GeoPoint> densified(const std::vector<GeoPoint>& ring,
                                double maximumDeg)
{
  std::vector<GeoPoint> result;
  result.reserve(ring.size());
  for (std::size_t i = 0; i < ring.size(); ++i) {
    if (i > 0) {
      const GeoPoint a = ring[i - 1];
      const GeoPoint b = ring[i];
      const double span = std::max(std::abs(b.lonDeg - a.lonDeg),
                                   std::abs(b.latDeg - a.latDeg));
      const int32_t pieces =
          std::max(1, static_cast<int32_t>(std::ceil(span / maximumDeg)));
      for (int32_t k = 1; k < pieces; ++k) {
        const double t = static_cast<double>(k) / pieces;
        result.push_back(GeoPoint{a.latDeg + t * (b.latDeg - a.latDeg),
                                  a.lonDeg + t * (b.lonDeg - a.lonDeg)});
      }
    }
    result.push_back(ring[i]);
  }
  return result;
}

/**
 * The parts of @p feature worth drawing: rings too small to see are left
 * out, and a polygon whose outer ring is, entirely.
 */
std::vector<const std::vector<GeoPoint>*>
visibleParts(const Feature& feature, double minimumDeg, const Rect& rect)
{
  std::vector<const std::vector<GeoPoint>*> parts;
  const bool rings = feature.type == Feature::Type::Polygon;
  for (std::size_t i = 0; i < feature.parts.size(); ++i) {
    const bool tiny = rings && isTiny(feature.parts[i], minimumDeg, rect);
    if (tiny && i == 0) {
      return {};
    }
    if (!tiny) {
      parts.push_back(&feature.parts[i]);
    }
  }
  return parts;
}

} // namespace

namespace
{

/** The index of @p feature's style in @p mesh's (0 for the layer's). */
std::size_t styleIndex(const Feature& feature, VectorTileMesh& mesh,
                       std::vector<Bucket>& buckets)
{
  if (!feature.style || feature.style->isEmpty()) {
    return 0;
  }
  const std::vector<FeatureStyle>::const_iterator known =
      std::find(mesh.styles.begin(), mesh.styles.end(), *feature.style);
  const std::size_t index =
      static_cast<std::size_t>(known - mesh.styles.begin()) + 1;
  if (known == mesh.styles.end()) {
    mesh.styles.push_back(*feature.style);
    buckets.emplace_back();
  }
  return index;
}

template <typename T>
void appendRun(std::vector<T>& all, const std::vector<T>& part,
               std::vector<VectorRun>& runs, int32_t style, bool points)
{
  const std::size_t before = all.size();
  all.insert(all.end(), part.begin(), part.end());
  if (all.size() > before) {
    runs.push_back(VectorRun{static_cast<int32_t>(before),
                             static_cast<int32_t>(all.size() - before), style,
                             points});
  }
}

/** Joins the buckets into @p mesh: fills and lines by style, then points. */
void joinRuns(const std::vector<Bucket>& buckets, VectorTileMesh& mesh)
{
  for (std::size_t s = 0; s < buckets.size(); ++s) {
    appendRun(mesh.triangles, buckets[s].triangles, mesh.fillRuns,
              static_cast<int32_t>(s), false);
    appendRun(mesh.segments, buckets[s].lines, mesh.segmentRuns,
              static_cast<int32_t>(s), false);
  }
  for (std::size_t s = 0; s < buckets.size(); ++s) {
    appendRun(mesh.segments, buckets[s].points, mesh.segmentRuns,
              static_cast<int32_t>(s), true);
  }
}

} // namespace

bool VectorMesher::Private::meshShape(const Feature& feature,
                                      const MeshFrame& frame,
                                      const std::atomic<bool>& cancelled,
                                      Bucket& bucket) const
{
  const bool rings = feature.type == Feature::Type::Polygon;
  const std::vector<const std::vector<GeoPoint>*> parts =
      visibleParts(feature, frame.minimumDeg, frame.rect);
  if (rings && !parts.empty()) {
    // The fill's boundary gets the outline's vertices.
    std::vector<std::vector<GeoPoint>> dense;
    dense.reserve(parts.size());
    std::vector<const std::vector<GeoPoint>*> denseParts;
    for (const std::vector<GeoPoint>* part : parts) {
      dense.push_back(densified(*part, frame.segmentDeg));
      denseParts.push_back(&dense.back());
    }
    const GeomPtr shape = polygon(denseParts);
    if (shape && GEOSisEmpty_r(context(), shape.get()) == 0 &&
        !cut(shape.get(), frame.rect, frame.cellDeg, frame.centre, cancelled,
             &bucket.triangles)) {
      return false;
    }
  }
  // Outlines: every ring or line, without the tile's own edges.
  for (const std::vector<GeoPoint>* part : parts) {
    for (std::size_t i = 0; i + 1 < part->size(); ++i) {
      const GeoPoint a = (*part)[i];
      const GeoPoint b = (*part)[i + 1];
      if (!rings || !onTileEdge(a, b, frame.rect)) {
        addSegments(a, b, frame.segmentDeg, frame.centre, &bucket.lines);
      }
    }
  }
  return true;
}

void VectorMesher::Private::addPoints(const Feature& feature,
                                      const MeshFrame& frame, Bucket& bucket)
{
  const Rect& rect = frame.rect;
  for (const std::vector<GeoPoint>& part : feature.parts) {
    for (const GeoPoint& point : part) {
      if (point.lonDeg >= rect.west && point.lonDeg <= rect.east &&
          point.latDeg >= rect.south && point.latDeg <= rect.north) {
        const VectorVertex v = vertex(point.lonDeg, point.latDeg, frame.centre);
        bucket.points.push_back(VectorSegment{v, v});
      }
    }
  }
}

std::optional<VectorTileMesh>
VectorMesher::build(const FeatureTile& tile, const std::atomic<bool>& cancelled)
{
  VectorTileMesh mesh;
  mesh.key = tile.key;
  mesh.centreEcef = tile.key.centreEcef();
  const GeoRect bounds = tile.key.bounds();
  mesh.centreLonDeg = bounds.centreLonDeg();
  mesh.centreLatDeg = bounds.centreLatDeg();
  MeshFrame frame;
  frame.centre = Centre{mesh.centreEcef, mesh.centreLonDeg, mesh.centreLatDeg};
  frame.rect =
      Rect{bounds.westDeg, bounds.southDeg, bounds.eastDeg, bounds.northDeg};
  frame.cellDeg = cellSizeDeg(tile.key.level);
  frame.segmentDeg = frame.cellDeg * d->settings.segmentFraction;
  frame.minimumDeg =
      TileKey::sizeDeg(tile.key.level) * d->settings.minimumExtentFraction;
  // Each style's output apart: bucket 0 is the layer's style.
  std::vector<Bucket> buckets(1);
  for (const Feature& feature : tile.features) {
    if (cancelled.load()) {
      return std::nullopt;
    }
    const std::size_t style = styleIndex(feature, mesh, buckets);
    if (feature.type == Feature::Type::Points) {
      Private::addPoints(feature, frame, buckets.at(style));
    } else if (!d->meshShape(feature, frame, cancelled, buckets.at(style))) {
      return std::nullopt;
    }
  }
  joinRuns(buckets, mesh);
  return mesh;
}

} // namespace ts
