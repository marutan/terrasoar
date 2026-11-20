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
#include "ts/orbits/earthframes.h"

#include <cmath>

#include "ts/maths/angles.h"

namespace ts
{

namespace
{

double siderealFromCenturies(double tut1)
{
  // Seconds of sidereal time (Vallado's gstime).
  double temp = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
                (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;
  // 360°/86400 s = 1/240.
  temp = std::fmod(temp * (pi / 180.0) / 240.0, 2.0 * pi);
  if (temp < 0.0) {
    temp += 2.0 * pi;
  }
  return temp;
}

} // namespace

double greenwichSiderealRad(double jdUt1)
{
  return siderealFromCenturies((jdUt1 - 2451545.0) / 36525.0);
}

double greenwichSiderealRad(JulianDate ut1)
{
  return siderealFromCenturies(((ut1.day - 2451545.0) + ut1.fraction) /
                               36525.0);
}

EcefState temeToEcef(const TemeState& teme, double gmstRad)
{
  const double c = std::cos(gmstRad);
  const double s = std::sin(gmstRad);
  const Vec3& r = teme.positionKm;
  const Vec3& v = teme.velocityKmS;
  EcefState out;
  out.positionM = Vec3{c * r.x + s * r.y, -s * r.x + c * r.y, r.z} * 1000.0;
  const Vec3 turned{c * v.x + s * v.y, -s * v.x + c * v.y, v.z};
  // Less the frame's own turning: v − ω × r.
  out.velocityMS = Vec3{turned.x * 1000.0 + earthRotationRadS * out.positionM.y,
                        turned.y * 1000.0 - earthRotationRadS * out.positionM.x,
                        turned.z * 1000.0};
  return out;
}

Vec3 ecefToTemeKm(const Vec3& positionM, double gmstRad)
{
  const double c = std::cos(gmstRad);
  const double s = std::sin(gmstRad);
  return Vec3{c * positionM.x - s * positionM.y,
              s * positionM.x + c * positionM.y, positionM.z} /
         1000.0;
}

} // namespace ts
