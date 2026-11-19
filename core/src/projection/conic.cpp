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
#include "ts/projection/conic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <QCoreApplication>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** PROJ's tolerance for the poles. */
constexpr double poleEpsilon = 1e-10;

double halfPi()
{
  return 0.5 * pi;
}

double eccentricity()
{
  return std::sqrt(wgs84::eccentricitySquared);
}

/** Snyder's t(φ) = exp(−ψ), ψ the isometric latitude (PROJ's pj_tsfn). */
double tsfn(double phi)
{
  const double e = eccentricity();
  const double s = std::sin(phi);
  const double c = std::cos(phi);
  return std::exp(e * std::atanh(e * s)) *
         (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c);
}

/** cos φ / √(1 − e² sin² φ) (PROJ's pj_msfn). */
double msfn(double phi)
{
  const double s = std::sin(phi);
  return std::cos(phi) / std::sqrt(1.0 - wgs84::eccentricitySquared * s * s);
}

/** The latitude whose tsfn() is @p ts (PROJ's pj_phi2). */
double phiOfTs(double ts)
{
  const double e = eccentricity();
  double phi = halfPi() - 2.0 * std::atan(ts);
  for (int32_t i = 0; i < 15; ++i) {
    const double con = e * std::sin(phi);
    const double next =
        halfPi() -
        2.0 * std::atan(ts * std::pow((1.0 - con) / (1.0 + con), 0.5 * e));
    const bool done = std::abs(next - phi) < 1e-15;
    phi = next;
    if (done) {
      break;
    }
  }
  return phi;
}

/** Helmert's series for WGS 84's meridian arc: A and the sin 2jφ
 *  coefficients (j = 1…4). */
struct MeridianSeries {
  double rectifyingRadius = 0.0;
  std::array<double, 4> arc{};
};

const MeridianSeries& meridianSeries()
{
  static const MeridianSeries series = []() {
    const double f = 1.0 - wgs84::semiMinorAxisM / wgs84::semiMajorAxisM;
    const double n = f / (2.0 - f);
    const double n2 = n * n;
    const double n3 = n2 * n;
    const double n4 = n3 * n;
    MeridianSeries s;
    s.rectifyingRadius =
        wgs84::semiMajorAxisM / (1.0 + n) * (1.0 + n2 / 4.0 + n4 / 64.0);
    s.arc = {-(1.5 * n - 9.0 / 16.0 * n3), 15.0 / 16.0 * n2 - 15.0 / 32.0 * n4,
             -35.0 / 48.0 * n3, 315.0 / 512.0 * n4};
    return s;
  }();
  return series;
}

/** The meridian arc from the equator to @p latRad, metres. */
double meridianArcM(double latRad)
{
  const MeridianSeries& s = meridianSeries();
  double sum = latRad;
  for (std::size_t j = 0; j < s.arc.size(); ++j) {
    sum += s.arc.at(j) * std::sin(2.0 * static_cast<double>(j + 1) * latRad);
  }
  return s.rectifyingRadius * sum;
}

/** sin z / z, 1 at 0. */
double sinc(double z)
{
  return std::abs(z) < 1e-4 ? 1.0 - z * z / 6.0 : std::sin(z) / z;
}

QByteArray number(double value)
{
  return QByteArray::number(value, 'g', 9);
}

/** GLSL of Snyder's t(φ) in float, as tsfn(), for latitude `phi`, sine
 *  `s` and cosine `c`. */
QByteArray glslTsfn()
{
  return "exp(" + number(eccentricity()) + " * tsAtanh(" +
         number(eccentricity()) +
         " * s)) * (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c)";
}

} // namespace

// --- Lambert Conformal Conic -------------------------------------------------

