// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Turns mouse, wheel and keyboard input into camera motion.
 */
#ifndef TS_CAMERA_CAMERACONTROLLER_H
#define TS_CAMERA_CAMERACONTROLLER_H

#include <deque>
#include <optional>

#include <QPointF>

#include "ts/camera/camera.h"
#include "ts/maths/vec3.h"

namespace ts
{

/**
 * Continuous motions driven by held keys. Combine with `|`.
 */
enum CameraMotion : unsigned int {
  /** No key held. */
  NoMotion = 0,
  /** Move the target forward (in the heading direction). */
  MoveForward = 1U << 0U,
  /** Move the target backward. */
  MoveBackward = 1U << 1U,
  /** Move the target to the right of the heading. */
  MoveRight = 1U << 2U,
  /** Move the target to the left of the heading. */
  MoveLeft = 1U << 3U,
  /** Shorten the range. */
  ZoomIn = 1U << 4U,
  /** Lengthen the range. */
  ZoomOut = 1U << 5U,
  /** Turn the heading clockwise (the view turns right). */
  TurnRight = 1U << 6U,
  /** Turn the heading anticlockwise. */
  TurnLeft = 1U << 7U,
  /** Tilt towards the horizon. */
  TiltUp = 1U << 8U,
  /** Tilt towards straight down. */
  TiltDown = 1U << 9U,
};

/**
 * Camera navigation: drag to pan, drag to rotate, wheel to zoom, keys to
 * move, inertia after a pan, and animated fly-to.
 *
 * Input methods take positions in logical viewport pixels and times in
 * seconds from any monotonic clock (QElapsedTimer in the application, a
 * fake clock in tests). Motion that continues over time (held keys,
 * inertia, fly-to) advances only in advance(), by elapsed time, never by
 * frame count, so it covers the same ground at any frame rate:
 *
 * - Panning keeps the grabbed point of the globe under the cursor.
 * - Held keys move at rates per second; zooming is a factor per second.
 *   Moves follow rhumb lines (constant heading), so a move split into many
 *   steps ends exactly where one step would.
 * - Inertia decays as exp(−k t) and is integrated exactly.
 * - Fly-to has a duration in seconds; at a low frame rate it skips frames.
 * - A step longer than maximumStepS (a stall) is shortened to it, so the
 *   camera never jumps.
 *
 * @reentrant
 */
class CameraController
{
public:
  /** Longest time step advance() applies, seconds. */
  static constexpr double maximumStepS = 0.25;

  /** Creates a controller with a default camera. */
  CameraController();

  /** Returns the camera. */
  const Camera& camera() const { return m_camera; }

  /** Sets the camera pose directly, stopping any animation. */
  void setPose(const CameraPose& pose);

  /** Sets the viewport size in logical pixels. */
  void setViewportSize(double widthPx, double heightPx);

  /**
   * Starts a pan drag at @p positionPx at time @p timeS. Returns false
   * (and starts nothing) if the cursor is not over the globe.
   */
  bool beginPan(QPointF positionPx, double timeS);

  /** Starts a rotate drag (heading and tilt about the target). */
  void beginRotate(QPointF positionPx);

  /** Moves the current drag to @p positionPx at time @p timeS. */
  void dragTo(QPointF positionPx, double timeS);

  /**
   * Ends the current drag at time @p timeS. A pan that was still moving
   * continues with inertia.
   */
  void endDrag(double timeS);

  /** Returns true while a drag is in progress. */
  bool isDragging() const { return m_drag != Drag::None; }

  /**
   * Zooms by @p steps wheel steps (positive is in; fractions allowed),
   * keeping the point under @p positionPx in place if it is on the globe.
   */
  void zoomAt(QPointF positionPx, double steps);

  /** Sets the held-key motions, a combination of CameraMotion flags. */
  void setMotion(unsigned int motion);

  /** Returns the held-key motions. */
  unsigned int motion() const { return m_motion; }

  /**
   * Starts an animated flight to @p pose. @p durationS ≤ 0 picks a
   * duration from the distance (about 1 s to 4 s).
   */
  void flyTo(const CameraPose& pose, double durationS);

  /**
   * Returns a pose looking at @p target (its height is ignored) from
   * @p rangeM with the current heading and tilt, for flyTo().
   */
  CameraPose poseLookingAt(const Geodetic& target, double rangeM) const;

  /** Stops inertia and any flight; held keys keep working. */
  void stop();

  /**
   * Returns true while the camera moves without input: inertia, a flight
   * or held keys. While false, nothing needs redrawing.
   */
  bool isAnimating() const;

  /**
   * Advances inertia, flights and held-key motion by @p elapsedS seconds
   * (at most maximumStepS). Returns isAnimating().
   */
  bool advance(double elapsedS);

  /** Returns the auto duration flyTo() would pick from here to @p pose. */
  double flightDurationS(const CameraPose& pose) const;

private:
  enum class Drag { None, Pan, Rotate };

  struct DragSample {
    double timeS;
    double latDeg;
    double lonDeg;
  };

  struct Inertia {
    double azimuthDeg = 0.0;
    double speedMPerS = 0.0;
    double elapsedS = 0.0;
  };

  struct Flight {
    CameraPose from;
    CameraPose to;
    double distanceM = 0.0;
    double azimuthDeg = 0.0;
    double peakRangeM = 0.0;
    double durationS = 1.0;
    double elapsedS = 0.0;
  };

  void applyPose(const CameraPose& pose);
  bool placeUnderCursor(const Vec3& pointEcef, QPointF positionPx);
  void advanceInertia(double elapsedS);
  void advanceFlight(double elapsedS);
  void advanceMotion(double elapsedS);
  static CameraPose flightPose(const Flight& flight, double progress);

  Camera m_camera;
  Drag m_drag = Drag::None;
  Vec3 m_grabEcef;
  QPointF m_lastPositionPx;
  std::deque<DragSample> m_samples;
  std::optional<Inertia> m_inertia;
  std::optional<Flight> m_flight;
  unsigned int m_motion = NoMotion;
};

} // namespace ts

#endif // TS_CAMERA_CAMERACONTROLLER_H
