// SPDX-License-Identifier: GPL-2.0-or-later
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
