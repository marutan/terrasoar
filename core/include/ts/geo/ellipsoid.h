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
 * Ray intersection with the WGS 84 ellipsoid, for picking.
 */
#ifndef TS_GEO_ELLIPSOID_H
#define TS_GEO_ELLIPSOID_H

#include <optional>

#include "ts/maths/ray.h"
#include "ts/maths/vec3.h"

namespace ts
{

/**
 * Returns where @p rayEcef first meets the WGS 84 ellipsoid surface
 * (height 0), as the ray parameter t ≥ 0, or nothing if it misses.
 *
 * A ray starting outside returns the entry point; one starting inside (or
 * on) the surface returns where it leaves. A ray that just touches the
 * surface counts as a hit.
 *
 * Solved as a quadratic in a frame where the ellipsoid is the unit sphere,
 * in the numerically stable form, so rays from far away that graze the
 * surface keep their precision.
 *
 * @param rayEcef The ray in ECEF metres; its direction must not be zero.
 *
 * @threadsafe
 */
std::optional<double> intersectEllipsoid(const Ray& rayEcef);

/**
 * Returns the first point where @p rayEcef meets the WGS 84 ellipsoid, in
 * ECEF metres, or nothing if it misses. See intersectEllipsoid().
 *
 * @threadsafe
 */
std::optional<Vec3> intersectEllipsoidPoint(const Ray& rayEcef);

} // namespace ts

#endif // TS_GEO_ELLIPSOID_H
