// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/camera/camera.h"

#include <algorithm>
#include <cmath>

#include "ts/geo/ellipsoid.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** Pole limit for the target: the heading is undefined at a pole itself. */
constexpr double maximumTargetLatDeg = 89.999999;

/** Extra far distance beyond the horizon, metres (room for terrain later). */
constexpr double farMarginM = 5.0e4;

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

Camera::Camera()
{
  update();
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
  if (!(clamped.rangeM > 0.0) || eyeHeightM(clamped) < minimumEyeHeightM) {
    clamped.rangeM = rangeForEyeHeight(clamped, minimumEyeHeightM);
  }
  m_pose = clamped;
  update();
}

void Camera::setViewportSize(double widthPx, double heightPx)
{
  if (widthPx > 0.0 && heightPx > 0.0) {
    m_widthPx = widthPx;
    m_heightPx = heightPx;
  }
}

void Camera::setVerticalFovDeg(double fovDeg)
{
  m_fovYDeg = std::clamp(fovDeg, 1.0, 120.0);
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

  // Everything visible on the ellipsoid lies within the distance to the
  // horizon of the inner (polar-radius) sphere.
  const double b = wgs84::semiMinorAxisM;
  const double horizonM =
      std::sqrt(std::max(lengthSquared(m_eyeEcef) - b * b, 0.0));
  m_farM = std::max(horizonM + farMarginM, m_pose.rangeM * 1.01 + 1.0);
  const double heightM = std::max(m_eyeGeodetic.heightM, 0.0);
  m_nearM = std::clamp(0.5 * heightM, 0.05, 0.5 * m_pose.rangeM);
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

std::optional<Geodetic> Camera::pickGeodetic(double xPx, double yPx) const
{
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
  const Vec3 offset = pointEcef - m_eyeEcef;
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
