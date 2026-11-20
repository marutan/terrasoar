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
#include "ts/orbits/satellitepasses.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ts/maths/angles.h"
#include "ts/orbits/earthframes.h"

namespace ts
{

namespace
{

/** Elevations of one satellite from one place over time. */
class Sky
{
public:
  Sky(const Sgp4& sgp4, const Geodetic& observer, JulianDate from)
      : m_sgp4(&sgp4), m_observer(observer), m_from(from)
  {
  }

  /** The time @p minutes after the start. */
  JulianDate at(double minutes) const
  {
    JulianDate time = m_from;
    time.fraction += minutes / 1440.0;
    return time;
  }

  /** Look angles @p minutes after the start; elevation −90 if none. */
  LookAngles look(double minutes) const
  {
    const JulianDate time = at(minutes);
    const Sgp4Result result = m_sgp4->propagate(time);
    if (!result.ok()) {
      return LookAngles{0.0, -90.0, 0.0};
    }
    const EcefState ecef = temeToEcef(result.teme, greenwichSiderealRad(time));
    return lookAngles(m_observer, ecef.positionM);
  }

  double elevation(double minutes) const { return look(minutes).elevationDeg; }

  /**
   * Returns where the elevation crosses @p limit between @p low (on one
   * side) and @p high (on the other), to about a second.
   */
  // Two times, then an elevation.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  double crossing(double low, double high, double limit) const
  {
    const bool lowUp = elevation(low) >= limit;
    for (int32_t i = 0; i < 40 && std::fabs(high - low) > 1.0 / 120.0; ++i) {
      const double middle = 0.5 * (low + high);
      if ((elevation(middle) >= limit) == lowUp) {
        low = middle;
      } else {
        high = middle;
      }
    }
    return 0.5 * (low + high);
  }

  /** Returns the time of the highest elevation in [@p a, @p b]. */
  double highest(double a, double b) const
  {
    // Golden-section search; a pass has one maximum.
    const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
    double c = b - ratio * (b - a);
    double d = a + ratio * (b - a);
    for (int32_t i = 0; i < 60 && b - a > 1.0 / 120.0; ++i) {
      if (elevation(c) > elevation(d)) {
        b = d;
      } else {
        a = c;
      }
      c = b - ratio * (b - a);
      d = a + ratio * (b - a);
    }
    return 0.5 * (a + b);
  }

private:
  const Sgp4* m_sgp4;
  Geodetic m_observer;
  JulianDate m_from;
};

/** Returns the first sample at or after @p start whose "up" differs. */
// Times, a step, then an elevation, as named.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double scan(const Sky& sky, double start, double end, double step, double limit,
            bool up)
{
  double t = start;
  while (t < end) {
    const double next = std::min(t + step, end);
    if ((sky.elevation(next) >= limit) != up) {
      return sky.crossing(t, next, limit);
    }
    t = next;
  }
  return end;
}

} // namespace

LookAngles lookAngles(const Geodetic& observer, const Vec3& targetEcefM)
{
  const LocalFrame frame = localFrame(observer.latDeg, observer.lonDeg);
  const Vec3 rho = targetEcefM - geodeticToEcef(observer);
  const double range = length(rho);
  LookAngles angles;
  angles.rangeKm = range / 1000.0;
  if (range <= 0.0) {
    angles.elevationDeg = 90.0;
    return angles;
  }
  angles.elevationDeg = radToDeg(
      std::asin(std::clamp(dot(rho, frame.upEcef) / range, -1.0, 1.0)));
  double azimuth =
      radToDeg(std::atan2(dot(rho, frame.eastEcef), dot(rho, frame.northEcef)));
  if (azimuth < 0.0) {
    azimuth += 360.0;
  }
  angles.azimuthDeg = azimuth >= 360.0 ? 0.0 : azimuth;
  return angles;
}

// An elevation, then days, as the header documents.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::optional<SatellitePass> nextPass(const Sgp4& sgp4,
                                      const Geodetic& observer, JulianDate from,
                                      double minElevationDeg, double searchDays)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const Sky sky(sgp4, observer, from);
  const double span = searchDays * 1440.0;
  // Steps short enough not to miss most passes: under a minute for low
  // orbits, a few minutes for high ones.
  const double step = std::clamp(sgp4.periodMinutes() / 100.0, 0.25, 5.0);
  SatellitePass pass;
  double rise = 0.0;
  if (sky.elevation(0.0) >= minElevationDeg) {
    pass.inProgress = true;
    // When it rose: back at most the search span.
    rise = -span;
    double t = 0.0;
    while (t > -span) {
      const double previous = std::max(t - step, -span);
      if (sky.elevation(previous) < minElevationDeg) {
        rise = sky.crossing(previous, t, minElevationDeg);
        break;
      }
      t = previous;
    }
  } else {
    rise = scan(sky, 0.0, span, step, minElevationDeg, false);
    if (rise >= span) {
      return std::nullopt;
    }
  }
  const double start = std::max(rise, 0.0);
  const double set =
      scan(sky, start, start + span, step, minElevationDeg, true);
  // Up for the whole search: it doesn't set (a geostationary satellite).
  pass.alwaysUp = set >= start + span;
  pass.rise = sky.at(rise);
  pass.set = sky.at(set);
  const double top = sky.highest(rise, set);
  pass.culmination = sky.at(top);
  pass.maxElevationDeg = sky.elevation(top);
  pass.riseAzimuthDeg = sky.look(rise).azimuthDeg;
  pass.setAzimuthDeg = sky.look(set).azimuthDeg;
  return pass;
}

} // namespace ts
