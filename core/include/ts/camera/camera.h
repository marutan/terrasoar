// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The globe camera: pose, view and projection, picking rays.
 */
#ifndef TS_CAMERA_CAMERA_H
#define TS_CAMERA_CAMERA_H

#include <optional>

#include "ts/geo/geodetic.h"
#include "ts/maths/mat4.h"
#include "ts/maths/ray.h"
#include "ts/maths/vec3.h"

namespace ts
{

/**
 * Where the camera is, as an orbit around a point on the ellipsoid.
 *
 * The camera looks at the target (on the WGS 84 surface, height 0) from
 * @c rangeM metres away. Heading turns the view about the target's
 * vertical; tilt raises it from straight down towards the horizon. The
 * target is always at the centre of the view.
 *
 * A plain aggregate; the Camera clamps the values (see Camera::setPose()).
 *
 * @reentrant
 */
struct CameraPose {
  /** Latitude of the target in degrees. */
  double targetLatDeg = 0.0;
  /** Longitude of the target in degrees. */
  double targetLonDeg = 0.0;
  /** Distance from the eye to the target in metres. */
  double rangeM = 2.0e7;
  /** Direction the camera faces, degrees clockwise from north. */
  double headingDeg = 0.0;
  /** 0 looks straight down; 90 would look at the horizon. Degrees. */
  double tiltDeg = 0.0;
};

/** Where a point lands on the screen. */
struct ScreenPoint {
  /** Horizontal position in pixels from the left edge. */
  double xPx = 0.0;
  /** Vertical position in pixels from the top edge. */
  double yPx = 0.0;
  /** Distance in front of the eye along the view direction, metres. */
  double depthM = 0.0;
};

/**
 * The globe camera: turns a CameraPose and a viewport into the eye
 * position, view direction, matrices and picking rays.
 *
 * Everything here is double precision. The matrices are relative to the
 * eye: viewRotation() and projection() map an ECEF *offset from the eye*
 * to clip space. Renderers subtract the eye from positions before they go
 * to single precision (relative-to-eye rendering, see
 * docs/design/camera-and-precision.md), so no matrix ever holds a
 * translation of Earth-radius size.
 *
 * Pixel coordinates are continuous: (0, 0) is the top-left corner of the
 * viewport and the centre of pixel (i, j) is at (i + 0.5, j + 0.5). The
 * viewport is in logical (device-independent) pixels; a renderer on a
 * high-DPI screen uses the same camera with more device pixels.
 *
 * A value type, cheap to copy.
 *
 * @reentrant
 */
class Camera
{
public:
  /** Smallest eye height above the ellipsoid, metres. */
  static constexpr double minimumEyeHeightM = 1.0;
  /** Largest range, metres (about eight Earth radii). */
  static constexpr double maximumRangeM = 5.0e7;
  /** Largest tilt, degrees. */
  static constexpr double maximumTiltDeg = 85.0;

  /** Creates a camera over (0°, 0°) at 20 000 km, 800 × 600 pixels. */
  Camera();

  /**
   * Sets the pose. Latitude is clamped to ±89.999 999° (the pose's
   * heading is undefined at a pole), longitude normalised to [−180, 180),
   * heading to [0, 360), tilt to [0, maximumTiltDeg], and the range to at
   * most maximumRangeM and at least what keeps the eye
   * minimumEyeHeightM above the ellipsoid.
   */
  void setPose(const CameraPose& pose);

  /** Returns the (clamped) pose. */
  const CameraPose& pose() const { return m_pose; }

  /** Sets the viewport size in logical pixels; both must be positive. */
  void setViewportSize(double widthPx, double heightPx);

  /** Returns the viewport width in logical pixels. */
  double viewportWidthPx() const { return m_widthPx; }

  /** Returns the viewport height in logical pixels. */
  double viewportHeightPx() const { return m_heightPx; }

  /** Sets the vertical field of view in degrees, in (1, 120). */
  void setVerticalFovDeg(double fovDeg);

  /** Returns the vertical field of view in degrees. */
  double verticalFovDeg() const { return m_fovYDeg; }

  /** Returns the eye position, ECEF metres. */
  const Vec3& eyeEcef() const { return m_eyeEcef; }

  /** Returns the eye position as geodetic coordinates. */
  const Geodetic& eyeGeodetic() const { return m_eyeGeodetic; }

  /** Returns the target position, ECEF metres. */
  const Vec3& targetEcef() const { return m_targetEcef; }

  /** Returns the unit view direction (eye towards target), ECEF. */
  const Vec3& forwardEcef() const { return m_forward; }

  /** Returns the unit screen-up direction, ECEF. */
  const Vec3& upEcef() const { return m_up; }

  /** Returns the unit screen-right direction, ECEF. */
  const Vec3& rightEcef() const { return m_right; }

  /** Returns tan(horizontal field of view / 2). */
  double tanHalfFovX() const;

  /** Returns tan(vertical field of view / 2). */
  double tanHalfFovY() const;

  /** Returns the near clip distance in metres. */
  double nearM() const { return m_nearM; }

  /**
   * Returns the far clip distance in metres: beyond the farthest point of
   * the ellipsoid that can be seen (the horizon).
   */
  double farM() const { return m_farM; }

  /**
   * Returns the rotation from ECEF offsets (point − eye) to camera space:
   * x right, y up, z towards the viewer (OpenGL convention).
   */
  Mat4 viewRotation() const;

  /** Returns the OpenGL perspective projection for nearM() and farM(). */
  Mat4 projection() const;

  /** Returns projection() × viewRotation(). */
  Mat4 viewProjection() const;

  /**
   * Returns the ray from the eye through the point (@p xPx, @p yPx) of
   * the viewport. Its direction has length 1, so the ray parameter is
   * metres.
   */
  Ray rayThroughPixel(double xPx, double yPx) const;

  /**
   * Returns the point of the WGS 84 surface under (@p xPx, @p yPx), or
   * nothing if the ray there misses the globe.
   */
  std::optional<Geodetic> pickGeodetic(double xPx, double yPx) const;

  /**
   * Returns where the ECEF point @p pointEcef lands on the screen, or
   * nothing if it is behind the eye. Points off screen still return their
   * (outside) coordinates.
   */
  std::optional<ScreenPoint> project(const Vec3& pointEcef) const;

private:
  void update();

  CameraPose m_pose;
  double m_widthPx = 800.0;
  double m_heightPx = 600.0;
  double m_fovYDeg = 45.0;

  Vec3 m_eyeEcef;
  Geodetic m_eyeGeodetic;
  Vec3 m_targetEcef;
  Vec3 m_forward;
  Vec3 m_up;
  Vec3 m_right;
  double m_nearM = 1.0;
  double m_farM = 1.0e8;
};

/**
 * Returns the eye position (ECEF metres) of @p pose without clamping it.
 *
 * @threadsafe
 */
Vec3 eyeEcefForPose(const CameraPose& pose);

} // namespace ts

#endif // TS_CAMERA_CAMERA_H