LambertConformalConicProjection::LambertConformalConicProjection(
    // Two parallels, an origin latitude and a meridian: angles all.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    double standardParallel1Deg, double standardParallel2Deg,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    double originLatDeg, double centralMeridianDeg)
    : LocalExpansionProjection(centralMeridianDeg),
      m_parallel1Deg(standardParallel1Deg),
      m_parallel2Deg(standardParallel2Deg), m_originLatDeg(originLatDeg)
{
  // The cylindrical limit (n = 0) has no cone: move the parallels apart.
  const double sum = m_parallel1Deg + m_parallel2Deg;
  if (std::abs(sum) < 1.0) {
    m_parallel2Deg = (sum < 0.0 ? -1.0 : 1.0) - m_parallel1Deg;
  }
  // PROJ's lcc set-up, on the ellipsoid.
  const double phi1 = degToRad(m_parallel1Deg);
  const double phi2 = degToRad(m_parallel2Deg);
  const double m1 = msfn(phi1);
  const double t1 = tsfn(phi1);
  if (std::abs(phi1 - phi2) >= 1e-10) {
    m_n = std::log(m1 / msfn(phi2)) / std::log(t1 / tsfn(phi2));
  } else {
    m_n = std::sin(phi1);
  }
  m_c = wgs84::semiMajorAxisM * m1 * std::pow(t1, -m_n) / m_n;
  const double phi0 = degToRad(m_originLatDeg);
  m_rho0 = std::abs(std::abs(phi0) - halfPi()) < poleEpsilon
               ? 0.0
               : m_c * std::pow(tsfn(phi0), m_n);
  // From the apex's pole to 60° beyond the nearer standard parallel
  // (further, the scale grows fast towards the far pole at infinity).
  if (m_n > 0.0) {
    m_minimumLatDeg =
        std::max(-80.0, std::min(m_parallel1Deg, m_parallel2Deg) - 60.0);
  } else {
    m_maximumLatDeg =
        std::min(80.0, std::max(m_parallel1Deg, m_parallel2Deg) + 60.0);
  }
}

QString LambertConformalConicProjection::id() const
{
  return QStringLiteral("lambert-conformal-conic");
}

QString LambertConformalConicProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection",
                                     "Lambert Conformal Conic");
}

QString LambertConformalConicProjection::projString() const
{
  return QStringLiteral("+proj=lcc +lat_1=%1 +lat_2=%2 +lat_0=%3 +lon_0=%4 "
                        "+x_0=0 +y_0=0 +ellps=WGS84")
      .arg(m_parallel1Deg, 0, 'g', 17)
      .arg(m_parallel2Deg, 0, 'g', 17)
      .arg(m_originLatDeg, 0, 'g', 17)
      .arg(centralMeridianDeg(), 0, 'g', 17);
}

std::vector<ProjectionParameter>
LambertConformalConicProjection::parameters() const
{
  return {ProjectionParameter{QStringLiteral("standardParallel1"),
                              QCoreApplication::translate(
                                  "ts::Projection", "First standard parallel"),
                              35.0, -85.0, 85.0, QStringLiteral("°"), 4},
          ProjectionParameter{
              QStringLiteral("standardParallel2"),
              QCoreApplication::translate(
                  "ts::Projection",
                  "Second standard parallel (the first again for one)"),
              65.0, -85.0, 85.0, QStringLiteral("°"), 4},
          ProjectionParameter{
              QStringLiteral("originLatitude"),
              QCoreApplication::translate("ts::Projection", "Origin latitude"),
              52.0, -85.0, 85.0, QStringLiteral("°"), 4},
          ProjectionParameter{
              QStringLiteral("centralMeridian"),
              QCoreApplication::translate("ts::Projection", "Central meridian"),
              10.0, -180.0, 180.0, QStringLiteral("°"), 4}};
}

ProjectionParameterValues
LambertConformalConicProjection::parameterValues() const
{
  return {{QStringLiteral("standardParallel1"), m_parallel1Deg},
          {QStringLiteral("standardParallel2"), m_parallel2Deg},
          {QStringLiteral("originLatitude"), m_originLatDeg},
          {QStringLiteral("centralMeridian"), centralMeridianDeg()}};
}

