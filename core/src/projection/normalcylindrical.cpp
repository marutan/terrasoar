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
#include "ts/projection/normalcylindrical.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <QCoreApplication>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** Slack for "on the edge of the map", metres. */
constexpr double edgeToleranceM = 1e-6;

/** Gall Stereographic's x and y factors (PROJ's XF and YF). */
constexpr double gallX = 0.70710678118654752440;
constexpr double gallY = 1.70710678118654752440;

/** Gall–Peters' standard parallel, degrees. */
constexpr double gallPetersParallelDeg = 45.0;

double eccentricity()
{
  return std::sqrt(wgs84::eccentricitySquared);
}

/** k0 of the cylindrical equal-area projection with standard parallel. */
double equalAreaScale(double standardParallelDeg)
{
  const double sine = std::sin(degToRad(standardParallelDeg));
  return std::cos(degToRad(standardParallelDeg)) /
         std::sqrt(1.0 - wgs84::eccentricitySquared * sine * sine);
}

/** The authalic q of @p latRad on WGS 84 (PROJ's qsfn). */
double authalicQ(double latRad)
{
  const double e = eccentricity();
  const double sine = std::sin(latRad);
  return (1.0 - wgs84::eccentricitySquared) *
         (sine / (1.0 - wgs84::eccentricitySquared * sine * sine) +
          std::atanh(e * sine) / e);
}

} // namespace

// --- NormalCylindricalProjection ---------------------------------------------

// The x scale, then the latitude limit, as documented.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
NormalCylindricalProjection::NormalCylindricalProjection(double xScale,
                                                         double maximumLatDeg)
    : Projection(0.0), m_xScale(xScale), m_minimumLatDeg(-maximumLatDeg),
      m_maximumLatDeg(maximumLatDeg)
{
}

NormalCylindricalProjection::NormalCylindricalProjection(
    // The x scale, then the latitude limits, as documented.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    double xScale, double minimumLatDeg, double maximumLatDeg)
    : Projection(0.0), m_xScale(xScale), m_minimumLatDeg(minimumLatDeg),
      m_maximumLatDeg(maximumLatDeg)
{
}

double NormalCylindricalProjection::minimumLatDeg() const
{
  return m_minimumLatDeg;
}

double NormalCylindricalProjection::maximumLatDeg() const
{
  return m_maximumLatDeg;
}

double NormalCylindricalProjection::northingM(double latDeg) const
{
  const double clamped = std::clamp(latDeg, minimumLatDeg(), maximumLatDeg());
  return wgs84::semiMajorAxisM * unitNorthing(degToRad(clamped));
}

// Latitude first, as everywhere in this code.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint NormalCylindricalProjection::forward(double latDeg,
                                              double lonDeg) const
{
  const double dLonDeg = normaliseLonDeg(lonDeg - centralMeridianDeg());
  return MapPoint{wgs84::semiMajorAxisM * m_xScale * degToRad(dLonDeg),
                  northingM(latDeg)};
}

std::optional<Geodetic>
NormalCylindricalProjection::inverse(MapPoint point) const
{
  // Written so that a NaN is outside too.
  const MapRect limits = bounds();
  const bool inside = point.xM >= limits.westM - edgeToleranceM &&
                      point.xM <= limits.eastM + edgeToleranceM &&
                      point.yM >= limits.southM - edgeToleranceM &&
                      point.yM <= limits.northM + edgeToleranceM;
  if (!inside) {
    return std::nullopt;
  }
  const double latDeg =
      std::clamp(radToDeg(unitLatitude(point.yM / wgs84::semiMajorAxisM)),
                 minimumLatDeg(), maximumLatDeg());
  const double lonDeg =
      normaliseLonDeg(centralMeridianDeg() +
                      radToDeg(point.xM / (wgs84::semiMajorAxisM * m_xScale)));
  return Geodetic{latDeg, lonDeg, 0.0};
}

