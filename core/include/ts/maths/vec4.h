// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Four-component double-precision vector (homogeneous coordinates).
 */
#ifndef TS_MATHS_VEC4_H
#define TS_MATHS_VEC4_H

#include "ts/maths/vec3.h"

namespace ts
{

/**
 * A four-component vector of doubles, mainly homogeneous coordinates for
 * Mat4: w = 1 for a point, w = 0 for a direction.
 *
 * A plain aggregate: create it with `Vec4{x, y, z, w}`; the default is zero.
 *
 * @reentrant
 */
struct Vec4 {
  /** First component. */
  double x = 0.0;
  /** Second component. */
  double y = 0.0;
  /** Third component. */
  double z = 0.0;
  /** Fourth (homogeneous) component. */
  double w = 0.0;

  /** Returns the point @p p as a homogeneous vector (w = 1). */
  static constexpr Vec4 point(const Vec3& p)
  {
    return Vec4{p.x, p.y, p.z, 1.0};
  }

  /** Returns the direction @p d as a homogeneous vector (w = 0). */
  static constexpr Vec4 direction(const Vec3& d)
  {
    return Vec4{d.x, d.y, d.z, 0.0};
  }

  /** Returns x, y and z, ignoring w. */
  constexpr Vec3 xyz() const { return Vec3{x, y, z}; }
};

/** Returns true if every component of @p a equals that of @p b exactly. */
constexpr bool operator==(const Vec4& a, const Vec4& b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/** Returns true if any component of @p a differs from that of @p b. */
constexpr bool operator!=(const Vec4& a, const Vec4& b)
{
  return !(a == b);
}

} // namespace ts

#endif // TS_MATHS_VEC4_H
