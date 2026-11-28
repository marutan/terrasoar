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

#include "ts/camera/camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "ts/geo/ellipsoid.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/projection.h"

namespace ts
{

namespace
{

/** Pole limit for the target: the heading is undefined at a pole itself. */
constexpr double maximumTargetLatDeg = 89.999999;

/** Extra far distance beyond the horizon, metres (room for terrain later). */
constexpr double farMarginM = 5.0e4;

/** Extra far distance while the map unrolls (the far edges swing out). */
constexpr double morphFarMarginM = 5.0e7;

/**
 * The far plane looking up past Camera::orbitTiltDeg, metres: beyond
 * geostationary orbit (42,164 km from the centre) seen from anywhere.
 */
constexpr double lookingUpFarM = 1.0e8;

/** Eye-relative camera axes for a pose. */
struct PoseFrame {
  Vec3 target;
  Vec3 eye;
  Vec3 forward;
  Vec3 up;
};

PoseFrame poseFrame(const CameraPose& pose)
{
  const LocalFrame local = localFrame(pose.targetLatDeg, pose.targetLonDeg);
  const double heading = degToRad(pose.headingDeg);
  const double tilt = degToRad(pose.tiltDeg);
  // The eye circles the target up to orbitTiltDeg and stays there beyond,
  // where only the view turns on up (Camera::maximumTiltDeg).
  const double orbit = degToRad(std::min(pose.tiltDeg, Camera::orbitTiltDeg));
  const Vec3 horizontal =
      local.eastEcef * std::sin(heading) + local.northEcef * std::cos(heading);
  const Vec3 towardsEye =
      local.upEcef * std::cos(orbit) - horizontal * std::sin(orbit);
  PoseFrame frame;
  frame.target =
      geodeticToEcef(Geodetic{pose.targetLatDeg, pose.targetLonDeg, 0.0});
  frame.eye = frame.target + towardsEye * pose.rangeM;
  frame.forward = horizontal * std::sin(tilt) - local.upEcef * std::cos(tilt);
  frame.up = horizontal * std::cos(tilt) + local.upEcef * std::sin(tilt);
  return frame;
}

double eyeHeightM(const CameraPose& pose)
{
  return ecefToGeodetic(eyeEcefForPose(pose)).heightM;
}

/**
 * Returns the range at which the eye is at @p heightM above the ellipsoid
 * for @p pose's target, heading and tilt. The height grows with the range
 * for any tilt below 90°, and the eye never goes past orbitTiltDeg, so
 * bisection finds it.
 */
double rangeForEyeHeight(CameraPose pose, double heightM)
{
  double low = 0.0;
  double high = std::max(heightM, 1.0);
  pose.rangeM = high;
  while (eyeHeightM(pose) < heightM && high < Camera::maximumRangeM) {
    high *= 2.0;
    pose.rangeM = high;
  }
  for (int32_t i = 0; i < 64; ++i) {
    pose.rangeM = 0.5 * (low + high);
    if (eyeHeightM(pose) < heightM) {
      low = pose.rangeM;
    } else {
      high = pose.rangeM;
    }
  }
  return high;
}

double normaliseHeadingDeg(double headingDeg)
{
  double result = std::fmod(headingDeg, 360.0);
  if (result < 0.0) {
    result += 360.0;
  }
  // fmod of a tiny negative number plus 360 can round to 360 itself.
  return result >= 360.0 ? 0.0 : result;
}

} // namespace

Vec3 eyeEcefForPose(const CameraPose& pose)
{
  return poseFrame(pose).eye;
}

MapPoint MapFrame::offsetOf(MapPoint point) const
{
  if (!(worldWidthM > 0.0)) {
    // A map that doesn't repeat: there is only one copy.
    return MapPoint{point.xM - centreXM, point.yM - centreYM};
  }
  double dx = std::fmod(point.xM - centreXM, worldWidthM);
  if (dx >= 0.5 * worldWidthM) {
    dx -= worldWidthM;
  } else if (dx < -0.5 * worldWidthM) {
    dx += worldWidthM;
  }
  return MapPoint{dx, point.yM - centreYM};
}

double MapFrame::distanceToEyeM(const MapRect& offsets) const
{
  const double dx =
      std::max({offsets.westM - eyeFootM.xM, 0.0, eyeFootM.xM - offsets.eastM});
  const double dy = std::max(
      {offsets.southM - eyeFootM.yM, 0.0, eyeFootM.yM - offsets.northM});
  return std::hypot(planeScale * std::hypot(dx, dy), eyeHeightAbovePlaneM);
}

bool MapFrame::rectInView(const MapRect& offsets) const
{
  if (footprint.empty()) {
    return offsets.westM < halfWidthM && offsets.eastM > -halfWidthM &&
           offsets.southM < halfHeightM && offsets.northM > -halfHeightM;
  }
  // Separating axes: the rectangle's (x and y), then the polygon's edges.
  double minX = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double minY = minX;
  double maxY = maxX;
  for (const MapPoint& p : footprint) {
    minX = std::min(minX, p.xM);
    maxX = std::max(maxX, p.xM);
    minY = std::min(minY, p.yM);
    maxY = std::max(maxY, p.yM);
  }
  if (offsets.westM >= maxX || offsets.eastM <= minX ||
      offsets.southM >= maxY || offsets.northM <= minY) {
    return false;
  }
  const std::size_t n = footprint.size();
  for (std::size_t i = 0; i < n; ++i) {
    const MapPoint& a = footprint.at(i);
    const MapPoint& b = footprint.at((i + 1) % n);
    // The outward normal of an anticlockwise polygon's edge.
    const double nx = b.yM - a.yM;
    const double ny = a.xM - b.xM;
    const double limit = nx * a.xM + ny * a.yM;
    // The rectangle's corner farthest inwards along the normal.
    const double inward = std::min(nx * offsets.westM, nx * offsets.eastM) +
                          std::min(ny * offsets.southM, ny * offsets.northM);
    if (inward >= limit) {
      return false;
    }
  }
  return true;
}

MapPoint MapFrame::offsetOf(double latDeg, double lonDeg) const
{
  return offsetOf(projection->forward(latDeg, lonDeg));
}

std::vector<double> MapFrame::copyShiftsM(double westDeg, double eastDeg,
                                          bool wholeWorld) const
{
  if (!(worldWidthM > 0.0)) {
    return {0.0}; // one copy; whether it is in view is the caller's test
  }
  const double middleXM =
      offsetOf(projection->forward(centreLatDeg, 0.5 * (westDeg + eastDeg))).xM;
  const double halfSpanM = 0.5 * (eastDeg - westDeg) / 360.0 * worldWidthM;
  const double viewM =
      wholeWorld ? 0.5 * worldWidthM : std::min(viewHalfXM, 0.5 * worldWidthM);
  std::vector<double> shifts;
  for (const double shift : {0.0, -worldWidthM, worldWidthM}) {
    if (middleXM + shift - halfSpanM < viewM &&
        middleXM + shift + halfSpanM > -viewM) {
      shifts.push_back(shift);
    }
  }
  return shifts;
}

Camera::Camera()
    : m_projection(projections().front()),
      m_requestedProjection(projections().front())
{
  update();
}

void Camera::setMorph(double morph)
{
  m_morph = std::clamp(morph, 0.0, 1.0);
  // Applies the map's limits at 1; otherwise only the clip planes change.
  setPose(m_pose);
}

void Camera::setMapProjection(const Projection* projection)
{
  if (projection != nullptr) {
    m_requestedProjection = projection;
    m_projection =
        projection->effectiveAt(m_pose.targetLatDeg, m_pose.targetLonDeg);
    setPose(m_pose);
  }
}

double Camera::maximumMapRangeM() const
{
  return mapRangeLimitM(m_pose.targetLatDeg, m_pose.targetLonDeg);
}

double Camera::mapRangeLimitM(double latDeg, double lonDeg) const
{
  const double planeScale =
      1.0 / m_projection->scaleAtPoint(latDeg, lonDeg).parallel;
  const MapRect bounds = m_projection->bounds();
  if (m_projection->repeats()) {
    // Never wider than the world.
    return std::min(maximumRangeM,
                    0.5 * bounds.widthM() * planeScale / tanHalfFovX());
  }
  // The whole map in view, and no further out.
  return std::min(maximumRangeM,
                  0.5 * planeScale *
                      std::max(bounds.widthM() / tanHalfFovX(),
                               bounds.heightM() / tanHalfFovY()));
}

MapFrame Camera::mapFrame() const
{
  MapFrame frame;
  frame.projection = m_projection;
  frame.centreLatDeg = m_pose.targetLatDeg;
  frame.centreLonDeg = m_pose.targetLonDeg;
  const MapPoint centre =
      m_projection->forward(m_pose.targetLatDeg, m_pose.targetLonDeg);
  frame.centreXM = centre.xM;
  frame.centreYM = centre.yM;
  frame.planeScale =
      1.0 / m_projection->scaleAtPoint(m_pose.targetLatDeg, m_pose.targetLonDeg)
                .parallel;
  frame.halfWidthM = m_pose.rangeM * tanHalfFovX() / frame.planeScale;
  frame.halfHeightM = m_pose.rangeM * tanHalfFovY() / frame.planeScale;
  frame.worldWidthM =
      m_projection->repeats() ? m_projection->bounds().widthM() : 0.0;
  frame.viewHalfXM = frame.halfWidthM;
  if (isMapTurnedOrTilted()) {
    addFootprint(frame);
  }
  return frame;
}

bool Camera::isMapTurnedOrTilted() const
{
  return m_morph >= 1.0 && (m_pose.headingDeg != 0.0 || m_pose.tiltDeg != 0.0);
}

double Camera::mapFarLimitM() const
{
  const double heightM = m_pose.rangeM * std::cos(degToRad(m_pose.tiltDeg));
  // The cap only shortens how far a tilted map reaches: never nearer than
  // a few ranges, or a map seen from further than the cap (zoomed out)
  // would be cut off whole, and its corners hazed.
  return std::min(
      std::max(mapFarEyeHeights * heightM, 2.0 * m_pose.rangeM),
      std::max(maximumMapFarM, mapFarMinimumRanges * m_pose.rangeM));
}

double Camera::mapReliefForTilt(double tiltDeg)
{
  const double u = std::clamp(tiltDeg / mapReliefFullTiltDeg, 0.0, 1.0);
  return u * u * (3.0 - 2.0 * u);
}

void Camera::addFootprint(MapFrame& frame) const
{
  // In map metres from the centre: a point (x, y) of the map is at
  // planeOrigin + E x s + N y s relative to the eye.
  const double s = frame.planeScale;
  const Vec3 origin = m_targetEcef - m_eyeEcef;
  const Vec3& east = m_local.eastEcef;
  const Vec3& north = m_local.northEcef;
  const double heightM = -dot(origin, m_local.upEcef);
  frame.eyeHeightAbovePlaneM = std::max(heightM, 0.0);
  frame.eyeFootM = MapPoint{-dot(origin, east) / s, -dot(origin, north) / s};
  // What is within the far limit: a polygon around the circle of that
  // radius on the plane (drawn outside it, so nothing is left out).
  const double farM = mapFarLimitM();
  const double radiusM =
      std::sqrt(std::max(farM * farM - heightM * heightM, 0.0)) / s;
  constexpr int32_t sides = 24;
  const double outer = radiusM / std::cos(pi / sides);
  std::vector<MapPoint> polygon;
  polygon.reserve(sides);
  for (int32_t i = 0; i < sides; ++i) {
    const double angle = 2.0 * pi * i / sides;
    polygon.push_back(MapPoint{frame.eyeFootM.xM + outer * std::cos(angle),
                               frame.eyeFootM.yM + outer * std::sin(angle)});
  }
  // Cut by the four sides of the view: a point p (from the eye) is inside
  // where dot(p, n) >= 0 for each side's inward normal n.
  const double tx = tanHalfFovX();
  const double ty = tanHalfFovY();
  const std::array<Vec3, 4> normals = {
      m_right + m_forward * tx, m_forward * tx - m_right, m_up + m_forward * ty,
      m_forward * ty - m_up};
  for (const Vec3& n : normals) {
    // dot(p, n) = c + a x + b y on the plane.
    const double c = dot(origin, n);
    const double a = s * dot(east, n);
    const double b = s * dot(north, n);
    std::vector<MapPoint> cut;
    const std::size_t count = polygon.size();
    for (std::size_t i = 0; i < count; ++i) {
      const MapPoint& p = polygon.at(i);
      const MapPoint& q = polygon.at((i + 1) % count);
      const double fp = c + a * p.xM + b * p.yM;
      const double fq = c + a * q.xM + b * q.yM;
      if (fp >= 0.0) {
        cut.push_back(p);
      }
      if ((fp >= 0.0) != (fq >= 0.0)) {
        const double u = fp / (fp - fq);
        cut.push_back(
            MapPoint{p.xM + u * (q.xM - p.xM), p.yM + u * (q.yM - p.yM)});
      }
    }
    polygon = std::move(cut);
  }
  if (polygon.size() < 3) {
    // Nothing of the map in view (looking above it): a point no tile
    // meets.
    polygon = {MapPoint{frame.eyeFootM.xM, frame.eyeFootM.yM},
               MapPoint{frame.eyeFootM.xM, frame.eyeFootM.yM},
               MapPoint{frame.eyeFootM.xM, frame.eyeFootM.yM}};
  }
  double halfXM = 0.0;
  for (const MapPoint& p : polygon) {
    halfXM = std::max(halfXM, std::abs(p.xM));
  }
  frame.viewHalfXM = halfXM;
  frame.footprint = std::move(polygon);
}

void Camera::setPose(const CameraPose& pose)
{
  CameraPose clamped = pose;
  clamped.targetLatDeg =
      std::clamp(pose.targetLatDeg, -maximumTargetLatDeg, maximumTargetLatDeg);
  clamped.targetLonDeg = normaliseLonDeg(pose.targetLonDeg);
  clamped.headingDeg = normaliseHeadingDeg(pose.headingDeg);
  clamped.tiltDeg = std::clamp(pose.tiltDeg, 0.0, maximumTiltDeg);
  clamped.rangeM = std::min(pose.rangeM, maximumRangeM);
  // The projection for the new centre (another ARC zone, for one): first,
  // as its limits hold the centre.
  m_projection = m_requestedProjection->effectiveAt(clamped.targetLatDeg,
                                                    clamped.targetLonDeg);
  if (m_morph >= 1.0) {
    // The flat map: straight down, north up, inside the map, and never
    // wider than the world.
    // The domain's latitudes, which needn't be symmetric (a cone's).
    clamped.targetLatDeg = std::clamp(
        clamped.targetLatDeg,
        std::max(m_projection->minimumLatDeg(), -maximumMapCentreLatDeg),
        std::min(m_projection->maximumLatDeg(), maximumMapCentreLatDeg));
    clamped.targetLonDeg =
        normaliseLonDeg(m_projection->clampLonDeg(clamped.targetLonDeg));
    if (m_projection->hasOwnDomain()) {
      // A domain of its own shape (Rotated Mercator): its nearest point.
      const Geodetic inside = m_projection->clampToDomain(clamped.targetLatDeg,
                                                          clamped.targetLonDeg);
      clamped.targetLatDeg = inside.latDeg;
      clamped.targetLonDeg = inside.lonDeg;
    }
    clamped.tiltDeg = std::min(clamped.tiltDeg, maximumMapTiltDeg);
    clamped.rangeM =
        std::min(clamped.rangeM,
                 mapRangeLimitM(clamped.targetLatDeg, clamped.targetLonDeg));
  }
  if (!(clamped.rangeM > 0.0) || eyeHeightM(clamped) < minimumEyeHeightM) {
    clamped.rangeM = rangeForEyeHeight(clamped, minimumEyeHeightM);
  }
  if (m_surfaceHeight && m_morph >= 1.0 && clamped.tiltDeg > 0.0) {
    // A tilted map shows the terrain in relief: the eye stays above the
    // ground below it, at the height it is drawn there.
    keepAboveMapRelief(clamped);
  }
  if (m_surfaceHeight && m_morph <= 0.0) {
    // Above the terrain, too: the range grows until the eye is clear.
    const Geodetic eye = ecefToGeodetic(eyeEcefForPose(clamped));
    const std::optional<double> ground =
        m_surfaceHeight(eye.latDeg, eye.lonDeg);
    if (ground && eye.heightM < *ground + terrainClearanceM) {
      clamped.rangeM = rangeForEyeHeight(clamped, *ground + terrainClearanceM);
    }
  }
  m_pose = clamped;
  update();
}

void Camera::keepAboveMapRelief(CameraPose& pose) const
{
  const double relief = mapReliefForTilt(pose.tiltDeg);
  const double cosTilt = std::cos(degToRad(pose.tiltDeg));
  if (!(relief > 0.0) || !(cosTilt > 0.0)) {
    return;
  }
  // The eye's foot on the plane, from the target: along the direction
  // opposite the heading, range sin(tilt) metres on the plane.
  const double scale =
      1.0 /
      m_projection->scaleAtPoint(pose.targetLatDeg, pose.targetLonDeg).parallel;
  const double backM = pose.rangeM * std::sin(degToRad(pose.tiltDeg)) / scale;
  const double heading = degToRad(pose.headingDeg);
  const MapPoint centre =
      m_projection->forward(pose.targetLatDeg, pose.targetLonDeg);
  const std::optional<Geodetic> below =
      m_projection->inverse(MapPoint{centre.xM - backM * std::sin(heading),
                                     centre.yM - backM * std::cos(heading)});
  if (!below) {
    return;
  }
  const std::optional<double> ground =
      m_surfaceHeight(below->latDeg, below->lonDeg);
  if (!ground) {
    return;
  }
  const double neededM = (std::max(*ground, 0.0) * relief + terrainClearanceM);
  if (pose.rangeM * cosTilt < neededM) {
    pose.rangeM = neededM / cosTilt;
  }
}

void Camera::setViewportSize(double widthPx, double heightPx)
{
  if (widthPx > 0.0 && heightPx > 0.0) {
    m_widthPx = widthPx;
    m_heightPx = heightPx;
    if (m_morph >= 1.0) {
      setPose(m_pose); // the map's range limit follows the shape
    }
  }
}

void Camera::setVerticalFovDeg(double fovDeg)
{
  m_fovYDeg = std::clamp(fovDeg, 1.0, 120.0);
  if (m_morph >= 1.0) {
    setPose(m_pose);
  }
}

double Camera::tanHalfFovY() const
{
  return std::tan(degToRad(m_fovYDeg) / 2.0);
}

double Camera::tanHalfFovX() const
{
  return tanHalfFovY() * m_widthPx / m_heightPx;
}

void Camera::update()
{
  const PoseFrame frame = poseFrame(m_pose);
  m_targetEcef = frame.target;
  m_eyeEcef = frame.eye;
  m_forward = frame.forward;
  m_up = frame.up;
  m_right = cross(m_forward, m_up);
  m_eyeGeodetic = ecefToGeodetic(m_eyeEcef);
  m_local = localFrame(m_pose.targetLatDeg, m_pose.targetLonDeg);

  // Everything visible on the ellipsoid lies within the distance to the
  // horizon of the inner (polar-radius) sphere.
  const double b = wgs84::semiMinorAxisM;
  const double horizonM =
      std::sqrt(std::max(lengthSquared(m_eyeEcef) - b * b, 0.0));
  m_farM = std::max(horizonM + farMarginM, m_pose.rangeM * 1.01 + 1.0);
  const double heightM = std::max(m_eyeGeodetic.heightM, 0.0);
  m_nearM = std::clamp(0.5 * heightM, 0.05, 0.5 * m_pose.rangeM);
  if (isMapTurnedOrTilted()) {
    // As far as the map is drawn, and as near as its bottom edge can be.
    const double aboveM = m_pose.rangeM * std::cos(degToRad(m_pose.tiltDeg));
    m_nearM = std::clamp(0.25 * aboveM, 0.05, 0.5 * m_pose.rangeM);
    m_farM = 1.01 * mapFarLimitM() + 1.0;
  } else if (m_morph >= 1.0) {
    // The whole map is range metres in front of the eye.
    m_nearM = 0.5 * m_pose.rangeM;
    m_farM = 2.0 * m_pose.rangeM;
  } else if (m_morph > 0.0) {
    m_farM = std::max(m_farM, 2.0 * m_pose.rangeM) + morphFarMarginM;
  } else if (m_pose.tiltDeg > orbitTiltDeg) {
    // Looking up: the sky beyond the horizon, out past geostationary
    // orbit (satellites). The depth is logarithmic, so this costs little
    // precision near the eye.
    m_farM = std::max(m_farM, lookingUpFarM);
  }
}

Mat4 Camera::viewRotation() const
{
  Mat4 view;
  const Vec3 back = -m_forward;
  view.set(0, 0, m_right.x);
  view.set(0, 1, m_right.y);
  view.set(0, 2, m_right.z);
  view.set(1, 0, m_up.x);
  view.set(1, 1, m_up.y);
  view.set(1, 2, m_up.z);
  view.set(2, 0, back.x);
  view.set(2, 1, back.y);
  view.set(2, 2, back.z);
  return view;
}

Mat4 Camera::projection() const
{
  const double f = 1.0 / tanHalfFovY();
  const double aspect = m_widthPx / m_heightPx;
  Mat4 result;
  result.set(0, 0, f / aspect);
  result.set(1, 1, f);
  result.set(2, 2, (m_farM + m_nearM) / (m_nearM - m_farM));
  result.set(2, 3, 2.0 * m_farM * m_nearM / (m_nearM - m_farM));
  result.set(3, 2, -1.0);
  result.set(3, 3, 0.0);
  return result;
}

Mat4 Camera::viewProjection() const
{
  return projection() * viewRotation();
}

Ray Camera::rayThroughPixel(double xPx, double yPx) const
{
  const double ndcX = 2.0 * xPx / m_widthPx - 1.0;
  const double ndcY = 1.0 - 2.0 * yPx / m_heightPx;
  const Vec3 direction = m_forward + m_right * (ndcX * tanHalfFovX()) +
                         m_up * (ndcY * tanHalfFovY());
  return Ray{m_eyeEcef, normalized(direction)};
}

Vec3 Camera::mapPlanePoint(MapPoint offset) const
{
  const double scale =
      1.0 / m_projection->scaleAtPoint(m_pose.targetLatDeg, m_pose.targetLonDeg)
                .parallel;
  return (m_targetEcef - m_eyeEcef) + m_local.eastEcef * (offset.xM * scale) +
         m_local.northEcef * (offset.yM * scale);
}

void Camera::setSurfaceHeight(SurfaceHeight surfaceHeight)
{
  m_surfaceHeight = std::move(surfaceHeight);
  setPose(m_pose);
}

std::optional<Geodetic> Camera::pickSurface(const Ray& ray) const
{
  if (!m_surfaceHeight) {
    return std::nullopt;
  }
  const Vec3 direction = normalized(ray.direction);
  const std::function<double(double)> above = [&](double t) {
    const Geodetic point = ecefToGeodetic(ray.origin + direction * t);
    return point.heightM -
           m_surfaceHeight(point.latDeg, point.lonDeg).value_or(0.0);
  };
  // March: the surface is at least as far along the ray as the height
  // above it (half that, for slopes), but at least a thousandth of the
  // way so far, so grazing rays progress.
  double previous = 0.0;
  double t = 0.0;
  double height = above(t);
  for (int32_t i = 0; i < 4000 && t < m_farM; ++i) {
    if (height <= 0.0) {
      // Bisect between the last point above and the first below.
      double low = previous;
      double high = t;
      for (int32_t j = 0; j < 40; ++j) {
        const double mid = 0.5 * (low + high);
        (above(mid) > 0.0 ? low : high) = mid;
      }
      Geodetic hit = ecefToGeodetic(ray.origin + direction * high);
      hit.heightM = m_surfaceHeight(hit.latDeg, hit.lonDeg).value_or(0.0);
      return hit;
    }
    // Climbing away above the highest terrain (with a wide margin): a miss.
    const Vec3 point = ray.origin + direction * t;
    if (height > 2.0e5 && dot(direction, normalized(point)) > 0.0) {
      return std::nullopt;
    }
    previous = t;
    t += std::max(0.5 * height, std::max(1.0e-3 * t, 0.5));
    height = above(t);
  }
  return std::nullopt;
}

std::optional<Geodetic> Camera::pickGeodetic(double xPx, double yPx) const
{
  if (m_surfaceHeight && m_morph <= 0.0) {
    return pickSurface(rayThroughPixel(xPx, yPx));
  }
  if (isMap()) {
    // The ray against the map's plane: through the target, facing up.
    const std::optional<MapPoint> offset = mapOffsetAt(xPx, yPx);
    if (!offset) {
      return std::nullopt;
    }
    const MapPoint centre =
        m_projection->forward(m_pose.targetLatDeg, m_pose.targetLonDeg);
    const MapRect bounds = m_projection->bounds();
    const double worldWidthM = m_projection->repeats() ? bounds.widthM() : 0.0;
    double xM = centre.xM + offset->xM;
    const double yM = centre.yM + offset->yM;
    if (worldWidthM > 0.0) {
      // The map repeats in x.
      xM = bounds.westM +
           std::fmod(std::fmod(xM - bounds.westM, worldWidthM) + worldWidthM,
                     worldWidthM);
    }
    return m_projection->inverse(MapPoint{xM, yM});
  }
  const std::optional<Vec3> hit =
      intersectEllipsoidPoint(rayThroughPixel(xPx, yPx));
  if (!hit) {
    return std::nullopt;
  }
  Geodetic position = ecefToGeodetic(*hit);
  // On the surface by construction; drop the rounding left in the height.
  position.heightM = 0.0;
  return position;
}

std::optional<MapPoint> Camera::mapOffsetAt(double xPx, double yPx) const
{
  const Ray ray = rayThroughPixel(xPx, yPx);
  const Vec3 toTarget = m_targetEcef - m_eyeEcef;
  const double towards = dot(ray.direction, m_local.upEcef);
  if (!(towards < 0.0)) {
    return std::nullopt;
  }
  const double distanceM = dot(toTarget, m_local.upEcef) / towards;
  if (isMapTurnedOrTilted() && distanceM > mapFarLimitM()) {
    return std::nullopt;
  }
  const Vec3 onPlane = ray.direction * distanceM - toTarget;
  const double scale =
      1.0 / m_projection->scaleAtPoint(m_pose.targetLatDeg, m_pose.targetLonDeg)
                .parallel;
  return MapPoint{dot(onPlane, m_local.eastEcef) / scale,
                  dot(onPlane, m_local.northEcef) / scale};
}

std::optional<ScreenPoint> Camera::project(const Vec3& pointEcef) const
{
  if (isMap()) {
    const Geodetic position = ecefToGeodetic(pointEcef);
    return projectGeodetic(position.latDeg, position.lonDeg);
  }
  return projectOffset(pointEcef - m_eyeEcef);
}

std::optional<ScreenPoint> Camera::projectGeodetic(double latDeg,
                                                   double lonDeg) const
{
  if (isMap()) {
    return projectOffset(mapPlanePoint(mapFrame().offsetOf(latDeg, lonDeg)));
  }
  return projectOffset(geodeticToEcef(Geodetic{latDeg, lonDeg, 0.0}) -
                       m_eyeEcef);
}

std::optional<ScreenPoint> Camera::projectOffset(const Vec3& offset) const
{
  const double depthM = dot(offset, m_forward);
  if (!(depthM > 0.0)) {
    return std::nullopt;
  }
  const double ndcX = dot(offset, m_right) / (depthM * tanHalfFovX());
  const double ndcY = dot(offset, m_up) / (depthM * tanHalfFovY());
  return ScreenPoint{(ndcX + 1.0) * 0.5 * m_widthPx,
                     (1.0 - ndcY) * 0.5 * m_heightPx, depthM};
}

} // namespace ts
