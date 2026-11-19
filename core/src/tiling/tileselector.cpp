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

#include "ts/tiling/tileselector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include "ts/camera/camera.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/projection.h"
#include "ts/tiling/tilemesh.h"

namespace ts
{

namespace
{

/** Surface points of a tile: corners, edge midpoints and centre. */
using TileSamples = std::array<Vec3, 9>;

TileSamples tileSamples(TileKey key)
{
  const GeoRect rect = key.bounds();
  TileSamples samples;
  std::size_t i = 0;
  for (int32_t row = 0; row < 3; ++row) {
    const double lat = rect.northDeg - 0.5 * row * rect.heightDeg();
    for (int32_t column = 0; column < 3; ++column) {
      const double lon = rect.westDeg + 0.5 * column * rect.widthDeg();
      samples.at(i++) = geodeticToEcef(Geodetic{lat, lon, 0.0});
    }
  }
  return samples;
}

/** Angle between neighbouring samples along a meridian, radians. */
double sampleSpacingRad(TileKey key)
{
  return 0.5 * degToRad(TileKey::sizeDeg(key.level));
}

TileBounds boundsFromSamples(TileKey key, const TileSamples& samples)
{
  TileBounds bounds;
  bounds.centreEcef = samples[4];
  for (const Vec3& sample : samples) {
    bounds.radiusM =
        std::max(bounds.radiusM, distance(sample, bounds.centreEcef));
  }
  // Between samples the surface bulges out from their chords by at most
  // the sag of an arc of one sample spacing; the skirt hangs below.
  const double sagM =
      wgs84::semiMajorAxisM * (1.0 - std::cos(0.5 * sampleSpacingRad(key)));
  bounds.radiusM +=
      sagM + tileSkirtDepthM(key.level, TileMesh::defaultSegments);
  return bounds;
}

bool sphereInFrustum(const Camera& camera, const TileBounds& bounds)
{
  const Vec3 offset = bounds.centreEcef - camera.eyeEcef();
  const double z = dot(offset, camera.forwardEcef());
  const double x = dot(offset, camera.rightEcef());
  const double y = dot(offset, camera.upEcef());
  const double r = bounds.radiusM;
  if (z < -r || z - r > camera.farM()) {
    return false;
  }
  // Side planes through the eye; the sphere is outside one if its centre
  // is farther than r beyond it.
  const double tanX = camera.tanHalfFovX();
  const double tanY = camera.tanHalfFovY();
  const double scaleX = std::sqrt(1.0 + tanX * tanX);
  const double scaleY = std::sqrt(1.0 + tanY * tanY);
  return std::abs(x) - z * tanX <= r * scaleX &&
         std::abs(y) - z * tanY <= r * scaleY;
}

bool samplesBehindHorizon(const Camera& camera, TileKey key,
                          const TileSamples& samples, double raisedM)
{
  // Very large tiles curve too much for nine samples to bound them.
  if (key.level < 2) {
    return false;
  }
  // In the frame scaled by (1/a, 1/a, 1/b) the ellipsoid is the unit
  // sphere and a surface point q can be seen from the eye v exactly when
  // q·v ≥ 1. Between samples q·v changes by at most |v| × the angle moved
  // (plus a little for the scaling), so a tile whose samples all fall
  // short by more than that is hidden.
  const double a = wgs84::semiMajorAxisM;
  const double b = wgs84::semiMinorAxisM;
  const Vec3& eye = camera.eyeEcef();
  const Vec3 v{eye.x / a, eye.y / a, eye.z / b};
  // Terrain raised by h stays visible down to q·v ≈ 1 − h / b.
  const double margin =
      length(v) * sampleSpacingRad(key) * 1.01 + std::max(raisedM, 0.0) / b;
  return std::all_of(samples.begin(), samples.end(),
                     [&v, margin, a, b](const Vec3& sample) {
                       const Vec3 q{sample.x / a, sample.y / a, sample.z / b};
                       return dot(q, v) < 1.0 - margin;
                     });
}

/**
 * Returns the part of @p rect inside @p projection's domain, its
 * longitudes counted from the central meridian (so west may be below
 * −180° or east above 180°), or nothing if they don't meet.
 */
std::optional<GeoRect> domainPart(const Projection& projection,
                                  const GeoRect& rect)
{
  const double southDeg = std::max(rect.southDeg, projection.minimumLatDeg());
  const double northDeg = std::min(rect.northDeg, projection.maximumLatDeg());
  if (!(southDeg < northDeg)) {
    return std::nullopt;
  }
  const double limit = projection.maximumLonOffsetDeg();
  const double centre = projection.centralMeridianDeg();
  const double west = normaliseLonDeg(rect.westDeg - centre);
  // The tile, or its copy a turn to the west, meets −limit…limit.
  for (const double shift : {0.0, -360.0}) {
    const double w = std::max(west + shift, -limit);
    const double e = std::min(west + shift + rect.widthDeg(), limit);
    if (w < e) {
      return GeoRect{w + centre, southDeg, e + centre, northDeg};
    }
  }
  return std::nullopt;
}

/** Points sampled along each edge of a tile for its extent on a map that
 *  isn't cylindrical. */
constexpr int32_t mapEdgeSamples = 8;

/**
 * Returns the extent on the map of the part of @p key inside the domain,
 * relative to the view's centre, map metres: a box around points sampled
 * along its edges (exact for a cylindrical projection, grown by a
 * fiftieth for the bulge of curved edges otherwise). Nothing outside the
 * domain. For maps that don't repeat.
 */
std::optional<MapRect> tileMapExtent(const MapFrame& frame, TileKey key)
{
  const Projection& projection = *frame.projection;
  const std::optional<GeoRect> part = domainPart(projection, key.bounds());
  if (!part) {
    return std::nullopt;
  }
  MapRect extent{std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::lowest(),
                 std::numeric_limits<double>::lowest()};
  const std::function<void(double, double)> include = [&](double latDeg,
                                                          double lonDeg) {
    const MapPoint point = frame.offsetOf(latDeg, lonDeg);
    extent.westM = std::min(extent.westM, point.xM);
    extent.eastM = std::max(extent.eastM, point.xM);
    extent.southM = std::min(extent.southM, point.yM);
    extent.northM = std::max(extent.northM, point.yM);
  };
  for (int32_t i = 0; i <= mapEdgeSamples; ++i) {
    const double f = static_cast<double>(i) / mapEdgeSamples;
    const double lon = part->westDeg + f * part->widthDeg();
    const double lat = part->southDeg + f * part->heightDeg();
    include(part->southDeg, lon);
    include(part->northDeg, lon);
    include(lat, part->westDeg);
    include(lat, part->eastDeg);
  }
  if (!projection.isCylindrical()) {
    const double growM = 0.02 * std::max(extent.widthM(), extent.heightM());
    extent.westM -= growM;
    extent.eastM += growM;
    extent.southM -= growM;
    extent.northM += growM;
  }
  return extent;
}

/**
 * How far the straight edges of @p key's mesh cells stray from the curves
 * they stand for on a map that isn't cylindrical, on screen: the largest
 * gap between an edge's middle and the projected point it should be, over
 * the cells in the corners and the middle of the tile's part in the
 * domain (where the curvature is largest: projections curve away from
 * their centre).
 */
double generalMapGeometryErrorPx(const Camera& camera, const MapFrame& frame,
                                 TileKey key, int32_t segments)
{
  const Projection& projection = *frame.projection;
  const std::optional<GeoRect> part = domainPart(projection, key.bounds());
  if (!part) {
    return 0.0;
  }
  const GeoRect rect = key.bounds();
  const double cellLonDeg = rect.widthDeg() / std::max(segments, 1);
  const double cellLatDeg = rect.heightDeg() / std::max(segments, 1);
  const std::function<MapPoint(double, double)> at = [&](double latDeg,
                                                         double lonDeg) {
    return projection.forward(latDeg, lonDeg);
  };
  const std::function<double(double, double, double, double)> stray =
      [&](double lat0, double lon0, double lat1, double lon1) {
        const MapPoint a = at(lat0, lon0);
        const MapPoint b = at(lat1, lon1);
        const MapPoint middle = at(0.5 * (lat0 + lat1), 0.5 * (lon0 + lon1));
        return std::hypot(middle.xM - 0.5 * (a.xM + b.xM),
                          middle.yM - 0.5 * (a.yM + b.yM));
      };
  double errorM = 0.0;
  const std::array<double, 3> lons = {
      part->westDeg, 0.5 * (part->westDeg + part->eastDeg - cellLonDeg),
      std::max(part->eastDeg - cellLonDeg, part->westDeg)};
  const std::array<double, 3> lats = {
      part->southDeg, 0.5 * (part->southDeg + part->northDeg - cellLatDeg),
      std::max(part->northDeg - cellLatDeg, part->southDeg)};
  for (const double lon : lons) {
    for (const double lat : lats) {
      const double east = std::min(lon + cellLonDeg, part->eastDeg);
      const double north = std::min(lat + cellLatDeg, part->northDeg);
      errorM = std::max(
          {errorM, stray(lat, lon, lat, east), stray(lat, lon, north, lon),
           stray(north, lon, north, east), stray(lat, east, north, east)});
    }
  }
  return errorM / (2.0 * frame.halfHeightM) * camera.viewportHeightPx();
}

/** True if @p key's rectangle on the map of @p frame meets the view, taken
 * as ± @p halfWidthM by ± @p halfHeightM map metres about the centre. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): width, then height
bool rectOnMap(const MapFrame& frame, TileKey key, double halfWidthM,
               double halfHeightM)
{
  if (frame.projection == nullptr) {
    return false;
  }
  const Projection& projection = *frame.projection;
  if (!projection.repeats()) {
    const std::optional<MapRect> extent = tileMapExtent(frame, key);
    return extent && extent->westM < halfWidthM &&
           extent->eastM > -halfWidthM && extent->southM < halfHeightM &&
           extent->northM > -halfHeightM;
  }
  const GeoRect rect = key.bounds();
  if (rect.southDeg >= projection.maximumLatDeg() ||
      rect.northDeg <= projection.minimumLatDeg()) {
    return false;
  }
  const double southM =
      projection.forward(rect.southDeg, 0.0).yM - frame.centreYM;
  const double northM =
      projection.forward(rect.northDeg, 0.0).yM - frame.centreYM;
  if (southM > halfHeightM || northM < -halfHeightM) {
    return false;
  }
  // Any copy of the world.
  const double middleM =
      frame
          .offsetOf(projection.forward(frame.centreLatDeg, rect.centreLonDeg()))
          .xM;
  const double halfSpanM = 0.5 * rect.widthDeg() / 360.0 * frame.worldWidthM;
  const std::array<double, 3> shifts = {0.0, -frame.worldWidthM,
                                        frame.worldWidthM};
  return std::any_of(shifts.begin(), shifts.end(), [&](double shift) {
    return middleM + shift - halfSpanM < halfWidthM &&
           middleM + shift + halfSpanM > -halfWidthM;
  });
}

/**
 * A bounding sphere of a tile where it is drawn part of the way between
 * globe and map, relative to the eye.
 */
struct MorphSphere {
  /** Centre, ECEF axes, metres from the eye. */
  Vec3 offsetM{};
  /** Radius, metres. */
  double radiusM = 0.0;
};

/**
 * Returns the spheres bounding @p key while the camera's morph factor is
 * between 0 and 1, one for each copy of the world it is drawn in.
 *
 * Each vertex is drawn at mix(globe position, map position, t), so the
 * tile lies within mix(globe centre, map centre, t), radius
 * mix(globe radius, map radius, t): the distance of a mixed point from
 * the mixed centre is at most the mix of the two distances.
 */
std::vector<MorphSphere> morphSpheres(const Camera& camera,
                                      const MapFrame& frame, TileKey key,
                                      const TileBounds& globe, double raiseM)
{
  if (frame.projection == nullptr) {
    return {};
  }
  const double t = camera.morph();
  const Projection& projection = *frame.projection;
  const Vec3 globeOffset = globe.centreEcef - camera.eyeEcef();
  if (!projection.repeats()) {
    // Outside the domain the tile is squashed onto its edge, as the
    // renderer draws it.
    const std::optional<MapRect> extent = tileMapExtent(frame, key);
    if (!extent) {
      // All of it squashed onto the edge: somewhere on the map's outline.
      const MapRect bounds = projection.bounds();
      const MapPoint centre{
          0.5 * (bounds.westM + bounds.eastM) - frame.centreXM,
          0.5 * (bounds.southM + bounds.northM) - frame.centreYM};
      const double mapRadiusM = frame.planeScale * 0.5 *
                                std::hypot(bounds.widthM(), bounds.heightM());
      return {MorphSphere{
          globeOffset * (1.0 - t) + camera.mapPlanePoint(centre) * t,
          (1.0 - t) * (globe.radiusM + raiseM) + t * (mapRadiusM + raiseM)}};
    }
    const double mapRadiusM =
        frame.planeScale * 0.5 *
            std::hypot(extent->widthM(), extent->heightM()) +
        raiseM;
    const Vec3 mapOffset =
        camera.mapPlanePoint(MapPoint{0.5 * (extent->westM + extent->eastM),
                                      0.5 * (extent->southM + extent->northM)});
    return {MorphSphere{globeOffset * (1.0 - t) + mapOffset * t,
                        (1.0 - t) * (globe.radiusM + raiseM) + t * mapRadiusM}};
  }
  const GeoRect rect = key.bounds();
  // Beyond the projection's latitudes the tile is squashed onto its edge,
  // as the renderer draws it.
  const double southDeg = std::clamp(rect.southDeg, projection.minimumLatDeg(),
                                     projection.maximumLatDeg());
  const double northDeg = std::clamp(rect.northDeg, projection.minimumLatDeg(),
                                     projection.maximumLatDeg());
  const double southM = projection.forward(southDeg, 0.0).yM - frame.centreYM;
  const double northM = projection.forward(northDeg, 0.0).yM - frame.centreYM;
  const double middleXM =
      frame
          .offsetOf(projection.forward(frame.centreLatDeg, rect.centreLonDeg()))
          .xM;
  const double halfSpanM = 0.5 * rect.widthDeg() / 360.0 * frame.worldWidthM;
  const double mapRadiusM =
      frame.planeScale * std::hypot(halfSpanM, 0.5 * (northM - southM)) +
      raiseM;
  const double radiusM = (1.0 - t) * (globe.radiusM + raiseM) + t * mapRadiusM;
  std::vector<MorphSphere> spheres;
  for (const double shiftM :
       frame.copyShiftsM(rect.westDeg, rect.eastDeg, true)) {
    const Vec3 mapOffset = camera.mapPlanePoint(
        MapPoint{middleXM + shiftM, 0.5 * (southM + northM)});
    spheres.push_back(
        MorphSphere{globeOffset * (1.0 - t) + mapOffset * t, radiusM});
  }
  return spheres;
}

/** Returns true if @p sphere is inside the view frustum of @p camera. */
bool sphereInFrustum(const Camera& camera, const MorphSphere& sphere)
{
  return sphereInFrustum(
      camera, TileBounds{camera.eyeEcef() + sphere.offsetM, sphere.radiusM});
}

} // namespace

/** The camera and, when the map is involved, how it lies. */
struct TileSelector::View {
  /** The camera; never null. */
  const Camera* camera;
  /** The camera's morph factor. */
  double morph = 0.0;
  /** The map (when morph > 0). */
  MapFrame frame;
  /** The view on the map for culling, map metres. */
  double halfWidthM = 0.0;
  double halfHeightM = 0.0;
  /** TileSelectionSettings::terrainHeightM. */
  double terrainHeightM = 0.0;

