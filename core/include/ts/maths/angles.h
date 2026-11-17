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
 * The constant pi and conversions between degrees and radians.
 */
#ifndef TS_MATHS_ANGLES_H
#define TS_MATHS_ANGLES_H

namespace ts
{

/** The ratio of a circle's circumference to its diameter. */
inline constexpr double pi = 3.14159265358979323846264338327950288;

/** Returns @p angleDeg degrees in radians. */
constexpr double degToRad(double angleDeg)
{
  return angleDeg * (pi / 180.0);
}

/** Returns @p angleRad radians in degrees. */
constexpr double radToDeg(double angleRad)
{
  return angleRad * (180.0 / pi);
}

} // namespace ts

#endif // TS_MATHS_ANGLES_H
