// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/camera/cameracontroller.h"

#include <algorithm>
#include <cmath>

#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

// Held-key rates.
/** Pan speed as a fraction of the range per second. */
constexpr double panRangeFractionPerS = 0.5;
/** Natural log of the zoom factor per second (a factor of 2). */
constexpr double zoomLogRatePerS = 0.69314718055994531;
/** Heading rate, degrees per second. */
constexpr double turnDegPerS = 60.0;
/** Tilt rate, degrees per second. */
constexpr double tiltDegPerS = 30.0;

// Mouse.
/** Range factor per wheel step (one notch zooms in by 20 %). */
constexpr double wheelStepFactor = 0.8;
/** Heading change for a drag across the whole viewport width, degrees. */
constexpr double rotateDegPerViewportWidth = 180.0;
/** Tilt change for a drag across the whole viewport height, degrees. */
constexpr double tiltDegPerViewportHeight = 90.0;

// Inertia.
/** Decay rate k of exp(−k t), per second. */
constexpr double inertiaDecayPerS = 3.0;
/** Drag samples older than this before the last one are ignored, s. */
constexpr double velocityWindowS = 0.1;
/** A drag released after holding still this long has no inertia, s. */
constexpr double releaseStillS = 0.05;
/** Inertia stops below this speed, as a fraction of the range per second. */
constexpr double inertiaStopRangeFractionPerS = 0.01;

// Pan iteration.
constexpr int placeIterations = 12;
/** Converged when the cursor point is this close in degrees (≈ 0.1 µm). */
constexpr double placeToleranceDeg = 1e-12;

/** Returns 1 if @p flag is set in @p motion, else 0. */
double held(unsigned int motion, unsigned int flag)
{
  return (motion & flag) != 0U ? 1.0 : 0.0;
}

double smoothStep(double u)
{
  return u * u * (3.0 - 2.0 * u);
}

/** Returns @p deg brought into (−180, 180]. */
double signedAngleDeg(double deg)
{
  const double lon = normaliseLonDeg(deg);
  return lon == -180.0 ? 180.0 : lon;
}

} // namespace

CameraController::CameraController() = default;

void CameraController::applyPose(const CameraPose& pose)
{
  m_camera.setPose(pose);
}

void CameraController::setPose(const CameraPose& pose)
{
  stop();
  applyPose(pose);
}

void CameraController::setViewportSize(double widthPx, double heightPx)
{
  m_camera.setViewportSize(widthPx, heightPx);
}

bool CameraController::beginPan(QPointF positionPx, double timeS)
{
  const std::optional<Vec3> hit = intersectEllipsoidPoint(
      m_camera.rayThroughPixel(positionPx.x(), positionPx.y()));
  if (!hit) {
    return false;
  }
  stop();
  m_drag = Drag::Pan;
  m_grabEcef = *hit;
  m_lastPositionPx = positionPx;
  m_samples.clear();
  m_samples.push_back(DragSample{timeS, m_camera.pose().targetLatDeg,
                                 m_camera.pose().targetLonDeg});
  return true;
}

void CameraController::beginRotate(QPointF positionPx)
{
  stop();
  m_drag = Drag::Rotate;
  m_lastPositionPx = positionPx;
}

void CameraController::dragTo(QPointF positionPx, double timeS)
{
  if (m_drag == Drag::Pan) {
    placeUnderCursor(m_grabEcef, positionPx);
    m_samples.push_back(DragSample{timeS, m_camera.pose().targetLatDeg,
                                   m_camera.pose().targetLonDeg});
    while (m_samples.size() > 2 &&
           m_samples.back().timeS - m_samples.front().timeS > velocityWindowS) {
      m_samples.pop_front();
    }
  } else if (m_drag == Drag::Rotate) {
    const QPointF delta = positionPx - m_lastPositionPx;
    CameraPose pose = m_camera.pose();
    pose.headingDeg +=
        delta.x() * rotateDegPerViewportWidth / m_camera.viewportWidthPx();
    pose.tiltDeg -=
        delta.y() * tiltDegPerViewportHeight / m_camera.viewportHeightPx();
    applyPose(pose);
  }
  m_lastPositionPx = positionPx;
}