MapRect NormalCylindricalProjection::bounds() const
{
  const double halfWidthM = pi * wgs84::semiMajorAxisM * m_xScale;
  return MapRect{-halfWidthM, northingM(minimumLatDeg()), halfWidthM,
                 northingM(maximumLatDeg())};
}

MapScale NormalCylindricalProjection::scaleAt(double latDeg) const
{
  // The ground is the WGS 84 ellipsoid whatever the formulas assume: a
  // radian of longitude is N cos φ long and a radian of latitude M.
  const double lat = degToRad(latDeg);
  const double sine = std::sin(lat);
  const double w2 = 1.0 - wgs84::eccentricitySquared * sine * sine;
  const double primeVertical = wgs84::semiMajorAxisM / std::sqrt(w2);
  const double meridional = wgs84::semiMajorAxisM *
                            (1.0 - wgs84::eccentricitySquared) /
                            (w2 * std::sqrt(w2));
  return MapScale{
      wgs84::semiMajorAxisM * m_xScale / (primeVertical * std::cos(lat)),
      wgs84::semiMajorAxisM * unitNorthingDerivative(lat) / meridional};
}

std::array<float, 2>
NormalCylindricalProjection::latitudeSpan(double latRad) const
{
  return {static_cast<float>(degToRad(minimumLatDeg()) - latRad),
          static_cast<float>(degToRad(maximumLatDeg()) - latRad)};
}

// --- Equidistant Cylindrical -------------------------------------------------

EquidistantCylindricalProjection::EquidistantCylindricalProjection(
    double standardParallelDeg)
    : NormalCylindricalProjection(std::cos(degToRad(standardParallelDeg)),
                                  90.0),
      m_standardParallelDeg(standardParallelDeg)
{
}

QString EquidistantCylindricalProjection::id() const
{
  return QStringLiteral("equidistant-cylindrical");
}

QString EquidistantCylindricalProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection",
                                     "Equidistant Cylindrical (Plate Carrée)");
}

QString EquidistantCylindricalProjection::projString() const
{
  // Spherical formulas with radius a, as Terra Soar's.
  return QStringLiteral("+proj=eqc +lat_ts=%1 +lon_0=0 +R=6378137")
      .arg(m_standardParallelDeg, 0, 'g', 17);
}

std::vector<ProjectionParameter>
EquidistantCylindricalProjection::parameters() const
{
  return {ProjectionParameter{
      QStringLiteral("standardParallel"),
      QCoreApplication::translate("ts::Projection", "Standard parallel"), 0.0,
      0.0, 85.0, QStringLiteral("°")}};
}

ProjectionParameterValues
EquidistantCylindricalProjection::parameterValues() const
{
  return {{QStringLiteral("standardParallel"), m_standardParallelDeg}};
}

QByteArray EquidistantCylindricalProjection::glslFunction() const
{
  return "tsProjectEquidistantCylindrical";
}

QByteArray EquidistantCylindricalProjection::glslSource() const
{
  // r0 = (least dLat, greatest dLat, k, 0): exact differences.
  return "\nvec2 " + glslFunction() +
         "(vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)\n{\n"
         "  return 6378137.0 * vec2(r0.z * dLonLatRad.x,\n"
         "                          clamp(dLonLatRad.y, r0.x, r0.y));\n"
         "}\n";
}

ProjectionShaderReference
EquidistantCylindricalProjection::shaderReference(double latDeg,
                                                  double /*lonDeg*/) const
{
  const double lat = degToRad(std::clamp(latDeg, -90.0, 90.0));
  const std::array<float, 2> span = latitudeSpan(lat);
  ProjectionShaderReference reference;
  reference.r0 = {span[0], span[1], static_cast<float>(xScale()), 0.0F};
  return reference;
}

double EquidistantCylindricalProjection::unitNorthing(double latRad) const
{
  return latRad;
}

double EquidistantCylindricalProjection::unitLatitude(double unitY) const
{
  return unitY;
}

