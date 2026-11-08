// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Splitting a double into two floats for relative-to-eye rendering.
 */
#ifndef TS_MATHS_SPLITDOUBLE_H
#define TS_MATHS_SPLITDOUBLE_H

#include "ts/maths/vec3.h"

namespace ts
{

/**
 * A double as the sum of two floats: high + low.
 *
 * high is the double rounded to float; low is what that rounding lost,
 * rounded to float. Together they keep about 48 bits of the mantissa: an
 * ECEF coordinate of Earth-radius size to about 0.1 µm.
 *
 * On the GPU, (pointHigh − eyeHigh) + (pointLow − eyeLow) gives the
 * offset of a point from the eye with that precision, as long as point and
 * eye are near each other (the high parts then subtract exactly), which is
 * where the precision matters. See docs/design/camera-and-precision.md.
 *
 * @reentrant
 */
struct SplitFloat {
  /** The double rounded to float. */
  float high = 0.0F;
  /** The rounding error of high, rounded to float. */
  float low = 0.0F;
};

/** The three components of a Vec3, each split. */
struct SplitVec3 {
  /** x, y, z rounded to float. */
  float highX = 0.0F;
  /** See highX. */
  float highY = 0.0F;
  /** See highX. */
  float highZ = 0.0F;
  /** Rounding errors of highX, highY, highZ. */
  float lowX = 0.0F;
  /** See lowX. */
  float lowY = 0.0F;
  /** See lowX. */
  float lowZ = 0.0F;
};

/**
 * Splits @p value into high and low floats.
 *
 * @threadsafe
 */
constexpr SplitFloat splitDouble(double value)
{
  const float high = static_cast<float>(value);
  return SplitFloat{high,
                    static_cast<float>(value - static_cast<double>(high))};
}

/**
 * Splits each component of @p v.
 *
 * @threadsafe
 */
constexpr SplitVec3 splitVec3(const Vec3& v)
{
  const SplitFloat x = splitDouble(v.x);
  const SplitFloat y = splitDouble(v.y);
  const SplitFloat z = splitDouble(v.z);
  return SplitVec3{x.high, y.high, z.high, x.low, y.low, z.low};
}

} // namespace ts

#endif // TS_MATHS_SPLITDOUBLE_H
