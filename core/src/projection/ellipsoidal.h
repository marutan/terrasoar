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
 * WGS 84 functions the projections share (PROJ's pj_tsfn, pj_msfn and
 * pj_phi2), and their GLSL. Private to core/src/projection.
 */
#ifndef TS_PROJECTION_ELLIPSOIDAL_H
#define TS_PROJECTION_ELLIPSOIDAL_H

#include <cmath>
#include <cstdint>

#include <QByteArray>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts::ellipsoidal
{

/** WGS 84's eccentricity. */
inline double eccentricity()
{
  return std::sqrt(wgs84::eccentricitySquared);
}

/** Snyder's t(φ) = exp(−ψ), ψ the isometric latitude (PROJ's pj_tsfn). */
inline double tsfn(double phi)
{
  const double e = eccentricity();
  const double s = std::sin(phi);
  const double c = std::cos(phi);
  return std::exp(e * std::atanh(e * s)) *
         (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c);
}

/** cos φ / √(1 − e² sin² φ) (PROJ's pj_msfn). */
inline double msfn(double phi)
{
  const double s = std::sin(phi);
  return std::cos(phi) / std::sqrt(1.0 - wgs84::eccentricitySquared * s * s);
}

/** The latitude whose tsfn() is @p ts (PROJ's pj_phi2). */
inline double phiOfTs(double ts)
{
  const double e = eccentricity();
  double phi = 0.5 * pi - 2.0 * std::atan(ts);
  for (int32_t i = 0; i < 15; ++i) {
    const double con = e * std::sin(phi);
    const double next =
        0.5 * pi -
        2.0 * std::atan(ts * std::pow((1.0 - con) / (1.0 + con), 0.5 * e));
    const bool done = std::abs(next - phi) < 1e-15;
    phi = next;
    if (done) {
      break;
    }
  }
  return phi;
}

/** A float for GLSL source. */
inline QByteArray glslNumber(double value)
{
  return QByteArray::number(value, 'g', 9);
}

/** GLSL of tsfn() in float, of a latitude whose sine is `s` and cosine
 *  `c` (needs tsAtanh). */
inline QByteArray glslTsfn()
{
  return "exp(" + glslNumber(eccentricity()) + " * tsAtanh(" +
         glslNumber(eccentricity()) +
         " * s)) * (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c)";
}

} // namespace ts::ellipsoidal

#endif // TS_PROJECTION_ELLIPSOIDAL_H
