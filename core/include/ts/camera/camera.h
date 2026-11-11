// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The globe camera: pose, view and projection, picking rays.
 */
#ifndef TS_CAMERA_CAMERA_H
#define TS_CAMERA_CAMERA_H

#include <optional>
#include <vector>

#include "ts/geo/geodetic.h"
#include "ts/maths/mat4.h"
#include "ts/maths/ray.h"
#include "ts/maths/vec3.h"
#include "ts/projection/projection.h"

namespace ts
{

class Projection;

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
 * How the flat map of the 2D view lies in front of the camera.
 *
 * The map is drawn on the plane that touches the ellipsoid at the
 * camera's target, with map east along the target's east and map north
 * along its north, scaled so that east-west distances at the target are
 * true. The camera looks at that plane exactly as it looks at the globe,
 * so close to the target the two views agree and the 3D/2D morph barely
 * moves anything there. See docs/design/morph.md.
 *
 * @reentrant
 */
struct MapFrame {
  /** The projection; never null. */
  const Projection* projection = nullptr;
  /** Latitude of the centre of the view (the camera's target), degrees. */
  double centreLatDeg = 0.0;
  /** Longitude of the centre of the view, degrees. */
  double centreLonDeg = 0.0;
  /** Map position of the centre, metres. */
  double centreXM = 0.0;
  /** Map position of the centre, metres. */
  double centreYM = 0.0;
  /** Metres on the plane per metre on the map (1 / parallel scale). */
  double planeScale = 1.0;
  /** Half the width of the view looking straight down, map metres. */
  double halfWidthM = 0.0;
  /** Half the height of the view looking straight down, map metres. */
  double halfHeightM = 0.0;
  /** The map repeats with this period in x, metres. */
  double worldWidthM = 0.0;

  /**
   * Returns the offset of a map position from the centre, in map metres,
   * taking the copy of the world nearest the centre (x within half a
   * world width).
   */
  MapPoint offsetOf(MapPoint point) const;

  /** Same for a point given by latitude and longitude. */
  MapPoint offsetOf(double latDeg, double lonDeg) const;

  /**
   * Returns which copies of the world something between the meridians
   * @p westDeg and @p eastDeg (west < east) must be drawn in to fill the
   * view across the edge where the map wraps around: the shifts in x, in
   * map metres, to add to the offsetOf() of its middle. Mostly one copy
   * (shift 0), two for what straddles the edge, none for what is out of
   * view sideways.
   *
   * With @p wholeWorld true one whole world width around the centre is
   * filled rather than only the view (while the globe unrolls).
   */
  std::vector<double> copyShiftsM(double westDeg, double eastDeg,
                                  bool wholeWorld) const;
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

  /** Largest latitude of the centre of the 2D map, degrees. */
  static constexpr double maximumMapCentreLatDeg = 89.0;

  /** Creates a camera over (0°, 0°) at 20 000 km, 800 × 600 pixels. */
  Camera();

  /**
   * Sets the morph factor between the globe (0) and the flat map (1),
   * clamped to [0, 1]. At exactly 1 the pose is limited to what a map
   * allows (see setPose()).
   */
  void setMorph(double morph);

  /** Returns the morph factor: 0 is the globe, 1 the flat map. */
  double morph() const { return m_morph; }

  /**
   * Returns true if positions on screen are those of the flat map (the
   * morph factor is at least one half): picking and project() use the
   * map then, the globe otherwise.
   */
  bool isMap() const { return m_morph >= 0.5; }

  /** Sets the projection of the flat map; null is ignored. */
  void setMapProjection(const Projection* projection);

  /** Returns the projection of the flat map; never null. */
  const Projection* mapProjection() const { return m_projection; }

  /** Returns how the flat map lies in front of the camera. */
  MapFrame mapFrame() const;

  /**
   * Returns the largest range of the flat map: the view is then as wide
   * as the world, so the map never repeats within it.
   */
  double maximumMapRangeM() const;

  /**
   * Sets the pose. Latitude is clamped to ±89.999 999° (the pose's
   * heading is undefined at a pole), longitude normalised to [−180, 180),
   * heading to [0, 360), tilt to [0, maximumTiltDeg], and the range to at
   * most maximumRangeM and at least what keeps the eye
   * minimumEyeHeightM above the ellipsoid.
   *
   * On the flat map (morph() == 1) the view looks straight down with
   * north up: heading and tilt are 0, the latitude stays within the
   * projection's domain (and maximumMapCentreLatDeg) and the range within
   * maximumMapRangeM().
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
   * nothing if the ray there misses the globe or, on the flat map
   * (isMap()), the point is off the map.
   */
  std::optional<Geodetic> pickGeodetic(double xPx, double yPx) const;

  /**
   * Returns where the ECEF point @p pointEcef lands on the screen, or
   * nothing if it is behind the eye. Points off screen still return their
   * (outside) coordinates. On the flat map (isMap()) the point's latitude
   * and longitude are placed on the map; its height is ignored.
   */
  std::optional<ScreenPoint> project(const Vec3& pointEcef) const;

  /**
   * Returns where the surface point at @p latDeg, @p lonDeg lands on the
   * screen: on the globe or, when isMap(), on the copy of the map nearest
   * the centre of the view.
   */
  std::optional<ScreenPoint> projectGeodetic(double latDeg,
                                             double lonDeg) const;

  /**
   * Returns the position, relative to the eye, of the flat map's point
   * @p offset map metres from the centre of the view (ECEF axes, metres).
   */
  Vec3 mapPlanePoint(MapPoint offset) const;

private:
  void update();
  double mapRangeLimitM(double latDeg) const;
  std::optional<ScreenPoint> projectOffset(const Vec3& offset) const;

  CameraPose m_pose;
  double m_morph = 0.0;
  const Projection* m_projection;
  double m_widthPx = 800.0;
  double m_heightPx = 600.0;
  double m_fovYDeg = 45.0;

  Vec3 m_eyeEcef;
  Geodetic m_eyeGeodetic;
  Vec3 m_targetEcef;
  Vec3 m_forward;
  Vec3 m_up;
  Vec3 m_right;
  LocalFrame m_local;
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
