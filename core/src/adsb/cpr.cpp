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
#include "ts/adsb/cpr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** Latitude zones from the equator to a pole, for each frame (NZ). */
constexpr double zones = 15.0;

/** CPR's 17-bit scale. */
constexpr double scale = 131072.0;

/** Returns the latitudes where NL steps down from n to n − 1 (n = 2–59). */
std::array<double, 60> transitionLatitudes()
{
  std::array<double, 60> latitudes{};
  const double a = 1.0 - std::cos(pi / (2.0 * zones));
  for (int32_t n = 2; n < 60; ++n) {
    latitudes[static_cast<std::size_t>(n)] =
        radToDeg(std::acos(std::sqrt(a / (1.0 - std::cos(2.0 * pi / n)))));
  }
  return latitudes;
}

/** x mod y in [0, y). */
double positiveMod(double x, double y)
{
  const double r = std::fmod(x, y);
  return r < 0.0 ? r + y : r;
}

/** The nearer to @p reference of @p angle plus multiples of @p period. */
double nearest(double angle, double reference, double period)
{
  return angle + period * std::round((reference - angle) / period);
}

/** Latitude solutions of a pair, for a world of @p span degrees. */
struct PairLatitudes {
  double even = 0.0;
  double odd = 0.0;
};

PairLatitudes pairLatitudes(const CprFrame& even, const CprFrame& odd,
                            double span)
{
  const double latEven = even.lat / scale;
  const double latOdd = odd.lat / scale;
  const double j = std::floor(59.0 * latEven - 60.0 * latOdd + 0.5);
  PairLatitudes result;
  result.even = span / 60.0 * (positiveMod(j, 60.0) + latEven);
  result.odd = span / 59.0 * (positiveMod(j, 59.0) + latOdd);
  return result;
}

/** The longitude of a pair at latitude @p latDeg, in [0, span). */
double pairLongitude(const CprFrame& even, const CprFrame& odd, bool oddNewer,
                     double latDeg, double span)
{
  const int32_t nl = cprLongitudeZones(latDeg);
  const double lonEven = even.lon / scale;
  const double lonOdd = odd.lon / scale;
  const double m = std::floor(lonEven * (nl - 1) - lonOdd * nl + 0.5);
  const int32_t n = std::max(oddNewer ? nl - 1 : nl, 1);
  const double lon = oddNewer ? lonOdd : lonEven;
  return span / n * (positiveMod(m, n) + lon);
}

} // namespace

int32_t cprLongitudeZones(double latDeg)
{
  static const std::array<double, 60> transitions = transitionLatitudes();
  const double lat = std::fabs(latDeg);
  for (int32_t n = 59; n >= 2; --n) {
    if (lat < transitions[static_cast<std::size_t>(n)]) {
      return n;
    }
  }
  return 1;
}

std::optional<CprPosition> cprGlobalAirborne(const CprFrame& even,
                                             const CprFrame& odd, bool oddNewer)
{
  PairLatitudes lat = pairLatitudes(even, odd, 360.0);
  if (lat.even >= 270.0) {
    lat.even -= 360.0;
  }
  if (lat.odd >= 270.0) {
    lat.odd -= 360.0;
  }
  if (std::fabs(lat.even) > 90.0 || std::fabs(lat.odd) > 90.0 ||
      cprLongitudeZones(lat.even) != cprLongitudeZones(lat.odd)) {
    return std::nullopt;
  }
  CprPosition position;
  position.latDeg = oddNewer ? lat.odd : lat.even;
  position.lonDeg = normaliseLonDeg(
      pairLongitude(even, odd, oddNewer, position.latDeg, 360.0));
  return position;
}

std::optional<CprPosition> cprGlobalSurface(const CprFrame& even,
                                            const CprFrame& odd, bool oddNewer,
                                            const CprPosition& reference)
{
  const PairLatitudes lat = pairLatitudes(even, odd, 90.0);
  // Each latitude is the northern solution; the southern is 90° less.
  const double north = oddNewer ? lat.odd : lat.even;
  const double latDeg = std::fabs(north - reference.latDeg) <=
                                std::fabs(north - 90.0 - reference.latDeg)
                            ? north
                            : north - 90.0;
  const double otherNorth = oddNewer ? lat.even : lat.odd;
  if (cprLongitudeZones(north) != cprLongitudeZones(otherNorth)) {
    return std::nullopt;
  }
  CprPosition position;
  position.latDeg = latDeg;
  const double lon = pairLongitude(even, odd, oddNewer, latDeg, 90.0);
  position.lonDeg = normaliseLonDeg(nearest(lon, reference.lonDeg, 90.0));
  return position;
}

CprPosition cprLocal(const CprFrame& frame, const CprPosition& reference)
{
  const double span = frame.surface ? 90.0 : 360.0;
  const double i = frame.odd ? 1.0 : 0.0;
  const double latCpr = frame.lat / scale;
  const double lonCpr = frame.lon / scale;
  const double dLat = span / (4.0 * zones - i);
  const double j =
      std::floor(reference.latDeg / dLat) +
      std::floor(positiveMod(reference.latDeg, dLat) / dLat - latCpr + 0.5);
  CprPosition position;
  position.latDeg = dLat * (j + latCpr);
  const double dLon =
      span / std::max(cprLongitudeZones(position.latDeg) - i, 1.0);
  const double m =
      std::floor(reference.lonDeg / dLon) +
      std::floor(positiveMod(reference.lonDeg, dLon) / dLon - lonCpr + 0.5);
  position.lonDeg = normaliseLonDeg(dLon * (m + lonCpr));
  return position;
}

} // namespace ts
