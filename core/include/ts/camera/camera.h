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

/**
 * @file
 * The globe camera: pose, view and projection, picking rays.
 */
#ifndef TS_CAMERA_CAMERA_H
#define TS_CAMERA_CAMERA_H

#include <functional>
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
   * Half the extent in x of what is in view, map metres: halfWidthM
   * looking straight down with north up, more on a turned or tilted map.
   */
  double viewHalfXM = 0.0;
  /**
   * On a turned or tilted map, the part of the map's plane in view: a
   * convex polygon of offsets from the centre in map metres (anticlockwise),
   * cut off at Camera::mapFarLimitM(). Empty looking straight down with
   * north up, where the view is ± halfWidthM by ± halfHeightM.
   */
  std::vector<MapPoint> footprint;
  /**
   * On a turned or tilted map: the eye's foot on the plane, as an offset
   * from the centre in map metres, and its height above the plane in
   * metres (for distances to the eye). Zero otherwise.
   */
  MapPoint eyeFootM{};
  /** See eyeFootM. */
  double eyeHeightAbovePlaneM = 0.0;

  /** Returns true if the view is turned or tilted (footprint not empty). */
  bool isTurnedOrTilted() const { return !footprint.empty(); }

  /**
   * Returns the distance in metres from the eye to the nearest point of
   * the map rectangle @p offsets (offsets from the centre, map metres),
   * on a turned or tilted map.
   */
  double distanceToEyeM(const MapRect& offsets) const;

  /**
   * Returns true if the rectangle @p offsets (from the centre, map
   * metres) meets what is in view: the footprint, or the straight-down
   * rectangle.
   */
  bool rectInView(const MapRect& offsets) const;

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
  /**
   * Largest tilt of the flat map, degrees: short of looking along it,
   * which a plane without a horizon of its own can't end.
   */
  static constexpr double maximumMapTiltDeg = 70.0;
  /**
   * How far a tilted map is drawn: this many times the eye's height above
   * it (see mapFarLimitM()).
   */
  static constexpr double mapFarEyeHeights = 40.0;
  /** The farthest a tilted map is drawn, metres. */
  static constexpr double maximumMapFarM = 1.0e7;
  /** The tilt at which a tilted map's terrain is in full relief, degrees. */
  static constexpr double mapReliefFullTiltDeg = 15.0;

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

  /**
   * Sets the projection of the flat map; null is ignored. The map is drawn
   * in its Projection::effectiveAt() the view's centre, which changes as
   * the view moves (ARC's zones when following the view).
   */
  void setMapProjection(const Projection* projection);

  /** Returns the projection the map is drawn in now; never null. */
  const Projection* mapProjection() const { return m_projection; }

  /** Returns the projection set with setMapProjection(); never null. */
  const Projection* requestedMapProjection() const
  {
    return m_requestedProjection;
  }

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
   * On the flat map (morph() == 1) the tilt is at most maximumMapTiltDeg,
   * the latitude stays within the projection's domain (and
   * maximumMapCentreLatDeg) and the range within maximumMapRangeM(); with
   * the terrain in relief (a tilted map) the eye stays above it.
   */
  void setPose(const CameraPose& pose);

  /**
   * Returns true on the flat map (morph() == 1) turned from north-up or
   * tilted from straight down.
   */
  bool isMapTurnedOrTilted() const;

  /**
   * Returns how far a tilted flat map is drawn from the eye, metres:
   * mapFarEyeHeights times the eye's height above the map, at most
   * maximumMapFarM. Nothing is drawn, picked or asked for beyond it; a
   * haze hides the edge.
   */
  double mapFarLimitM() const;

  /**
   * Returns how much of the terrain's relief a flat map with tilt
   * @p tiltDeg shows: 0 straight down (the map is flat, with its
   * hillshade), rising smoothly to 1 at mapReliefFullTiltDeg.
   */
  static double mapReliefForTilt(double tiltDeg);

  /**
   * Returns the offset from the centre, in map metres (not wrapped), of
   * the point of the flat map's plane under (@p xPx, @p yPx), or nothing
   * if the ray misses the plane or meets it beyond mapFarLimitM() on a
   * tilted map.
   */
  std::optional<MapPoint> mapOffsetAt(double xPx, double yPx) const;

  /**
   * The height of the surface above the ellipsoid at a point as drawn
   * (the terrain), or nothing where it isn't known yet.
   */
  using SurfaceHeight =
      std::function<std::optional<double>(double latDeg, double lonDeg)>;

  /**
   * Sets the surface the camera picks and keeps above (empty: the
   * ellipsoid). On the globe pickGeodetic() then finds where a ray meets
   * the terrain, and setPose() keeps the eye terrainClearanceM above it.
   * See docs/design/terrain.md.
   */
  void setSurfaceHeight(SurfaceHeight surfaceHeight);

  /** How far above the terrain the eye stays, metres. */
  static constexpr double terrainClearanceM = 20.0;

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
   * Returns where @p ray (from the eye) first meets the surface of
   * setSurfaceHeight() on the globe, the height being the surface's, or
   * nothing if it doesn't within the far distance.
   */
  std::optional<Geodetic> pickSurface(const Ray& ray) const;

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
  void addFootprint(MapFrame& frame) const;
  void keepAboveMapRelief(CameraPose& pose) const;
  double mapRangeLimitM(double latDeg, double lonDeg) const;
  std::optional<ScreenPoint> projectOffset(const Vec3& offset) const;

  CameraPose m_pose;
  double m_morph = 0.0;
  const Projection* m_projection;
  const Projection* m_requestedProjection;
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
  SurfaceHeight m_surfaceHeight;
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
