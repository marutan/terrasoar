// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A half-line: origin plus direction.
 */
#ifndef TS_MATHS_RAY_H
#define TS_MATHS_RAY_H

#include "ts/maths/vec3.h"

namespace ts
{

/**
 * A ray: the points origin + t × direction for t ≥ 0. The direction need not
 * have length 1; t is measured in multiples of it.
 *
 * Picking rays are in ECEF metres: the camera position and the direction
 * through a pixel.
 *
 * @reentrant
 */
struct Ray {
  /** Start point. */
  Vec3 origin;
  /** Direction; not necessarily of length 1. */
  Vec3 direction;

  /** Returns the point at parameter @p t. */
  constexpr Vec3 pointAt(double t) const { return origin + direction * t; }
};

} // namespace ts

#endif // TS_MATHS_RAY_H