double EquidistantCylindricalProjection::unitNorthingDerivative(
    double /*latRad*/) const
{
  return 1.0;
}

// --- Gall Stereographic
// -------------------------------------------------------

GallStereographicProjection::GallStereographicProjection()
    : NormalCylindricalProjection(gallX, 90.0)
{
}

QString GallStereographicProjection::id() const
{
  return QStringLiteral("gall-stereographic");
}

QString GallStereographicProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Gall Stereographic");
}

QString GallStereographicProjection::projString() const
{
  return QStringLiteral("+proj=gall +lon_0=0 +R=6378137");
}

QByteArray GallStereographicProjection::glslFunction() const
{
  return "tsProjectGallStereographic";
}

QByteArray GallStereographicProjection::glslSource() const
{
  // y = Y tan(φ/2): with A = φt/2 and h = d/2,
  //   tan(A + h) − tan(A) = sin(h) / (cos(A) cos(A + h)),
  //   cos(A + h) = cos(A) cos(h) − sin(A) sin(h),
  // nothing subtracted that is nearly equal. r0 = (least d, greatest d,
  // cos(A), sin(A)).
  return "\nvec2 " + glslFunction() +
         "(vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)\n{\n"
         "  float h = 0.5 * clamp(dLonLatRad.y, r0.x, r0.y);\n"
         "  float sinH = tsSinSmall(h);\n"
         "  float dy = 1.7071067811865475 * sinH\n"
         "           / (r0.z * (r0.z * tsCosSmall(h) - r0.w * sinH));\n"
         "  return 6378137.0 * vec2(0.7071067811865475 * dLonLatRad.x, dy);\n"
         "}\n";
}

ProjectionShaderReference
GallStereographicProjection::shaderReference(double latDeg,
                                             double /*lonDeg*/) const
{
  const double lat = degToRad(std::clamp(latDeg, -90.0, 90.0));
  const std::array<float, 2> span = latitudeSpan(lat);
  ProjectionShaderReference reference;
  reference.r0 = {span[0], span[1], static_cast<float>(std::cos(0.5 * lat)),
                  static_cast<float>(std::sin(0.5 * lat))};
  return reference;
}

double GallStereographicProjection::unitNorthing(double latRad) const
{
  return gallY * std::tan(0.5 * latRad);
}

double GallStereographicProjection::unitLatitude(double unitY) const
{
  return 2.0 * std::atan(unitY / gallY);
}

double GallStereographicProjection::unitNorthingDerivative(double latRad) const
{
  const double cosine = std::cos(0.5 * latRad);
  return 0.5 * gallY / (cosine * cosine);
}

// --- Gall–Peters
// ----------------------------------------------------------------

GallPetersProjection::GallPetersProjection()
    : NormalCylindricalProjection(equalAreaScale(gallPetersParallelDeg), 90.0)
{
}

QString GallPetersProjection::id() const
{
  return QStringLiteral("gall-peters");
}

QString GallPetersProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection",
                                     "Gall–Peters (Gall Orthographic)");
}

QString GallPetersProjection::projString() const
{
  return QStringLiteral("+proj=cea +lat_ts=45 +lon_0=0 +ellps=WGS84");
}

QByteArray GallPetersProjection::glslFunction() const
{
  return "tsProjectGallPeters";
}

