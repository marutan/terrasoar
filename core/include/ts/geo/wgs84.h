// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The WGS 84 reference ellipsoid.
 */
#ifndef TS_GEO_WGS84_H
#define TS_GEO_WGS84_H

/**
 * Parameters of the WGS 84 ellipsoid (NGA.STND.0036, EPSG:7030), the shape
 * every Terra Soar coordinate refers to. The defining constants are the
 * semi-major axis and the flattening; the rest follow from them.
 */
namespace ts::wgs84
{

/** Semi-major axis a (equatorial radius), in metres. Defining constant. */
inline constexpr double semiMajorAxisM = 6378137.0;

/** Flattening f = (a − b) / a. Defining constant. */
inline constexpr double flattening = 1.0 / 298.257223563;

/** Semi-minor axis b = a (1 − f) (polar radius), in metres. */
inline constexpr double semiMinorAxisM = semiMajorAxisM * (1.0 - flattening);

/** First eccentricity squared e² = f (2 − f). */
inline constexpr double eccentricitySquared = flattening * (2.0 - flattening);

/** Second eccentricity squared e′² = e² / (1 − e²). */
inline constexpr double secondEccentricitySquared =
    eccentricitySquared / (1.0 - eccentricitySquared);

} // namespace ts::wgs84

#endif // TS_GEO_WGS84_H
