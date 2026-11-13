// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/tiling/tileselector.h"

#include <algorithm>
#include <array>
#include <cmath>

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
  for (int row = 0; row < 3; ++row) {
    const double lat = rect.northDeg - 0.5 * row * rect.heightDeg();
    for (int column = 0; column < 3; ++column) {
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
      if (morph < 1.0) {
        // The view may still be turned: take its diagonal both ways.
        halfWidthM = std::hypot(frame.halfWidthM, frame.halfHeightM);
        halfHeightM = halfWidthM;
      }
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
    if (rectOnMap(frame, key, halfWidthM, halfHeightM)) {
      return true;
    }
    // Changing view: also what the globe shows, the far side included (it
    // unrolls into view).
    return morph < 1.0 && sphereInFrustum(*camera, raised);
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
  double mapErrorPx = 0.0;
  if (camera.morph() > 0.0) {
    // One texel of the tile along a meridian of the map, where the
    // projection stretches most.
    const MapFrame frame = camera.mapFrame();
    const GeoRect rect = key.bounds();
    const double heightM = frame.projection->forward(rect.northDeg, 0.0).yM -
                           frame.projection->forward(rect.southDeg, 0.0).yM;
    mapErrorPx = heightM / (2.0 * frame.halfHeightM) *
                 camera.viewportHeightPx() / std::max(m_settings.tileSizePx, 1);
    if (camera.morph() >= 1.0) {
      return mapErrorPx;
    }
  }
  // One texel of the tile, in metres along a meridian (the largest
  // extent: east-west texels shrink towards the poles).
  const double texelM = wgs84::semiMajorAxisM *
                        degToRad(TileKey::sizeDeg(key.level)) /
                        std::max(m_settings.tileSizePx, 1);
  const TileBounds bounds = tileBounds(key);
  const double distanceM = std::max(
      distance(bounds.centreEcef, camera.eyeEcef()) - bounds.radiusM, 1e-3);
  return std::max(mapErrorPx, texelM * camera.viewportHeightPx() /
                                  (2.0 * distanceM * camera.tanHalfFovY()));
}

double TileSelector::mapGeometryErrorPx(const Camera& camera, TileKey key) const
{
  const MapFrame frame = camera.mapFrame();
  const Projection& projection = *frame.projection;
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
         screenSpaceErrorPx(*view.camera, key) >
             m_settings.maximumScreenSpaceErrorPx;
}

bool TileSelector::refine(
    const View& view, TileKey key, bool renderable,
    const std::function<bool(const TileKey&)>& isRenderable,
    TileSelection& selection) const
{
  std::vector<TileKey> children;
  for (int i = 0; i < 4; ++i) {
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
