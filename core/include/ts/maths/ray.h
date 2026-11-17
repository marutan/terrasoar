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
