// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Three-component double-precision vector.
 */
#ifndef TS_MATHS_VEC3_H
#define TS_MATHS_VEC3_H

#include <cmath>

namespace ts
{

/**
 * A three-component vector of doubles: a position or a direction.
 *
 * Positions on the globe are Earth-centred, Earth-fixed (ECEF) metres, which
 * need double precision: a float has a 7-digit mantissa, so at Earth's
 * radius it can't resolve better than about half a metre.
 *
 * A plain aggregate: create it with `Vec3{x, y, z}`; the default is zero.
 *
 * @reentrant
 */
struct Vec3 {
  /** First component. */
  double x = 0.0;
  /** Second component. */
  double y = 0.0;
  /** Third component. */
  double z = 0.0;

  /** Adds @p other component by component. */
  constexpr Vec3& operator+=(const Vec3& other)
  {
    x += other.x;
    y += other.y;
    z += other.z;
    return *this;
  }

  /** Subtracts @p other component by component. */
  constexpr Vec3& operator-=(const Vec3& other)
  {
    x -= other.x;
    y -= other.y;
    z -= other.z;
    return *this;
  }

  /** Multiplies every component by @p factor. */
  constexpr Vec3& operator*=(double factor)
  {
    x *= factor;
    y *= factor;
    z *= factor;
    return *this;
  }
};

/** Returns the component-wise sum of @p a and @p b. */
constexpr Vec3 operator+(const Vec3& a, const Vec3& b)
{
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

/** Returns the component-wise difference @p a - @p b. */
constexpr Vec3 operator-(const Vec3& a, const Vec3& b)
{
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

/** Returns @p v with every component negated. */
constexpr Vec3 operator-(const Vec3& v)
{
  return Vec3{-v.x, -v.y, -v.z};
}

/** Returns @p v scaled by @p factor. */
constexpr Vec3 operator*(const Vec3& v, double factor)
{
  return Vec3{v.x * factor, v.y * factor, v.z * factor};
}

/** Returns @p v scaled by @p factor. */
constexpr Vec3 operator*(double factor, const Vec3& v)
{
  return v * factor;
}

/** Returns @p v divided by @p divisor. */
constexpr Vec3 operator/(const Vec3& v, double divisor)
{
  return Vec3{v.x / divisor, v.y / divisor, v.z / divisor};
}

/** Returns true if every component of @p a equals that of @p b exactly. */
constexpr bool operator==(const Vec3& a, const Vec3& b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

/** Returns true if any component of @p a differs from that of @p b. */
constexpr bool operator!=(const Vec3& a, const Vec3& b)
{
  return !(a == b);
}

/** Returns the dot product of @p a and @p b. */
constexpr double dot(const Vec3& a, const Vec3& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

/** Returns the cross product @p a × @p b (right-handed). */
constexpr Vec3 cross(const Vec3& a, const Vec3& b)
{
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x};
}

/** Returns the squared length of @p v. */
constexpr double lengthSquared(const Vec3& v)
{
  return dot(v, v);
}

/**
 * Returns the length of @p v. Uses the plain square root of the squared
 * length: ECEF values (about 1e7 m) are nowhere near overflowing a double.
 */
inline double length(const Vec3& v)
{
  return std::sqrt(lengthSquared(v));
}

/**
 * Returns @p v scaled to length 1, or the zero vector if @p v has length 0.
 */
inline Vec3 normalized(const Vec3& v)
{
  const double vectorLength = length(v);
  return vectorLength > 0.0 ? v / vectorLength : Vec3{};
}

/** Returns the distance between the points @p a and @p b. */
inline double distance(const Vec3& a, const Vec3& b)
{
  return length(a - b);
}

} // namespace ts

#endif // TS_MATHS_VEC3_H