QByteArray LambertConformalConicProjection::glslFunction() const
{
  return "tsProjectLambertConformalConic";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint LambertConformalConicProjection::project(double latRad,
                                                  double dLonRad) const
{
  // PROJ's lcc forward: ρ = c t(φ)ⁿ about the apex, θ = n λ.
  double rho = 0.0;
  if (std::abs(std::abs(latRad) - halfPi()) >= poleEpsilon) {
    rho = m_c * std::pow(tsfn(latRad), m_n);
  }
  const double theta = m_n * dLonRad;
  return MapPoint{rho * std::sin(theta), m_rho0 - rho * std::cos(theta)};
}

std::optional<std::array<double, 2>>
LambertConformalConicProjection::approximateInverse(MapPoint point) const
{
  // PROJ's lcc inverse (exact but for the latitude's iteration).
  double x = point.xM;
  double y = m_rho0 - point.yM;
  double rho = std::hypot(x, y);
  if (!(std::isfinite(rho))) {
    return std::nullopt;
  }
  if (rho == 0.0) {
    return std::array<double, 2>{m_n > 0.0 ? halfPi() : -halfPi(), 0.0};
  }
  if (m_n < 0.0) {
    rho = -rho;
    x = -x;
    y = -y;
  }
  const double lat = phiOfTs(std::pow(rho / m_c, 1.0 / m_n));
  return std::array<double, 2>{lat, std::atan2(x, y) / m_n};
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double LambertConformalConicProjection::domainMarginDeg(double latDeg,
                                                        double lonDeg) const
{
  // The latitudes are the box's; the longitudes the cone's own, so that
  // its cut is never near what is drawn.
  const double offset =
      std::abs(normaliseLonDeg(lonDeg - centralMeridianDeg()));
  return (lonLimitDeg() - offset) * std::cos(degToRad(latDeg));
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Geodetic LambertConformalConicProjection::clampToDomain(double latDeg,
                                                        double lonDeg) const
{
  const double offset =
      std::clamp(normaliseLonDeg(lonDeg - centralMeridianDeg()), -lonLimitDeg(),
                 lonLimitDeg());
  return Geodetic{std::clamp(latDeg, m_minimumLatDeg, m_maximumLatDeg),
                  normaliseLonDeg(centralMeridianDeg() + offset), 0.0};
}

std::array<float, 8>
LambertConformalConicProjection::glslDomainParameters() const
{
  return {static_cast<float>(degToRad(centralMeridianDeg())),
          static_cast<float>(degToRad(lonLimitDeg())),
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F};
}

QByteArray LambertConformalConicProjection::glslDomainSource() const
{
  // d0: the central meridian and the limit about it, radians.
  return "\nbool " + glslFunction() +
         "Outside(vec2 lonLatRad, vec4 d0, vec4 d1)\n{\n"
         "  float offset = lonLatRad.x - d0.x;\n"
         "  offset -= 6.28318531 * floor((offset + 3.14159265) / "
         "6.28318531);\n"
         "  return abs(offset) > d0.y;\n"
         "}\n";
}

QByteArray LambertConformalConicProjection::glslForwardSource() const
{
  // p0: n, c and ρ0 (metres).
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float phi = clamp(lonLatRad.y, -1.5707953, 1.5707953);\n"
         "  float s = sin(phi);\n"
         "  float c = cos(phi);\n"
         "  float rho = p0.y * pow(max(" +
         glslTsfn() +
         ", 1e-30), p0.x);\n"
         "  float theta = p0.x * lonLatRad.x;\n"
         "  return vec2(rho * sin(theta), p0.z - rho * cos(theta));\n"
         "}\n";
}

double
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
LambertConformalConicProjection::localExpansionLimitRadAt(double latRad,
                                                          double dLonRad) const
{
  // ρ goes as the distance from the apex to the power n < 1: not smooth
  // there, so a tile uses the expansion only if it is small beside its
  // distance from the apex (the formula is precise near the apex, where ρ
  // is small).
  Q_UNUSED(dLonRad)
  const double fromApex = halfPi() - (m_n > 0.0 ? latRad : -latRad);
  return std::min(localExpansionLimitRad(), 0.02 * std::max(fromApex, 0.0));
}

std::array<std::array<float, 4>, 2>
LambertConformalConicProjection::glslParameters() const
{
  return {std::array<float, 4>{static_cast<float>(m_n), static_cast<float>(m_c),
                               static_cast<float>(m_rho0), 0.0F},
          std::array<float, 4>{}};
}

// --- Polyconic ---------------------------------------------------------------

// A meridian, then a latitude.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PolyconicProjection::PolyconicProjection(double centralMeridianDeg,
                                         double originLatDeg)
    : LocalExpansionProjection(centralMeridianDeg),
      m_originLatDeg(originLatDeg),
      m_originArcM(meridianArcM(degToRad(originLatDeg)))
{
}

QString PolyconicProjection::id() const
{
  return QStringLiteral("polyconic");
}

QString PolyconicProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "American Polyconic");
}

QString PolyconicProjection::projString() const
{
  return QStringLiteral("+proj=poly +lat_0=%1 +lon_0=%2 +x_0=0 +y_0=0 "
                        "+ellps=WGS84")
      .arg(m_originLatDeg, 0, 'g', 17)
      .arg(centralMeridianDeg(), 0, 'g', 17);
}

std::vector<ProjectionParameter> PolyconicProjection::parameters() const
{
  return {ProjectionParameter{
              QStringLiteral("centralMeridian"),
              QCoreApplication::translate("ts::Projection", "Central meridian"),
              0.0, -180.0, 180.0, QStringLiteral("°"), 4},
          ProjectionParameter{
              QStringLiteral("originLatitude"),
              QCoreApplication::translate("ts::Projection", "Origin latitude"),
              0.0, -85.0, 85.0, QStringLiteral("°"), 4}};
}

ProjectionParameterValues PolyconicProjection::parameterValues() const
{
  return {{QStringLiteral("centralMeridian"), centralMeridianDeg()},
          {QStringLiteral("originLatitude"), m_originLatDeg}};
}

