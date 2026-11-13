// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/camera/camera.h"

#include <algorithm>
#include <cmath>

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
  const Vec3 horizontal =
      local.eastEcef * std::sin(heading) + local.northEcef * std::cos(heading);
  const Vec3 towardsEye =
      local.upEcef * std::cos(tilt) - horizontal * std::sin(tilt);
  PoseFrame frame;
  frame.target =
      geodeticToEcef(Geodetic{pose.targetLatDeg, pose.targetLonDeg, 0.0});
  frame.eye = frame.target + towardsEye * pose.rangeM;
  frame.forward = -towardsEye;
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
 * for any tilt below 90°, so bisection finds it.
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
  for (int i = 0; i < 64; ++i) {
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
  double dx = std::fmod(point.xM - centreXM, worldWidthM);
  if (dx >= 0.5 * worldWidthM) {
    dx -= worldWidthM;
  } else if (dx < -0.5 * worldWidthM) {
    dx += worldWidthM;
  }
  return MapPoint{dx, point.yM - centreYM};
}

MapPoint MapFrame::offsetOf(double latDeg, double lonDeg) const
{
  return offsetOf(projection->forward(latDeg, lonDeg));
}

std::vector<double> MapFrame::copyShiftsM(double westDeg, double eastDeg,
                                          bool wholeWorld) const
{
  const double middleXM =
      offsetOf(projection->forward(centreLatDeg, 0.5 * (westDeg + eastDeg))).xM;
  const double halfSpanM = 0.5 * (eastDeg - westDeg) / 360.0 * worldWidthM;
  const double viewM =
      wholeWorld ? 0.5 * worldWidthM : std::min(halfWidthM, 0.5 * worldWidthM);
  std::vector<double> shifts;
  for (const double shift : {0.0, -worldWidthM, worldWidthM}) {
    if (middleXM + shift - halfSpanM < viewM &&
        middleXM + shift + halfSpanM > -viewM) {
      shifts.push_back(shift);
    }
  }
  return shifts;
}

Camera::Camera() : m_projection(projections().front())
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
    m_projection = projection;
    setPose(m_pose);
  }
}

double Camera::maximumMapRangeM() const
{
  return mapRangeLimitM(m_pose.targetLatDeg);
}

double Camera::mapRangeLimitM(double latDeg) const
{
  const double planeScale = 1.0 / m_projection->scaleAt(latDeg).parallel;
  return std::min(maximumRangeM, 0.5 * m_projection->bounds().widthM() *
                                     planeScale / tanHalfFovX());
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
  frame.planeScale = 1.0 / m_projection->scaleAt(m_pose.targetLatDeg).parallel;
  frame.halfWidthM = m_pose.rangeM * tanHalfFovX() / frame.planeScale;
  frame.halfHeightM = m_pose.rangeM * tanHalfFovY() / frame.planeScale;
  frame.worldWidthM = m_projection->bounds().widthM();
  return frame;
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
  if (m_morph >= 1.0) {
    // The flat map: straight down, north up, inside the map, and never
    // wider than the world.
    const double latLimit =
        std::min({m_projection->maximumLatDeg(), -m_projection->minimumLatDeg(),
                  maximumMapCentreLatDeg});
    clamped.targetLatDeg =
        std::clamp(clamped.targetLatDeg, -latLimit, latLimit);
    clamped.headingDeg = 0.0;
    clamped.tiltDeg = 0.0;
    clamped.rangeM =
        std::min(clamped.rangeM, mapRangeLimitM(clamped.targetLatDeg));
  }
  if (!(clamped.rangeM > 0.0) || eyeHeightM(clamped) < minimumEyeHeightM) {
    clamped.rangeM = rangeForEyeHeight(clamped, minimumEyeHeightM);
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
  if (m_morph >= 1.0) {
    // The whole map is range metres in front of the eye.
    m_nearM = 0.5 * m_pose.rangeM;
    m_farM = 2.0 * m_pose.rangeM;
  } else if (m_morph > 0.0) {
    m_farM = std::max(m_farM, 2.0 * m_pose.rangeM) + morphFarMarginM;
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
      1.0 / m_projection->scaleAt(m_pose.targetLatDeg).parallel;
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
  for (int i = 0; i < 4000 && t < m_farM; ++i) {
    if (height <= 0.0) {
      // Bisect between the last point above and the first below.
      double low = previous;
      double high = t;
      for (int j = 0; j < 40; ++j) {
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
    const Ray ray = rayThroughPixel(xPx, yPx);
    const Vec3 toTarget = m_targetEcef - m_eyeEcef;
    const double towards = dot(ray.direction, m_local.upEcef);
    if (!(towards < 0.0)) {
      return std::nullopt;
    }
    const double distanceM = dot(toTarget, m_local.upEcef) / towards;
    const Vec3 onPlane = ray.direction * distanceM - toTarget;
    const MapFrame frame = mapFrame();
    const MapRect bounds = m_projection->bounds();
    double xM =
        frame.centreXM + dot(onPlane, m_local.eastEcef) / frame.planeScale;
    const double yM =
        frame.centreYM + dot(onPlane, m_local.northEcef) / frame.planeScale;
    // The map repeats in x.
    xM = bounds.westM +
         std::fmod(std::fmod(xM - bounds.westM, frame.worldWidthM) +
                       frame.worldWidthM,
                   frame.worldWidthM);
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
