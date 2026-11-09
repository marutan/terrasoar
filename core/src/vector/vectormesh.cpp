// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/vector/vectormesh.h"

#include <algorithm>
#include <cmath>
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
struct VectorMesher::Private {
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
  static VectorVertex vertex(double lonDeg, double latDeg, const Vec3& centre)
  {
    const Vec3 ecef = geodeticToEcef(Geodetic{latDeg, lonDeg, 0.0});
    VectorVertex v;
    v.offsetM = {static_cast<float>(ecef.x - centre.x),
                 static_cast<float>(ecef.y - centre.y),
                 static_cast<float>(ecef.z - centre.z)};
    v.lonLatDeg = {static_cast<float>(lonDeg), static_cast<float>(latDeg)};
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
          GEOSCoordSeq_create_r(c, static_cast<unsigned int>(part.size()), 2);
      for (std::size_t i = 0; i < part.size(); ++i) {
        GEOSCoordSeq_setXY_r(c, sequence, static_cast<unsigned int>(i),
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
    GeomPtr result = wrap(
        GEOSGeom_createPolygon_r(c, rings.front(), rings.data() + 1,
                                 static_cast<unsigned int>(rings.size() - 1)));
    if (result && GEOSisValid_r(c, result.get()) != 1) {
      result = wrap(GEOSMakeValid_r(c, result.get()));
    }
    return result;
  }

  /** Appends the triangles of @p piece (a polygonal geometry). */
  void triangulate(const GEOSGeometry* piece, const Vec3& centre,
                   std::vector<VectorVertex>* triangles) const
  {
    GEOSContextHandle_t c = context();
    const GeomPtr result =
        wrap(GEOSConstrainedDelaunayTriangulation_r(c, piece));
    if (!result) {
      return;
    }
    const int count = GEOSGetNumGeometries_r(c, result.get());
    for (int i = 0; i < count; ++i) {
      const GEOSGeometry* triangle = GEOSGetGeometryN_r(c, result.get(), i);
      const GEOSGeometry* ring = GEOSGetExteriorRing_r(c, triangle);
      const GEOSCoordSequence* sequence =
          ring ? GEOSGeom_getCoordSeq_r(c, ring) : nullptr;
      unsigned int size = 0;
      if (sequence == nullptr ||
          GEOSCoordSeq_getSize_r(c, sequence, &size) == 0 || size < 3) {
        continue;
      }
      for (unsigned int k = 0; k < 3; ++k) {
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
           const Vec3& centre, const std::atomic<bool>& cancelled,
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
                          const Vec3& centre,
                          std::vector<VectorSegment>* segments)
  {
    const double span =
        std::max(std::abs(b.lonDeg - a.lonDeg), std::abs(b.latDeg - a.latDeg));
    const int pieces =
        std::max(1, static_cast<int>(std::ceil(span / maximumDeg)));
    VectorVertex previous = vertex(a.lonDeg, a.latDeg, centre);
    for (int i = 1; i <= pieces; ++i) {
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

double VectorMesher::cellSizeDeg(int level) const
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

std::optional<VectorTileMesh>
VectorMesher::build(const FeatureTile& tile, const std::atomic<bool>& cancelled)
{
  VectorTileMesh mesh;
  mesh.key = tile.key;
  mesh.centreEcef = tile.key.centreEcef();
  const GeoRect bounds = tile.key.bounds();
  const Rect rect{bounds.westDeg, bounds.southDeg, bounds.eastDeg,
                  bounds.northDeg};
  const double cellDeg = cellSizeDeg(tile.key.level);
  const double segmentDeg = cellDeg * d->settings.segmentFraction;
  const double minimumDeg =
      TileKey::sizeDeg(tile.key.level) * d->settings.minimumExtentFraction;

  for (const Feature& feature : tile.features) {
    if (cancelled.load()) {
      return std::nullopt;
    }
    if (feature.type == Feature::Type::Points) {
      continue;
    }
    const bool rings = feature.type == Feature::Type::Polygon;
    const std::vector<const std::vector<GeoPoint>*> parts =
        visibleParts(feature, minimumDeg, rect);
    if (rings && !parts.empty()) {
      const GeomPtr polygon = d->polygon(parts);
      if (polygon && GEOSisEmpty_r(d->context(), polygon.get()) == 0 &&
          !d->cut(polygon.get(), rect, cellDeg, mesh.centreEcef, cancelled,
                  &mesh.triangles)) {
        return std::nullopt;
      }
    }
    // Outlines: every ring or line, without the tile's own edges.
    for (const std::vector<GeoPoint>* part : parts) {
      for (std::size_t i = 0; i + 1 < part->size(); ++i) {
        const GeoPoint a = (*part)[i];
        const GeoPoint b = (*part)[i + 1];
        if (!rings || !onTileEdge(a, b, rect)) {
          Private::addSegments(a, b, segmentDeg, mesh.centreEcef,
                               &mesh.segments);
        }
      }
    }
  }
  return mesh;
}

} // namespace ts
