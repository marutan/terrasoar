// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Map projections for the 2D views: the interface and the list of
 * projections the program offers.
 */
#ifndef TS_PROJECTION_PROJECTION_H
#define TS_PROJECTION_PROJECTION_H

#include <array>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include "ts/geo/geodetic.h"

namespace ts
{

/** A point on the map plane, metres. */
struct MapPoint {
  /** Easting from the central meridian. */
  double xM = 0.0;
  /** Northing from the equator. */
  double yM = 0.0;
};

/** A rectangle on the map plane, metres. */
struct MapRect {
  /** Smallest easting. */
  double westM = 0.0;
  /** Smallest northing. */
  double southM = 0.0;
  /** Largest easting. */
  double eastM = 0.0;
  /** Largest northing. */
  double northM = 0.0;

  /** Returns the width. */
  constexpr double widthM() const { return eastM - westM; }
  /** Returns the height. */
  constexpr double heightM() const { return northM - southM; }
};

/** How much the map stretches the ground at a point (1 is true scale). */
struct MapScale {
  /** Along the parallel (east-west), often written k. */
  double parallel = 1.0;
  /** Along the meridian (north-south), often written h. */
  double meridian = 1.0;
};

/**
 * What a projection's GLSL function needs about a reference point,
 * computed in double precision and handed to the shader as floats: two
 * vec4 uniforms whose meaning is the projection's own.
 */
struct ProjectionShaderReference {
  /** First vec4. */
  std::array<float, 4> r0{};
  /** Second vec4. */
  std::array<float, 4> r1{};
};

/**
 * A map projection of the WGS 84 ellipsoid onto a plane.
 *
 * Besides the usual forward and inverse, a projection brings a GLSL
 * function, so vertex shaders place geometry on the map themselves and
 * one mesh serves the globe and every projection. Floats can't hold a
 * map coordinate to better than a metre or so; the GLSL function
 * therefore doesn't project a position but the *difference* from a
 * reference point (a tile's centre), which stays exact close to it:
 *
 *     vec2 <glslFunction>(vec2 dLonLatRad, vec4 r0, vec4 r1)
 *
 * returns, in metres, forward(reference + d) − forward(reference) for an
 * offset in longitude and latitude (radians) from the reference whose
 * shaderReference() is (r0, r1). See docs/design/projections.md, which
 * also says how to add a projection.
 *
 * Projections are immutable after construction.
 *
 * @threadsafe
 */
class Projection
{
public:
  /** Creates a projection centred on @p centralMeridianDeg. */
  explicit Projection(double centralMeridianDeg = 0.0);
  virtual ~Projection();

  Projection(const Projection&) = delete;
  Projection& operator=(const Projection&) = delete;
  Projection(Projection&&) = delete;
  Projection& operator=(Projection&&) = delete;

  /** Returns the identifier used in project files, e.g. "mercator". */
  virtual QString id() const = 0;

  /** Returns the name to show the user, translated. */
  virtual QString displayName() const = 0;

  /** Returns the PROJ string of the same projection (for tests, WMS). */
  virtual QString projString() const = 0;

  /** Returns the longitude that maps to x = 0, degrees. */
  double centralMeridianDeg() const { return m_centralMeridianDeg; }

  /** Returns the southern limit of the valid domain, degrees. */
  virtual double minimumLatDeg() const = 0;

  /** Returns the northern limit of the valid domain, degrees. */
  virtual double maximumLatDeg() const = 0;

  /** Returns true if @p latDeg is inside the valid domain. */
  bool containsLatitude(double latDeg) const
  {
    return latDeg >= minimumLatDeg() && latDeg <= maximumLatDeg();
  }

  /**
   * Returns the map position of a point. Longitudes wrap to within 180°
   * of the central meridian; latitudes outside the valid domain are
   * placed on its edge (the edge of the map).
   */
  virtual MapPoint forward(double latDeg, double lonDeg) const = 0;

  /**
   * Returns the point (height 0) at a map position, or nothing if it is
   * outside bounds().
   */
  virtual std::optional<Geodetic> inverse(const MapPoint& point) const = 0;

  /** Returns the rectangle the valid domain maps to. */
  virtual MapRect bounds() const = 0;

  /** Returns the map's scale factors at @p latDeg. */
  virtual MapScale scaleAt(double latDeg) const = 0;

  /**
   * Returns true if x depends only on the longitude, linearly, and y only
   * on the latitude (a normal cylindrical projection). A rectangle of
   * longitude and latitude is then a rectangle on the map, which lets the
   * tile renderer draw a tile as one column of quads instead of a grid.
   */
  virtual bool isCylindrical() const { return false; }

  /** Returns the name of the GLSL function (see the class description). */
  virtual QByteArray glslFunction() const = 0;

  /**
   * Returns the GLSL source of glslFunction(). It may use the helpers of
   * projectionGlsl().
   */
  virtual QByteArray glslSource() const = 0;

  /**
   * Returns the uniforms glslFunction() needs for a reference point at
   * @p latDeg, @p lonDeg.
   */
  virtual ProjectionShaderReference shaderReference(double latDeg,
                                                    double lonDeg) const = 0;

private:
  double m_centralMeridianDeg;
};

/**
 * Returns the projections the program offers, centred on Greenwich, in
 * the order of the user interface. The first is the default. They live
 * for the whole run.
 *
 * @threadsafe
 */
const std::vector<const Projection*>& projections();

/**
 * Returns the projection with identifier @p id, or null.
 *
 * @threadsafe
 */
const Projection* projectionById(const QString& id);

/**
 * Returns the index of @p projection in projections(), which is also its
 * index for tsProject() in shaders; −1 if it is not one of them.
 *
 * @threadsafe
 */
int projectionIndex(const Projection* projection);

/**
 * Returns the GLSL source of every projection in projections(), their
 * shared helpers, and
 *
 *     vec2 tsProject(int index, vec2 dLonLatRad, vec4 r0, vec4 r1)
 *
 * which calls the projection with that projectionIndex().
 *
 * @threadsafe
 */
QByteArray projectionGlsl();

} // namespace ts

#endif // TS_PROJECTION_PROJECTION_H
