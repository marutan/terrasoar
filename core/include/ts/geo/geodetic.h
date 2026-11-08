// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Geodetic coordinates on WGS 84 and their conversion to and from ECEF.
 */
#ifndef TS_GEO_GEODETIC_H
#define TS_GEO_GEODETIC_H

#include "ts/maths/vec3.h"

namespace ts
{

/**
 * A position as geodetic latitude, longitude and ellipsoidal height on
 * WGS 84 (EPSG:4979 axis order aside: latitude first here).
 *
 * Latitude is in [-90, 90] degrees, positive north; longitude in degrees,
 * positive east (any value; normaliseLonDeg() brings it into [-180, 180));
 * height in metres above the ellipsoid, not above sea level.
 *
 * A plain aggregate: `Geodetic{latDeg, lonDeg, heightM}`.
 *
 * @reentrant
 */
struct Geodetic {
  /** Geodetic latitude in degrees, positive north. */
  double latDeg = 0.0;
  /** Longitude in degrees, positive east. */
  double lonDeg = 0.0;
  /** Height above the WGS 84 ellipsoid in metres. */
  double heightM = 0.0;
};

/**
 * The local east-north-up frame at a point: three orthonormal ECEF
 * directions. Up is the ellipsoid normal (the geodetic vertical).
 *
 * @reentrant
 */
struct LocalFrame {
  /** Unit vector towards increasing longitude, ECEF. */
  Vec3 eastEcef;
  /** Unit vector towards increasing latitude, ECEF. */
  Vec3 northEcef;
  /** Unit ellipsoid normal, ECEF. */
  Vec3 upEcef;
};

/**
 * Returns @p lonDeg brought into [-180, 180) degrees.
 *
 * @threadsafe
 */
double normaliseLonDeg(double lonDeg);

/**
 * Converts @p position to Earth-centred, Earth-fixed (ECEF, EPSG:4978)
 * metres. Closed form, exact to rounding.
 *
 * @threadsafe
 */
Vec3 geodeticToEcef(const Geodetic& position);

/**
 * Converts the ECEF point @p positionEcef (metres) to geodetic coordinates.
 *
 * Uses Vermeille's closed-form solution (J. Geodesy 85, 2011): no
 * iteration, and accurate to well under a millimetre from 1000 km below the
 * surface out to beyond geostationary orbit. Longitude is in [-180, 180];
 * on the polar axis it is 0.
 *
 * @threadsafe
 */
Geodetic ecefToGeodetic(const Vec3& positionEcef);

/**
 * Returns the unit ellipsoid normal (geodetic vertical) at @p latDeg,
 * @p lonDeg as an ECEF direction.
 *
 * @threadsafe
 */
Vec3 surfaceNormalEcef(double latDeg, double lonDeg);

/**
 * Returns the unit normal of the ellipsoid through the ECEF point
 * @p pointEcef, i.e. the gradient (x/a², y/a², z/b²). On the surface it
 * equals surfaceNormalEcef() of that point's latitude and longitude, without
 * any trigonometry.
 *
 * @threadsafe
 */
Vec3 ellipsoidNormalEcef(const Vec3& pointEcef);

/**
 * Returns the east-north-up frame at @p latDeg, @p lonDeg.
 *
 * @threadsafe
 */
LocalFrame localFrame(double latDeg, double lonDeg);

} // namespace ts

#endif // TS_GEO_GEODETIC_H