void CameraController::endDrag(double timeS)
{
  if (m_drag == Drag::Pan && m_samples.size() >= 2 &&
      timeS - m_samples.back().timeS <= releaseStillS) {
    const DragSample& first = m_samples.front();
    const DragSample& last = m_samples.back();
    const double spanS = last.timeS - first.timeS;
    if (spanS > 0.0) {
      const GeodesicInverse line =
          rhumbInverse(first.latDeg, first.lonDeg, last.latDeg, last.lonDeg);
      const double speed = line.distanceM / spanS;
      if (speed > inertiaStopRangeFractionPerS * m_camera.pose().rangeM) {
        m_inertia = Inertia{line.azimuth1Deg, speed, 0.0};
      }
    }
  }
  m_drag = Drag::None;
  m_samples.clear();
}

bool CameraController::placeUnderCursor(const Vec3& pointEcef,
                                        QPointF positionPx)
{
  // Move the target by the lat/lon difference between the grabbed point
  // and the point now under the cursor until they agree. The heading stays
  // fixed, so north keeps its direction on screen.
  const Geodetic grabbed = ecefToGeodetic(pointEcef);
  for (int i = 0; i < placeIterations; ++i) {
    const std::optional<Geodetic> hit =
        m_camera.pickGeodetic(positionPx.x(), positionPx.y());
    if (!hit) {
      return false;
    }
    const double dLat = grabbed.latDeg - hit->latDeg;
    const double dLon = normaliseLonDeg(grabbed.lonDeg - hit->lonDeg);
    if (std::abs(dLat) < placeToleranceDeg &&
        std::abs(dLon) * std::cos(degToRad(grabbed.latDeg)) <
            placeToleranceDeg) {
      return true;
    }
    CameraPose pose = m_camera.pose();
    pose.targetLatDeg += dLat;
    pose.targetLonDeg += dLon;
    applyPose(pose);
  }
  return true;
}

void CameraController::zoomAt(QPointF positionPx, double steps)
{
  m_inertia.reset();
  m_flight.reset();
  const std::optional<Vec3> hit = intersectEllipsoidPoint(
      m_camera.rayThroughPixel(positionPx.x(), positionPx.y()));
  CameraPose pose = m_camera.pose();
  pose.rangeM *= std::pow(wheelStepFactor, steps);
  applyPose(pose);
  if (hit) {
    placeUnderCursor(*hit, positionPx);
  }
}

void CameraController::setMotion(unsigned int motion)
{
  if (motion != NoMotion) {
    m_flight.reset();
  }
  m_motion = motion;
}

CameraPose CameraController::poseLookingAt(const Geodetic& target,
                                           double rangeM) const
{
  CameraPose pose = m_camera.pose();
  pose.targetLatDeg = target.latDeg;
  pose.targetLonDeg = target.lonDeg;
  pose.rangeM = rangeM;
  return pose;
}

double CameraController::flightDurationS(const CameraPose& pose) const
{
  const CameraPose& from = m_camera.pose();
  const double distanceM = geodesicInverse(from.targetLatDeg, from.targetLonDeg,
                                           pose.targetLatDeg, pose.targetLonDeg)
                               .distanceM;
  const double zoomDecades =
      std::abs(std::log10(std::max(pose.rangeM, 1.0) / from.rangeM));
  return std::clamp(1.0 + 0.5 * std::log10(1.0 + distanceM / 1000.0) +
                        0.3 * zoomDecades,
                    1.0, 4.0);
}

void CameraController::flyTo(const CameraPose& pose, double durationS)
{
  stop();
  // Clamp the destination the way the camera will.
  Camera destination = m_camera;
  destination.setPose(pose);
  Flight flight;
  flight.from = m_camera.pose();
  flight.to = destination.pose();
  const GeodesicInverse line =
      geodesicInverse(flight.from.targetLatDeg, flight.from.targetLonDeg,
                      flight.to.targetLatDeg, flight.to.targetLonDeg);
  flight.distanceM = line.distanceM;
  flight.azimuthDeg = line.azimuth1Deg;
  // Rise to see both ends on long flights: about 0.6 × the distance at
  // the midpoint, never lower than the straight interpolation.
  flight.peakRangeM = std::min(0.6 * flight.distanceM, Camera::maximumRangeM);
  flight.durationS = durationS > 0.0 ? durationS : flightDurationS(flight.to);
  m_flight = flight;
}

