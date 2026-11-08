// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Geodesics on the WGS 84 ellipsoid: distances, azimuths and destinations.
 */
#ifndef TS_GEO_GEODESIC_H
#define TS_GEO_GEODESIC_H

namespace ts
{

/**
 * The shortest path between two points on the ellipsoid.
 *
 * @reentrant
 */
struct GeodesicInverse {
  /** Length of the geodesic in metres. */
  double distanceM = 0.0;
  /** Azimuth at the first point, degrees clockwise from north, (-180, 180]. */
  double azimuth1Deg = 0.0;
  /** Azimuth at the second point (forward direction), degrees, (-180, 180]. */
  double azimuth2Deg = 0.0;
};

/**
 * The end of a geodesic of given start, azimuth and length.
 *
 * @reentrant
 */
struct GeodesicDirect {
  /** Latitude of the end point in degrees. */
  double latDeg = 0.0;
  /** Longitude of the end point in degrees, [-180, 180]. */
  double lonDeg = 0.0;
  /** Azimuth at the end point (forward direction), degrees, (-180, 180]. */
  double azimuth2Deg = 0.0;
};

/**
 * Returns the geodesic between (@p lat1Deg, @p lon1Deg) and
 * (@p lat2Deg, @p lon2Deg) on WGS 84 (the inverse problem).
 *
 * Computed with GeographicLib (C. F. F. Karney, "Algorithms for geodesics",
 * J. Geodesy 87, 2013): accurate to about 15 nanometres, including nearly
 * antipodal points. Heights are not involved; distances are on the
 * ellipsoid surface.
 *
 * @threadsafe
 */
GeodesicInverse geodesicInverse(double lat1Deg, double lon1Deg, double lat2Deg,
                                double lon2Deg);

/**
 * Returns where a geodesic ends that starts at (@p latDeg, @p lonDeg) with
 * azimuth @p azimuthDeg (degrees clockwise from north) and runs for
 * @p distanceM metres on WGS 84 (the direct problem).
 *
 * Computed with GeographicLib, like geodesicInverse().
 *
 * @threadsafe
 */
GeodesicDirect geodesicDirect(double latDeg, double lonDeg, double azimuthDeg,
                              double distanceM);

} // namespace ts

#endif // TS_GEO_GEODESIC_H
