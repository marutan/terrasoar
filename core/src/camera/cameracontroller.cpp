// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/camera/cameracontroller.h"

#include <algorithm>
#include <cmath>

#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"
#include "ts/projection/projection.h"

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
  finishMorph();
  stop();
  applyPose(pose);
}

bool CameraController::isOnMap() const
{
  return m_camera.morph() >= 1.0;
}

void CameraController::moveMapCentre(double xM, double yM)
{
  // The map keeps its size on screen: the range follows the scale at the
  // new centre.
  const MapFrame frame = m_camera.mapFrame();
  const Projection& projection = *frame.projection;
  const MapRect bounds = projection.bounds();
  const double wrappedXM =
      bounds.westM + std::fmod(std::fmod(xM - bounds.westM, frame.worldWidthM) +
                                   frame.worldWidthM,
                               frame.worldWidthM);
  const std::optional<Geodetic> centre = projection.inverse(
      MapPoint{wrappedXM, std::clamp(yM, bounds.southM, bounds.northM)});
  if (!centre) {
    return;
  }
  const double mapRangeM = m_camera.pose().rangeM / frame.planeScale;
  CameraPose pose = m_camera.pose();
  pose.targetLatDeg = centre->latDeg;
  pose.targetLonDeg = centre->lonDeg;
  applyPose(pose); // limits the latitude
  pose = m_camera.pose();
  pose.rangeM = mapRangeM / projection.scaleAt(pose.targetLatDeg).parallel;
  applyPose(pose);
}

void CameraController::setMapProjection(const Projection* projection)
{
  if (projection == nullptr) {
    return;
  }
  m_camera.setMapProjection(projection);
  if (m_morph && m_morph->toT >= 1.0) {
    Camera destination = m_camera;
    destination.setMorph(1.0);
    m_morph->to = destination.pose();
  }
}

void CameraController::setViewMode(ViewMode mode, double durationS)
{
  if (mode == m_viewMode) {
    return;
  }
  stop();
  m_drag = Drag::None;
  m_samples.clear();
  m_viewMode = mode;
  Morph morph;
  morph.fromT = m_camera.morph();
  morph.toT = mode == ViewMode::Map ? 1.0 : 0.0;
  morph.from = m_camera.pose();
  // The pose at the end, limited the way the camera will limit it.
  Camera destination = m_camera;
  destination.setMorph(morph.toT);
  morph.to = destination.pose();
  morph.durationS = durationS * std::abs(morph.toT - morph.fromT);
  m_morph = morph;
  if (!(morph.durationS > 0.0)) {
    finishMorph();
  }
}

void CameraController::finishMorph()
{
  if (!m_morph) {
    return;
  }
  const Morph morph = *m_morph;
  m_morph.reset();
  m_camera.setMorph(morph.toT);
  applyPose(morph.to);
}

void CameraController::advanceMorph(double elapsedS)
{
  if (!m_morph) {
    return;
  }
  Morph& morph = *m_morph;
  morph.elapsedS += elapsedS;
  if (morph.elapsedS >= morph.durationS) {
    finishMorph();
    return;
  }
  const double e = smoothStep(morph.elapsedS / morph.durationS);
  CameraPose pose = morph.to;
  pose.targetLatDeg = morph.from.targetLatDeg +
                      e * (morph.to.targetLatDeg - morph.from.targetLatDeg);
  pose.headingDeg =
      morph.from.headingDeg +
      e * signedAngleDeg(morph.to.headingDeg - morph.from.headingDeg);
  pose.tiltDeg =
      morph.from.tiltDeg + e * (morph.to.tiltDeg - morph.from.tiltDeg);
  pose.rangeM =
      std::exp(std::log(morph.from.rangeM) +
               e * (std::log(morph.to.rangeM) - std::log(morph.from.rangeM)));
  m_camera.setMorph(morph.fromT + e * (morph.toT - morph.fromT));
  applyPose(pose);
}

void CameraController::setViewportSize(double widthPx, double heightPx)
{
  m_camera.setViewportSize(widthPx, heightPx);
}

bool CameraController::beginPan(QPointF positionPx, double timeS)
{
  if (m_morph) {
    return false;
  }
  const std::optional<Geodetic> hit =
      m_camera.pickGeodetic(positionPx.x(), positionPx.y());
  if (!hit) {
    return false;
  }
  stop();
  m_drag = Drag::Pan;
  m_grab = *hit;
  m_lastPositionPx = positionPx;
  m_samples.clear();
  m_samples.push_back(DragSample{timeS, m_camera.pose().targetLatDeg,
                                 m_camera.pose().targetLonDeg});
  return true;
}