CameraPose CameraController::flightPose(const Flight& f, double progress)
{
  const double e = smoothStep(progress);
  CameraPose pose = f.to;
  const GeodesicDirect along = geodesicDirect(
      f.from.targetLatDeg, f.from.targetLonDeg, f.azimuthDeg, e * f.distanceM);
  pose.targetLatDeg = along.latDeg;
  pose.targetLonDeg = along.lonDeg;
  const double logRange = std::log(f.from.rangeM) +
                          e * (std::log(f.to.rangeM) - std::log(f.from.rangeM));
  const double midRangeM = std::sqrt(f.from.rangeM * f.to.rangeM);
  const double bump = std::max(0.0, f.peakRangeM / midRangeM - 1.0);
  pose.rangeM =
      std::exp(logRange) * (1.0 + bump * 4.0 * progress * (1.0 - progress));
  pose.headingDeg = f.from.headingDeg +
                    e * signedAngleDeg(f.to.headingDeg - f.from.headingDeg);
  pose.tiltDeg = f.from.tiltDeg + e * (f.to.tiltDeg - f.from.tiltDeg);
  return pose;
}

void CameraController::stop()
{
  m_inertia.reset();
  m_flight.reset();
}

bool CameraController::isAnimating() const
{
  return m_flight.has_value() || m_inertia.has_value() || m_motion != NoMotion;
}

bool CameraController::advance(double elapsedS)
{
  const double stepS = std::clamp(elapsedS, 0.0, maximumStepS);
  if (m_flight) {
    advanceFlight(stepS);
  } else if (m_inertia) {
    advanceInertia(stepS);
  }
  if (m_motion != NoMotion) {
    advanceMotion(stepS);
  }
  return isAnimating();
}

void CameraController::advanceFlight(double elapsedS)
{
  if (!m_flight) {
    return;
  }
  Flight& flight = *m_flight;
  flight.elapsedS += elapsedS;
  if (flight.elapsedS >= flight.durationS) {
    const CameraPose end = flight.to;
    m_flight.reset();
    applyPose(end);
    return;
  }
  applyPose(flightPose(flight, flight.elapsedS / flight.durationS));
}

void CameraController::advanceInertia(double elapsedS)
{
  if (!m_inertia) {
    return;
  }
  Inertia& inertia = *m_inertia;
  // Distance covered from t0 to t1 with speed v exp(−k t):
  // v / k (exp(−k t0) − exp(−k t1)).
  const double k = inertiaDecayPerS;
  const double t0 = inertia.elapsedS;
  const double t1 = t0 + elapsedS;
  const double distanceM =
      inertia.speedMPerS / k * (std::exp(-k * t0) - std::exp(-k * t1));
  inertia.elapsedS = t1;
  CameraPose pose = m_camera.pose();
  const GeodesicDirect end = rhumbDirect(pose.targetLatDeg, pose.targetLonDeg,
                                         inertia.azimuthDeg, distanceM);
  pose.targetLatDeg = end.latDeg;
  if (std::isfinite(end.lonDeg)) {
    pose.targetLonDeg = end.lonDeg;
  }
  applyPose(pose);
  if (inertia.speedMPerS * std::exp(-k * t1) <
      inertiaStopRangeFractionPerS * m_camera.pose().rangeM) {
    m_inertia.reset();
  }
}

void CameraController::advanceMotion(double elapsedS)
{
  const unsigned int m = m_motion;
  CameraPose pose = m_camera.pose();

  // The range changes as exp(λ t); the pan speed is proportional to it, so
  // the distance is the integral r0 (exp(λ t) − 1) / λ.
  const double lambda = zoomLogRatePerS * (held(m, ZoomOut) - held(m, ZoomIn));
  const double growth = std::exp(lambda * elapsedS);
  const double rangeIntegral = lambda != 0.0
                                   ? pose.rangeM * (growth - 1.0) / lambda
                                   : pose.rangeM * elapsedS;

  const double forward = held(m, MoveForward) - held(m, MoveBackward);
  const double right = held(m, MoveRight) - held(m, MoveLeft);
  if (forward != 0.0 || right != 0.0) {
    const double azimuthDeg =
        pose.headingDeg + radToDeg(std::atan2(right, forward));
    const GeodesicDirect end =
        rhumbDirect(pose.targetLatDeg, pose.targetLonDeg, azimuthDeg,
                    panRangeFractionPerS * rangeIntegral);
    pose.targetLatDeg = end.latDeg;
    if (std::isfinite(end.lonDeg)) {
      pose.targetLonDeg = end.lonDeg;
    }
  }
  pose.rangeM *= growth;
  pose.headingDeg +=
      turnDegPerS * elapsedS * (held(m, TurnRight) - held(m, TurnLeft));
  pose.tiltDeg +=
      tiltDegPerS * elapsedS * (held(m, TiltUp) - held(m, TiltDown));
  applyPose(pose);
}

} // namespace ts