QByteArray GallPetersProjection::glslSource() const
{
  // y = Q q(φ) with q = (1 − e²)(s / (1 − e² s²) + atanh(e s) / e) and
  // s = sin φ. With S = sin φt, D = s − S from
  //   sin(p + q) − sin(p) = 2 cos(p + q/2) sin(q/2),
  // the two terms' differences are
  //   s / (1 − e² s²) − S / (1 − e² S²) = D (1 + e² S s) / (w W),
  //   atanh(e s) − atanh(e S) = atanh(e D / (1 − e² S s)),
  // nothing subtracted that is nearly equal. r0 = (least d, greatest d,
  // S, cos φt); r1 = (e, k0, W = 1 − e² S², Q (1 − e²) = (1 − e²)/(2 k0)).
  return "\nvec2 " + glslFunction() +
         "(vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)\n{\n"
         "  float q = clamp(dLonLatRad.y, r0.x, r0.y);\n"
         "  float sinHalf = tsSinSmall(0.5 * q);\n"
         "  float cosHalf = tsCosSmall(0.5 * q);\n"
         "  float d = 2.0 * (r0.w * cosHalf - r0.z * sinHalf) * sinHalf;\n"
         "  float s = r0.z + d;\n"
         "  float e2 = r1.x * r1.x;\n"
         "  float first = d * (1.0 + e2 * r0.z * s)\n"
         "              / ((1.0 - e2 * s * s) * r1.z);\n"
         "  float second = tsAtanh(r1.x * d / (1.0 - e2 * r0.z * s)) / r1.x;\n"
         "  return 6378137.0 * vec2(r1.y * dLonLatRad.x,\n"
         "                          r1.w * (first + second));\n"
         "}\n";
}

ProjectionShaderReference
GallPetersProjection::shaderReference(double latDeg, double /*lonDeg*/) const
{
  const double lat = degToRad(std::clamp(latDeg, -90.0, 90.0));
  const std::array<float, 2> span = latitudeSpan(lat);
  const double sine = std::sin(lat);
  const double e = eccentricity();
  const double k0 = xScale();
  ProjectionShaderReference reference;
  reference.r0 = {span[0], span[1], static_cast<float>(sine),
                  static_cast<float>(std::cos(lat))};
  reference.r1 = {
      static_cast<float>(e), static_cast<float>(k0),
      static_cast<float>(1.0 - wgs84::eccentricitySquared * sine * sine),
      static_cast<float>((1.0 - wgs84::eccentricitySquared) / (2.0 * k0))};
  return reference;
}

double GallPetersProjection::unitNorthing(double latRad) const
{
  return authalicQ(latRad) / (2.0 * xScale());
}

double GallPetersProjection::unitLatitude(double unitY) const
{
  // q(φ) = 2 k0 y: Newton's method from the spherical guess; q rises
  // monotonically with φ, steeply enough away from the poles to converge
  // in a few steps, and is clamped at them.
  const double target = 2.0 * xScale() * unitY;
  const double polar = authalicQ(pi / 2.0);
  // q is flat at the poles (q ≈ qp − c (π/2 − φ)²), so the last few
  // units in the last place of a northing there are rounding, not
  // distance: within them, the pole.
  const double poleSlack =
      16.0 * std::numeric_limits<double>::epsilon() * polar;
  if (target >= polar - poleSlack) {
    return pi / 2.0;
  }
  if (target <= -polar + poleSlack) {
    return -pi / 2.0;
  }
  double lat = std::asin(std::clamp(target / polar, -1.0, 1.0));
  for (int32_t i = 0; i < 50; ++i) {
    const double sine = std::sin(lat);
    const double w = 1.0 - wgs84::eccentricitySquared * sine * sine;
    const double derivative =
        2.0 * (1.0 - wgs84::eccentricitySquared) * std::cos(lat) / (w * w);
    if (!(derivative > 1e-15)) {
      break;
    }
    const double step = (authalicQ(lat) - target) / derivative;
    lat = std::clamp(lat - step, -pi / 2.0, pi / 2.0);
    if (std::abs(step) < 1e-15) {
      break;
    }
  }
  return lat;
}

double GallPetersProjection::unitNorthingDerivative(double latRad) const
{
  const double sine = std::sin(latRad);
  const double w = 1.0 - wgs84::eccentricitySquared * sine * sine;
  return (1.0 - wgs84::eccentricitySquared) * std::cos(latRad) /
         (xScale() * w * w);
}

} // namespace ts
