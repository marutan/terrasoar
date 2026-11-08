// SPDX-License-Identifier: GPL-2.0-or-later
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
