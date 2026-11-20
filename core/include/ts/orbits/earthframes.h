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
 * Sidereal time and the conversion from SGP4's TEME frame to the
 * Earth-fixed WGS 84 frame (ECEF) the globe uses.
 */
#ifndef TS_ORBITS_EARTHFRAMES_H
#define TS_ORBITS_EARTHFRAMES_H

#include "ts/maths/vec3.h"
#include "ts/orbits/orbitalelements.h"
#include "ts/orbits/sgp4.h"

namespace ts
{

/** The Earth's rotation rate, radians per second (Vallado's value). */
constexpr double earthRotationRadS = 7.292115146706979e-5;

/**
 * Returns Greenwich mean sidereal time (IAU 1982, as SGP4 uses), radians
 * in [0, 2π), at the Julian date @p jdUt1 (UT1; UTC is used for it).
 */
double greenwichSiderealRad(double jdUt1);

/** Same, from a split Julian date (keeps its precision). */
double greenwichSiderealRad(JulianDate ut1);

/**
 * A position and velocity in the Earth-fixed frame (ECEF, WGS 84),
 * metres; the velocity is relative to the turning Earth.
 *
 * @reentrant
 */
struct EcefState {
  /** Position, metres. */
  Vec3 positionM;
  /** Velocity, metres per second. */
  Vec3 velocityMS;
};

/**
 * Returns @p teme in the Earth-fixed frame at the sidereal time
 * @p gmstRad: turned about the pole by sidereal time (TEME to the
 * pseudo-Earth-fixed frame). Polar motion, a few metres, is left out.
 */
EcefState temeToEcef(const TemeState& teme, double gmstRad);

/** Returns the TEME position (km) of the Earth-fixed @p positionM. */
Vec3 ecefToTemeKm(const Vec3& positionM, double gmstRad);

} // namespace ts

#endif // TS_ORBITS_EARTHFRAMES_H
