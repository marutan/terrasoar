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
#include "ts/projection/localexpansion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** Step of the central differences, radians (about 640 m). Rounding and
 *  truncation errors of J and H are then both well under a millimetre
 *  over a tile that uses them. */
constexpr double stepRad = 1e-4;

/** Points along each edge of the domain for bounds(). */
constexpr int32_t outlineSamples = 720;

/** Map metres within which the inverse counts as found. */
constexpr double inverseToleranceM = 1e-7;

/** Slack for "on the edge of the domain", degrees. */
constexpr double domainSlackDeg = 1e-9;

double halfPi()
{
  return 0.5 * pi;
}

} // namespace

LocalExpansionProjection::LocalExpansionProjection(double centralMeridianDeg)
    : Projection(centralMeridianDeg)
{
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint LocalExpansionProjection::forward(double latDeg, double lonDeg) const
{
  const double lat = std::clamp(latDeg, minimumLatDeg(), maximumLatDeg());
  const double limit = maximumLonOffsetDeg();
  const double dLon =
      std::clamp(normaliseLonDeg(lonDeg - centralMeridianDeg()), -limit, limit);
  return project(degToRad(lat), degToRad(dLon));
}

std::optional<Geodetic> LocalExpansionProjection::inverse(MapPoint point) const
{
  const std::optional<std::array<double, 2>> solved = solve(point);
  if (!solved) {
    return std::nullopt;
  }
  const double latDeg = radToDeg((*solved)[0]);
  const double dLonDeg = radToDeg((*solved)[1]);
  if (latDeg < minimumLatDeg() - domainSlackDeg ||
      latDeg > maximumLatDeg() + domainSlackDeg ||
      std::abs(dLonDeg) > maximumLonOffsetDeg() + domainSlackDeg) {
    return std::nullopt;
  }
  return Geodetic{std::clamp(latDeg, minimumLatDeg(), maximumLatDeg()),
                  normaliseLonDeg(centralMeridianDeg() + dLonDeg), 0.0};
}

std::optional<Geodetic>
LocalExpansionProjection::nearestInDomain(MapPoint point) const
{
  const std::optional<std::array<double, 2>> solved = solve(point);
  if (!solved) {
    return std::nullopt;
  }
  const double limit = maximumLonOffsetDeg();
  return Geodetic{
      std::clamp(radToDeg((*solved)[0]), minimumLatDeg(), maximumLatDeg()),
      normaliseLonDeg(centralMeridianDeg() +
                      std::clamp(radToDeg((*solved)[1]), -limit, limit)),
      0.0};
}

std::optional<std::array<double, 2>>
LocalExpansionProjection::solve(MapPoint point) const
{
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const std::optional<std::array<double, 2>> guess = approximateInverse(point);
  if (!guess) {
    return std::nullopt;
  }
  // Newton's method on project(): exact for this forward().
  double lat = (*guess)[0];
  double lon = (*guess)[1];
  for (int32_t i = 0; i < 20; ++i) {
    const MapPoint at = project(lat, lon);
    const double ex = point.xM - at.xM;
    const double ey = point.yM - at.yM;
    if (std::hypot(ex, ey) < inverseToleranceM) {
      break;
    }
    const double h = 1e-7;
    const MapPoint east = project(lat, lon + h);
    const MapPoint north = project(lat + h, lon);
    const double a = (east.xM - at.xM) / h;
    const double b = (north.xM - at.xM) / h;
    const double c = (east.yM - at.yM) / h;
    const double d = (north.yM - at.yM) / h;
    const double det = a * d - b * c;
    if (!(std::abs(det) > 1e-9)) {
      break; // a singular point (a pole of a polar aspect): keep the guess
    }
    lon += (d * ex - b * ey) / det;
    lat += (a * ey - c * ex) / det;
    lat = std::clamp(lat, -halfPi(), halfPi());
  }
  // The point must really be there (a guess off the map may converge to
  // somewhere else).
  const MapPoint check = project(lat, lon);
  if (!(std::hypot(check.xM - point.xM, check.yM - point.yM) < 1e-3)) {
    return std::nullopt;
  }
  return std::array<double, 2>{lat, lon};
}

MapRect LocalExpansionProjection::bounds() const
{
  std::call_once(m_boundsOnce, [this]() {
    // The domain's outline: its meridians and parallels, and the central
    // meridian and equator (an edge needn't hold the extremes).
    MapRect box{std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::lowest(),
                std::numeric_limits<double>::lowest()};
    const double limit = maximumLonOffsetDeg();
    for (int32_t i = 0; i <= outlineSamples; ++i) {
      const double f = static_cast<double>(i) / outlineSamples;
      const double lat =
          minimumLatDeg() + f * (maximumLatDeg() - minimumLatDeg());
      const double lon = -limit + f * 2.0 * limit;
      for (const MapPoint& p :
           {project(degToRad(lat), degToRad(-limit)),
            project(degToRad(lat), degToRad(limit)),
            project(degToRad(lat), 0.0),
            project(degToRad(minimumLatDeg()), degToRad(lon)),
            project(degToRad(maximumLatDeg()), degToRad(lon)),
            project(0.0, degToRad(lon))}) {
        box.westM = std::min(box.westM, p.xM);
        box.eastM = std::max(box.eastM, p.xM);
        box.southM = std::min(box.southM, p.yM);
        box.northM = std::max(box.northM, p.yM);
      }
    }
    m_bounds = box;
  });
  return m_bounds;
}

MapScale LocalExpansionProjection::scaleAt(double latDeg) const
{
  return scaleAtPoint(latDeg, centralMeridianDeg());
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapScale LocalExpansionProjection::scaleAtPoint(double latDeg,
                                                double lonDeg) const
{
  // Lengths on the map of a radian of longitude and of latitude, against
  // their lengths on the WGS 84 ellipsoid (N cos φ and M).
  const double lat =
      std::clamp(degToRad(latDeg), -halfPi() + 1e-9, halfPi() - 1e-9);
  const double dLon = degToRad(normaliseLonDeg(lonDeg - centralMeridianDeg()));
  const Expansion e = expansionAt(lat, dLon);
  const double sine = std::sin(lat);
  const double w2 = 1.0 - wgs84::eccentricitySquared * sine * sine;
  const double primeVertical = wgs84::semiMajorAxisM / std::sqrt(w2);
  const double meridional = wgs84::semiMajorAxisM *
                            (1.0 - wgs84::eccentricitySquared) /
                            (w2 * std::sqrt(w2));
  return MapScale{std::hypot(e.jacobian[0], e.jacobian[2]) /
                      (primeVertical * std::cos(lat)),
                  std::hypot(e.jacobian[1], e.jacobian[3]) / meridional};
}

LocalExpansionProjection::Expansion
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
LocalExpansionProjection::expansionAt(double latRad, double dLonRad) const
{
  // Central differences; near a pole the stencil moves inside, and the
  // expansion is carried back to the point: J(p) = J(c) + H (p − c).
  const double h = stepRad;
  const double centreLat = std::clamp(latRad, -halfPi() + h, halfPi() - h);
  const double shift = latRad - centreLat;
  const double lat = centreLat;
  const double lon = dLonRad;
  const MapPoint f0 = project(lat, lon);
  const MapPoint fe = project(lat, lon + h);
  const MapPoint fw = project(lat, lon - h);
  const MapPoint fn = project(lat + h, lon);
  const MapPoint fs = project(lat - h, lon);
  const MapPoint fne = project(lat + h, lon + h);
  const MapPoint fnw = project(lat + h, lon - h);
  const MapPoint fse = project(lat - h, lon + h);
  const MapPoint fsw = project(lat - h, lon - h);
  Expansion e;
  e.jacobian = {(fe.xM - fw.xM) / (2.0 * h), (fn.xM - fs.xM) / (2.0 * h),
                (fe.yM - fw.yM) / (2.0 * h), (fn.yM - fs.yM) / (2.0 * h)};
  e.hessianX = {(fe.xM - 2.0 * f0.xM + fw.xM) / (h * h),
                (fne.xM - fnw.xM - fse.xM + fsw.xM) / (4.0 * h * h),
                (fn.xM - 2.0 * f0.xM + fs.xM) / (h * h)};
  e.hessianY = {(fe.yM - 2.0 * f0.yM + fw.yM) / (h * h),
                (fne.yM - fnw.yM - fse.yM + fsw.yM) / (4.0 * h * h),
                (fn.yM - 2.0 * f0.yM + fs.yM) / (h * h)};
  e.jacobian[0] += e.hessianX[1] * shift;
  e.jacobian[1] += e.hessianX[2] * shift;
  e.jacobian[2] += e.hessianY[1] * shift;
  e.jacobian[3] += e.hessianY[2] * shift;
  return e;
}

QByteArray LocalExpansionProjection::glslSource() const
{
  // r3: the offsets the domain allows (least and greatest d in longitude,
  // then latitude). r2.w: 1 for the expansion (J in r0; H of x and the
  // first of y's in r1; the rest of y's in r2.xy), 0 for the formula (p0
  // and p1 in r0 and r1, the reference in r2.xy).
  const QByteArray name = glslFunction();
  return glslForwardSource() + "\nvec2 " + name +
         "(vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)\n{\n"
         "  vec2 d = clamp(dLonLatRad, r3.xz, r3.yw);\n"
         "  if (r2.w > 0.5) {\n"
         "    vec2 dd = d * d;\n"
         "    float cross = d.x * d.y;\n"
         "    return vec2(r0.x * d.x + r0.y * d.y\n"
         "                  + 0.5 * (r1.x * dd.x + r1.z * dd.y) + r1.y * "
         "cross,\n"
         "                r0.z * d.x + r0.w * d.y\n"
         "                  + 0.5 * (r1.w * dd.x + r2.y * dd.y) + r2.x * "
         "cross);\n"
         "  }\n"
         "  return " +
         name + "Forward(r2.xy + d, r0, r1) - " + name +
         "Forward(r2.xy, r0, r1);\n"
         "}\n";
}

ProjectionShaderReference
LocalExpansionProjection::shaderReference(double latDeg, double lonDeg) const
{
  return shaderReferenceFor(latDeg, lonDeg,
                            std::numeric_limits<double>::infinity());
}

ProjectionShaderReference
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
LocalExpansionProjection::shaderReferenceFor(double latDeg, double lonDeg,
                                             double halfSpanRad) const
{
  const double latRad =
      degToRad(std::clamp(latDeg, minimumLatDeg(), maximumLatDeg()));
  const double limitRad = degToRad(maximumLonOffsetDeg());
  const double dLonRad =
      std::clamp(degToRad(normaliseLonDeg(lonDeg - centralMeridianDeg())),
                 -limitRad, limitRad);
  ProjectionShaderReference reference;
  reference.r3 = {static_cast<float>(-limitRad - dLonRad),
                  static_cast<float>(limitRad - dLonRad),
                  static_cast<float>(degToRad(minimumLatDeg()) - latRad),
                  static_cast<float>(degToRad(maximumLatDeg()) - latRad)};
  if (halfSpanRad < localExpansionLimitRad()) {
    const Expansion e = expansionAt(latRad, dLonRad);
    reference.r0 = {
        static_cast<float>(e.jacobian[0]), static_cast<float>(e.jacobian[1]),
        static_cast<float>(e.jacobian[2]), static_cast<float>(e.jacobian[3])};
    reference.r1 = {
        static_cast<float>(e.hessianX[0]), static_cast<float>(e.hessianX[1]),
        static_cast<float>(e.hessianX[2]), static_cast<float>(e.hessianY[0])};
    reference.r2 = {static_cast<float>(e.hessianY[1]),
                    static_cast<float>(e.hessianY[2]), 0.0F, 1.0F};
    return reference;
  }
  const std::array<std::array<float, 4>, 2> parameters =
      glslParametersAt(latRad, dLonRad);
  reference.r0 = parameters[0];
  reference.r1 = parameters[1];
  reference.r2 = {static_cast<float>(dLonRad), static_cast<float>(latRad), 0.0F,
                  0.0F};
  return reference;
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<std::array<float, 4>, 2>
LocalExpansionProjection::glslParametersAt(double latRad, double dLonRad) const
{
  Q_UNUSED(latRad)
  Q_UNUSED(dLonRad)
  return glslParameters();
}

} // namespace ts
