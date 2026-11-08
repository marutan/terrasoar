// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/geo/ellipsoid.h"

#include <cmath>
#include <optional>
#include <utility>

#include "ts/geo/wgs84.h"

namespace ts
{

std::optional<double> intersectEllipsoid(const Ray& rayEcef)
{
  // Scale x and y by 1/a and z by 1/b: the ellipsoid becomes the unit
  // sphere, and the ray parameter t is unchanged.
  constexpr double inverseA = 1.0 / wgs84::semiMajorAxisM;
  constexpr double inverseB = 1.0 / wgs84::semiMinorAxisM;
  const Vec3 origin{rayEcef.origin.x * inverseA, rayEcef.origin.y * inverseA,
                    rayEcef.origin.z * inverseB};
  const Vec3 direction{rayEcef.direction.x * inverseA,
                       rayEcef.direction.y * inverseA,
                       rayEcef.direction.z * inverseB};

  // |origin + t direction|² = 1, i.e. qa t² + 2 qb t + qc = 0.
  const double qa = dot(direction, direction);
  const double qb = dot(origin, direction);
  const double qc = dot(origin, origin) - 1.0;
  if (qa == 0.0) {
    return std::nullopt;
  }
  const double discriminant = qb * qb - qa * qc;
  if (discriminant < 0.0) {
    return std::nullopt;
  }

  // Stable roots: avoid subtracting nearly equal numbers. With
  // q = -(qb + sign(qb) sqrt(disc)), the roots are q / qa and qc / q.
  const double root = std::sqrt(discriminant);
  const double q = qb >= 0.0 ? -(qb + root) : -(qb - root);
  double t0 = q / qa;
  double t1 = q != 0.0 ? qc / q : t0;
  if (t0 > t1) {
    std::swap(t0, t1);
  }
  if (t0 >= 0.0) {
    return t0;
  }
  if (t1 >= 0.0) {
    return t1;
  }
  return std::nullopt;
}

std::optional<Vec3> intersectEllipsoidPoint(const Ray& rayEcef)
{
  const std::optional<double> t = intersectEllipsoid(rayEcef);
  if (!t) {
    return std::nullopt;
  }
  return rayEcef.pointAt(*t);
}

} // namespace ts
