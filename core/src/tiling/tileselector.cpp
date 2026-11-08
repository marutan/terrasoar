// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/tiling/tileselector.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ts/camera/camera.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
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
                          const TileSamples& samples)
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
  const double margin = length(v) * sampleSpacingRad(key) * 1.01;
  return std::all_of(samples.begin(), samples.end(),
                     [&v, margin, a, b](const Vec3& sample) {
                       const Vec3 q{sample.x / a, sample.y / a, sample.z / b};
                       return dot(q, v) < 1.0 - margin;
                     });
}

} // namespace

TileBounds tileBounds(TileKey key)
{
  return boundsFromSamples(key, tileSamples(key));
}

TileSelector::TileSelector(TileSelectionSettings settings)
    : m_settings(settings)
{
}

void TileSelector::setSettings(TileSelectionSettings settings)
{
  m_settings = settings;
}

bool TileSelector::isInFrustum(const Camera& camera, TileKey key)
{
  return sphereInFrustum(camera, tileBounds(key));
}

bool TileSelector::isBehindHorizon(const Camera& camera, TileKey key)
{
  return samplesBehindHorizon(camera, key, tileSamples(key));
}

bool TileSelector::isVisible(const Camera& camera, TileKey key)
{
  const TileSamples samples = tileSamples(key);
  return sphereInFrustum(camera, boundsFromSamples(key, samples)) &&
         !samplesBehindHorizon(camera, key, samples);
}

double TileSelector::screenSpaceErrorPx(const Camera& camera, TileKey key) const
{
  // One texel of the tile, in metres along a meridian (the largest
  // extent: east-west texels shrink towards the poles).
  const double texelM = wgs84::semiMajorAxisM *
                        degToRad(TileKey::sizeDeg(key.level)) /
                        std::max(m_settings.tileSizePx, 1);
  const TileBounds bounds = tileBounds(key);
  const double distanceM = std::max(
      distance(bounds.centreEcef, camera.eyeEcef()) - bounds.radiusM, 1e-3);
  return texelM * camera.viewportHeightPx() /
         (2.0 * distanceM * camera.tanHalfFovY());
}

TileSelection TileSelector::select(
    const Camera& camera,
    const std::function<bool(const TileKey&)>& isRenderable) const
{
  TileSelection selection;
  for (const TileKey& root : TileKey::roots()) {
    visit(camera, root, isRenderable, selection);
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
    const Camera& camera, TileKey key,
    const std::function<bool(const TileKey&)>& isRenderable,
    TileSelection& selection) const
{
  ++selection.visited;
  const TileSamples samples = tileSamples(key);
  const TileBounds bounds = boundsFromSamples(key, samples);
  if (!sphereInFrustum(camera, bounds) ||
      samplesBehindHorizon(camera, key, samples)) {
    return;
  }
  const bool renderable = isRenderable(key);
  if (wantsRefinement(camera, key) &&
      refine(camera, key, renderable, isRenderable, selection)) {
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

bool TileSelector::wantsRefinement(const Camera& camera, TileKey key) const
{
  return key.level < std::min(m_settings.maximumLevel, TileKey::maximumLevel) &&
         screenSpaceErrorPx(camera, key) > m_settings.maximumScreenSpaceErrorPx;
}

bool TileSelector::refine(
    const Camera& camera, TileKey key, bool renderable,
    const std::function<bool(const TileKey&)>& isRenderable,
    TileSelection& selection) const
{
  std::vector<TileKey> children;
  for (int i = 0; i < 4; ++i) {
    const TileKey child = key.child(i);
    if (isVisible(camera, child)) {
      children.push_back(child);
    }
  }
  const bool allReady = std::all_of(
      children.begin(), children.end(),
      [&isRenderable](TileKey child) { return isRenderable(child); });
  if (allReady) {
    // The children replace this tile.
    for (const TileKey& child : children) {
      visit(camera, child, isRenderable, selection);
    }
    return true;
  }
  for (const TileKey& child : children) {
    if (renderable && !isRenderable(child)) {
      // This tile stays meanwhile; fetch the missing children.
      selection.load.push_back(TileLoadRequest{
          child, distance(tileBounds(child).centreEcef, camera.eyeEcef())});
    } else if (!renderable && isRenderable(child)) {
      // This tile can't be drawn yet: use what is there below it. The
      // others wait until this tile has loaded (coarse first).
      visit(camera, child, isRenderable, selection);
    }
  }
  return false;
}

} // namespace ts