  explicit View(const Camera& viewCamera, bool forceMap = false)
      : camera(&viewCamera), morph(forceMap ? 1.0 : viewCamera.morph())
  {
    if (morph > 0.0) {
      frame = viewCamera.mapFrame();
      halfWidthM = frame.halfWidthM;
      halfHeightM = frame.halfHeightM;
    }
  }

  bool isVisible(TileKey key, const TileSamples& samples,
                 const TileBounds& bounds) const
  {
    TileBounds raised = bounds;
    raised.radiusM += terrainHeightM;
    if (morph <= 0.0) {
      return sphereInFrustum(*camera, raised) &&
             !samplesBehindHorizon(*camera, key, samples, terrainHeightM);
    }
    if (morph >= 1.0) {
      return rectOnMap(frame, key, halfWidthM, halfHeightM);
    }
    // Changing view: where the tile is drawn now, between globe and map.
    // No horizon test: the far side unrolls into view.
    const std::vector<MorphSphere> spheres =
        morphSpheres(*camera, frame, key, bounds, terrainHeightM);
    return std::any_of(spheres.begin(), spheres.end(),
                       [this](const MorphSphere& sphere) {
                         return sphereInFrustum(*camera, sphere);
                       });
  }

  bool isVisible(TileKey key) const
  {
    const TileSamples samples = tileSamples(key);
    return isVisible(key, samples, boundsFromSamples(key, samples));
  }
};

TileBounds tileBounds(TileKey key)
{
  return boundsFromSamples(key, tileSamples(key));
}

TileSelector::TileSelector(const TileSelectionSettings& settings)
    : m_settings(settings)
{
}

void TileSelector::setSettings(const TileSelectionSettings& settings)
{
  m_settings = settings;
}

bool TileSelector::isInFrustum(const Camera& camera, TileKey key)
{
  return sphereInFrustum(camera, tileBounds(key));
}

bool TileSelector::isBehindHorizon(const Camera& camera, TileKey key)
{
  return samplesBehindHorizon(camera, key, tileSamples(key), 0.0);
}

bool TileSelector::isVisible(const Camera& camera, TileKey key)
{
  return View(camera).isVisible(key);
}

bool TileSelector::isVisibleOnMap(const Camera& camera, TileKey key)
{
  return View(camera, true).isVisible(key);
}

double TileSelector::screenSpaceErrorPx(const Camera& camera, TileKey key) const
{
  View view(camera);
  view.terrainHeightM = m_settings.terrainHeightM;
  return screenSpaceErrorPx(view, key);
}

double TileSelector::screenSpaceErrorPx(const View& view, TileKey key) const
{
  const Camera& camera = *view.camera;
  const double tileSizePx = std::max(m_settings.tileSizePx, 1);
  // One texel of the tile on the plane it is drawn on, metres: along a
  // meridian or a parallel, whichever the projection stretches more
  // (meridians on Mercator, parallels near the poles of Gall–Peters).
  double mapTexelM = 0.0;
  if (view.morph > 0.0) {
    const GeoRect rect = key.bounds();
    const Projection& projection = *view.frame.projection;
    double heightM = 0.0;
    if (projection.repeats()) {
      const double tallM = projection.forward(rect.northDeg, 0.0).yM -
                           projection.forward(rect.southDeg, 0.0).yM;
      const double halfWidthDeg = 0.5 * rect.widthDeg();
      const double wideM = projection.forward(0.0, halfWidthDeg).xM -
                           projection.forward(0.0, -halfWidthDeg).xM;
      heightM = std::max(tallM, wideM);
    } else {
      // The tile's part in the domain, as large as it is drawn; outside
      // it the tile is squashed onto the edge and needs no detail.
      const std::optional<MapRect> extent = tileMapExtent(view.frame, key);
      if (!extent) {
        return 0.0;
      }
      heightM = std::max(extent->widthM(), extent->heightM());
    }
    if (view.morph >= 1.0) {
      // Looking straight down at the plane every texel of the map is the
      // same size on screen, wherever it is.
      return heightM / (2.0 * view.frame.halfHeightM) *
             camera.viewportHeightPx() / tileSizePx;
    }
    mapTexelM = heightM * view.frame.planeScale / tileSizePx;
  }
  // One texel of the tile on the globe, in metres along a meridian (the
  // largest extent: east-west texels shrink towards the poles).
  const double globeTexelM = wgs84::semiMajorAxisM *
                             degToRad(TileKey::sizeDeg(key.level)) / tileSizePx;
  const TileBounds bounds = tileBounds(key);
  const double pixelsPerRadian =
      camera.viewportHeightPx() / (2.0 * camera.tanHalfFovY());
  if (view.morph <= 0.0) {
    const double distanceM = std::max(
        distance(bounds.centreEcef, camera.eyeEcef()) - bounds.radiusM, 1e-3);
    return globeTexelM * pixelsPerRadian / distanceM;
  }
  // Changing view: a texel where the tile is drawn now. Its size there is
  // at most the mix of its sizes on the globe and on the map, and it is
  // no nearer than the nearest point of the tile's sphere. The distance
  // goes over from straight-line (as on the globe) to depth along the view
  // (exact for the map, seen straight down) as the map takes over.
  const double t = view.morph;
  const double texelM = (1.0 - t) * globeTexelM + t * mapTexelM;
  double nearestM = std::numeric_limits<double>::max();
  for (const MorphSphere& sphere :
       morphSpheres(camera, view.frame, key, bounds, view.terrainHeightM)) {
    const double depthM = dot(sphere.offsetM, camera.forwardEcef());
    const double centreM = (1.0 - t) * length(sphere.offsetM) + t * depthM;
    nearestM = std::min(nearestM, centreM - sphere.radiusM);
  }
  return texelM * pixelsPerRadian / std::max(nearestM, 1e-3);
}

double TileSelector::mapGeometryErrorPx(const Camera& camera, TileKey key) const
{
  const MapFrame frame = camera.mapFrame();
  const Projection& projection = *frame.projection;
  if (!projection.isCylindrical()) {
    return generalMapGeometryErrorPx(camera, frame, key,
                                     m_settings.meshSegments);
  }
  const GeoRect rect = key.bounds();
  const double southDeg = std::max(rect.southDeg, projection.minimumLatDeg());
  const double northDeg = std::min(rect.northDeg, projection.maximumLatDeg());
  if (!(southDeg < northDeg)) {
    return 0.0;
  }
  // The grid row nearest a pole, where cylindrical projections curve most.
  const double rowDeg = rect.heightDeg() / std::max(m_settings.meshSegments, 1);
  const bool northern = std::abs(northDeg) >= std::abs(southDeg);
  const double outerDeg = northern ? northDeg : southDeg;
  const double innerDeg = northern ? std::max(northDeg - rowDeg, southDeg)
                                   : std::min(southDeg + rowDeg, northDeg);
  const double outerM = projection.forward(outerDeg, 0.0).yM;
  const double innerM = projection.forward(innerDeg, 0.0).yM;
  const double middleM =
      projection.forward(0.5 * (outerDeg + innerDeg), 0.0).yM;
  const double errorM = std::abs(middleM - 0.5 * (outerM + innerM));
  return errorM / (2.0 * frame.halfHeightM) * camera.viewportHeightPx();
}

TileSelection TileSelector::select(
    const Camera& camera,
    const std::function<bool(const TileKey&)>& isRenderable) const
{
  TileSelection selection;
  View view(camera);
  view.terrainHeightM = m_settings.terrainHeightM;
  for (const TileKey& root : TileKey::roots()) {
    visit(view, root, isRenderable, selection);
  }
  std::sort(selection.load.begin(), selection.load.end(),
            [](const TileLoadRequest& a, const TileLoadRequest& b) {
              if (a.key.level != b.key.level) {
                return a.key.level < b.key.level;
              }
              return a.distanceM < b.distanceM;
            });
  return selection;
}

void TileSelector::visit(
    const View& view, TileKey key,
    const std::function<bool(const TileKey&)>& isRenderable,
    TileSelection& selection) const
{
  ++selection.visited;
  const TileSamples samples = tileSamples(key);
  const TileBounds bounds = boundsFromSamples(key, samples);
  if (!view.isVisible(key, samples, bounds)) {
    return;
  }
  const Camera& camera = *view.camera;
  const bool renderable = isRenderable(key);
  if (wantsRefinement(view, key) &&
      refine(view, key, renderable, isRenderable, selection)) {
    return;
  }
  if (renderable) {
    selection.render.push_back(key);
  } else {
    selection.load.push_back(TileLoadRequest{
        key,
        std::max(distance(bounds.centreEcef, camera.eyeEcef()) - bounds.radiusM,
                 0.0)});
  }
}

bool TileSelector::wantsRefinement(const View& view, TileKey key) const
{
  if (key.level >= TileKey::maximumLevel) {
    return false;
  }
  // The map's geometry needs its coarsest level even where the data ends
  // above it.
  if (view.morph > 0.0 && key.level < m_settings.minimumMapLevel) {
    return true;
  }
  if (view.morph >= 1.0 && mapGeometryErrorPx(*view.camera, key) >
                               m_settings.maximumMapGeometryErrorPx) {
    return true;
  }
  return key.level < m_settings.maximumLevel &&
         screenSpaceErrorPx(view, key) > m_settings.maximumScreenSpaceErrorPx;
}

bool TileSelector::refine(
    const View& view, TileKey key, bool renderable,
    const std::function<bool(const TileKey&)>& isRenderable,
    TileSelection& selection) const
{
  std::vector<TileKey> children;
  for (int32_t i = 0; i < 4; ++i) {
    const TileKey child = key.child(i);
    if (view.isVisible(child)) {
      children.push_back(child);
    }
  }
  const bool allReady = std::all_of(
      children.begin(), children.end(),
      [&isRenderable](TileKey child) { return isRenderable(child); });
  if (allReady) {
    // The children replace this tile.
    for (const TileKey& child : children) {
      visit(view, child, isRenderable, selection);
    }
    return true;
  }
  for (const TileKey& child : children) {
    if (renderable && !isRenderable(child)) {
      // This tile stays meanwhile; fetch the missing children.
      selection.load.push_back(
          TileLoadRequest{child, distance(tileBounds(child).centreEcef,
                                          view.camera->eyeEcef())});
    } else if (!renderable && isRenderable(child)) {
      // This tile can't be drawn yet: use what is there below it. The
      // others wait until this tile has loaded (coarse first).
      visit(view, child, isRenderable, selection);
    }
  }
  return false;
}

} // namespace ts