QByteArray PolyconicProjection::glslFunction() const
{
  return "tsProjectPolyconic";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint PolyconicProjection::project(double latRad, double dLonRad) const
{
  // PROJ's poly forward, written to be smooth through the equator (where
  // PROJ has its own case): with N cos φ = a msfn(φ) and z = λ sin φ,
  //   x = N cos φ λ sinc z,  y = M(φ) − M(φ0) + N cos φ λ sin(z/2) sinc(z/2)
  // (N cos φ / sin φ · sin z, and · 2 sin²(z/2), PROJ's 1 − cos z).
  const double nCos = wgs84::semiMajorAxisM * msfn(latRad);
  const double z = dLonRad * std::sin(latRad);
  return MapPoint{nCos * dLonRad * sinc(z),
                  meridianArcM(latRad) - m_originArcM +
                      nCos * dLonRad * std::sin(0.5 * z) * sinc(0.5 * z)};
}

std::optional<std::array<double, 2>>
PolyconicProjection::approximateInverse(MapPoint point) const
{
  // PROJ's poly inverse, on the unit ellipsoid: Newton's method on the
  // latitude, then the longitude from it (where the asin allows; Newton's
  // method on project() then makes it exact).
  const double a = wgs84::semiMajorAxisM;
  const double es = wgs84::eccentricitySquared;
  const double x = point.xM / a;
  const double y = (point.yM + m_originArcM) / a;
  if (!(std::isfinite(x) && std::isfinite(y))) {
    return std::nullopt;
  }
  if (std::abs(y) <= 1e-10) {
    return std::array<double, 2>{0.0, std::clamp(x, -pi, pi)};
  }
  const double r = y * y + x * x;
  double phi = y;
  for (int32_t i = 0; i < 20; ++i) {
    const double sp = std::sin(phi);
    const double cp = std::cos(phi);
    if (std::abs(cp) < 1e-12) {
      break;
    }
    const double s2ph = sp * cp;
    double mlp = std::sqrt(1.0 - es * sp * sp);
    const double c = sp * mlp / cp;
    const double ml = meridianArcM(phi) / a;
    const double mlb = ml * ml + r;
    mlp = (1.0 - es) / (mlp * mlp * mlp);
    const double dPhi = (ml + ml + c * mlb - 2.0 * y * (c * ml + 1.0)) /
                        (es * s2ph * (mlb - 2.0 * y * ml) / c +
                         2.0 * (y - ml) * (c * mlp - 1.0 / s2ph) - mlp - mlp);
    if (!std::isfinite(dPhi)) {
      break;
    }
    phi = std::clamp(phi + dPhi, -halfPi(), halfPi());
    if (std::abs(dPhi) <= 1e-12) {
      break;
    }
  }
  const double sp = std::sin(phi);
  const double arg =
      std::clamp(x * std::tan(phi) * std::sqrt(1.0 - es * sp * sp), -1.0, 1.0);
  const double lam = std::abs(sp) > 1e-12 ? std::asin(arg) / sp : x;
  return std::array<double, 2>{phi, std::clamp(lam, -pi, pi)};
}

QByteArray PolyconicProjection::glslForwardSource() const
{
  // The same in float; the meridian arc with its constants written in.
  const MeridianSeries& m = meridianSeries();
  return "\nfloat " + glslFunction() +
         "Sinc(float z)\n{\n"
         "  return abs(z) < 1e-3 ? 1.0 - z * z / 6.0 : sin(z) / z;\n"
         "}\n"
         "\nvec2 " +
         glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float phi = lonLatRad.y;\n"
         "  float s = sin(phi);\n"
         "  float nCos = " +
         number(wgs84::semiMajorAxisM) + " * cos(phi) / sqrt(1.0 - " +
         number(wgs84::eccentricitySquared) +
         " * s * s);\n"
         "  float z = lonLatRad.x * s;\n"
         "  float arc = " +
         number(m.rectifyingRadius) + " * (phi + " + number(m.arc[0]) +
         " * sin(2.0 * phi) + " + number(m.arc[1]) +
         " * sin(4.0 * phi)\n"
         "              + " +
         number(m.arc[2]) +
         " * sin(6.0 * phi));\n"
         "  return vec2(nCos * lonLatRad.x * " +
         glslFunction() +
         "Sinc(z),\n"
         "              arc + nCos * lonLatRad.x * sin(0.5 * z) * " +
         glslFunction() + "Sinc(0.5 * z));\n}\n";
}

std::array<std::array<float, 4>, 2> PolyconicProjection::glslParameters() const
{
  return {std::array<float, 4>{}, std::array<float, 4>{}};
}

} // namespace ts