void CameraController::beginRotate(QPointF positionPx)
{
  if (m_morph || isOnMap()) {
    return;
  }
  stop();
  m_drag = Drag::Rotate;
  m_lastPositionPx = positionPx;
}

void CameraController::dragTo(QPointF positionPx, double timeS)
{
  if (m_drag == Drag::Pan) {
    placeUnderCursor(m_grab, positionPx);
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
    if (spanS > 0.0 && isOnMap()) {
      const MapFrame frame = m_camera.mapFrame();
      const MapPoint from = frame.offsetOf(first.latDeg, first.lonDeg);
      const MapPoint to = frame.offsetOf(last.latDeg, last.lonDeg);
      // Both are within half a world of the centre; so is their difference
      // for any drag a hand can make in a tenth of a second.
      Inertia inertia;
      inertia.onMap = true;
      inertia.mapVelocityXMPerS = (to.xM - from.xM) / spanS;
      inertia.mapVelocityYMPerS = (to.yM - from.yM) / spanS;
      const double speed =
          std::hypot(inertia.mapVelocityXMPerS, inertia.mapVelocityYMPerS);
      if (speed > inertiaStopRangeFractionPerS * m_camera.pose().rangeM /
                      frame.planeScale) {
        m_inertia = inertia;
      }
    } else if (spanS > 0.0) {
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

bool CameraController::placeUnderCursor(const Geodetic& grabbed,
                                        QPointF positionPx)
{
  if (isOnMap()) {
    // The cursor is a fixed offset from the centre in map coordinates, so
    // the centre follows directly.
    const MapFrame frame = m_camera.mapFrame();
    const double ndcX = 2.0 * positionPx.x() / m_camera.viewportWidthPx() - 1.0;
    const double ndcY =
        1.0 - 2.0 * positionPx.y() / m_camera.viewportHeightPx();
    const MapPoint grabbedMap =
        frame.projection->forward(grabbed.latDeg, grabbed.lonDeg);
    moveMapCentre(grabbedMap.xM - ndcX * frame.halfWidthM,
                  grabbedMap.yM - ndcY * frame.halfHeightM);
    return true;
  }
  // Move the target by the lat/lon difference between the grabbed point
  // and the point now under the cursor until they agree. The heading stays
  // fixed, so north keeps its direction on screen.
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
  if (m_morph) {
    return;
  }
  m_inertia.reset();
  m_flight.reset();
  const std::optional<Geodetic> hit =
      m_camera.pickGeodetic(positionPx.x(), positionPx.y());
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
  finishMorph();
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
  return m_morph.has_value() || m_flight.has_value() || m_inertia.has_value() ||
         m_motion != NoMotion;
}

bool CameraController::advance(double elapsedS)
{
  const double stepS = std::clamp(elapsedS, 0.0, maximumStepS);
  if (m_morph) {
    advanceMorph(stepS);
    return isAnimating();
  }
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
  if (inertia.onMap) {
    const double factor = (std::exp(-k * t0) - std::exp(-k * t1)) / k;
    const double speed =
        std::hypot(inertia.mapVelocityXMPerS, inertia.mapVelocityYMPerS) *
        std::exp(-k * t1);
    const MapFrame frame = m_camera.mapFrame();
    moveMapCentre(frame.centreXM + inertia.mapVelocityXMPerS * factor,
                  frame.centreYM + inertia.mapVelocityYMPerS * factor);
    if (!isOnMap() || speed < inertiaStopRangeFractionPerS *
                                  m_camera.pose().rangeM / frame.planeScale) {
      m_inertia.reset();
    }
    return;
  }
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
  if (isOnMap()) {
    // Straight across the map, at a speed proportional to its scale; the
    // map's range (range / plane scale) grows by the same exp(λ t).
    if (forward != 0.0 || right != 0.0) {
      const MapFrame frame = m_camera.mapFrame();
      const double distanceM = panRangeFractionPerS * rangeIntegral /
                               (frame.planeScale * std::hypot(forward, right));
      moveMapCentre(frame.centreXM + right * distanceM,
                    frame.centreYM + forward * distanceM);
      pose = m_camera.pose();
    }
    pose.rangeM *= growth;
    applyPose(pose);
    return;
  }
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
