// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/geo/geodesic.h"

#include <GeographicLib/Geodesic.hpp>

namespace ts
{

namespace
{

/**
 * GeographicLib's WGS 84 geodesic object. It is immutable once constructed,
 * and its const member functions are safe to call from several threads at
 * once; C++11 guarantees the one-time construction is thread-safe too.
 */
const GeographicLib::Geodesic& wgs84Geodesic()
{
  return GeographicLib::Geodesic::WGS84();
}

} // namespace

GeodesicInverse geodesicInverse(double lat1Deg, double lon1Deg, double lat2Deg,
                                double lon2Deg)
{
  GeodesicInverse result;
  wgs84Geodesic().Inverse(lat1Deg, lon1Deg, lat2Deg, lon2Deg, result.distanceM,
                          result.azimuth1Deg, result.azimuth2Deg);
  return result;
}

GeodesicDirect geodesicDirect(double latDeg, double lonDeg, double azimuthDeg,
                              double distanceM)
{
  GeodesicDirect result;
  wgs84Geodesic().Direct(latDeg, lonDeg, azimuthDeg, distanceM, result.latDeg,
                         result.lonDeg, result.azimuth2Deg);
  return result;
}

} // namespace ts
