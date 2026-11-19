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
#include "ts/projection/transversemercator.h"

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

/** WGS 84's eccentricity e and third flattening n. */
struct Ellipsoid {
  double e = 0.0;
  double n = 0.0;
  /** a / (1 + n) (1 + n²/4 + n⁴/64 + n⁶/256): the rectifying radius. */
  double rectifyingRadius = 0.0;
  /** Krüger's α1…α6 (forward) and β1…β6 (inverse). */
  std::array<double, 6> alpha{};
  std::array<double, 6> beta{};
  /** The meridian arc's sin 2jφ coefficients (j = 1…4), and those of
   *  its inverse (the footpoint latitude). */
  std::array<double, 4> arc{};
  std::array<double, 4> footpoint{};
};

const Ellipsoid& wgs84Series()
{
  static const Ellipsoid series = []() {
    Ellipsoid s;
    s.e = std::sqrt(wgs84::eccentricitySquared);
    const double f = 1.0 - wgs84::semiMinorAxisM / wgs84::semiMajorAxisM;
    const double n = f / (2.0 - f);
    s.n = n;
    const double n2 = n * n;
    const double n3 = n2 * n;
    const double n4 = n3 * n;
    const double n5 = n4 * n;
    const double n6 = n5 * n;
    s.rectifyingRadius = wgs84::semiMajorAxisM / (1.0 + n) *
                         (1.0 + n2 / 4.0 + n4 / 64.0 + n6 / 256.0);
    // Karney (2011), eqs. 35 and 36.
    s.alpha = {n / 2.0 - 2.0 / 3.0 * n2 + 5.0 / 16.0 * n3 + 41.0 / 180.0 * n4 -
                   127.0 / 288.0 * n5 + 7891.0 / 37800.0 * n6,
               13.0 / 48.0 * n2 - 3.0 / 5.0 * n3 + 557.0 / 1440.0 * n4 +
                   281.0 / 630.0 * n5 - 1983433.0 / 1935360.0 * n6,
               61.0 / 240.0 * n3 - 103.0 / 140.0 * n4 + 15061.0 / 26880.0 * n5 +
                   167603.0 / 181440.0 * n6,
               49561.0 / 161280.0 * n4 - 179.0 / 168.0 * n5 +
                   6601661.0 / 7257600.0 * n6,
               34729.0 / 80640.0 * n5 - 3418889.0 / 1995840.0 * n6,
               212378941.0 / 319334400.0 * n6};
    s.beta = {n / 2.0 - 2.0 / 3.0 * n2 + 37.0 / 96.0 * n3 - 1.0 / 360.0 * n4 -
                  81.0 / 512.0 * n5 + 96199.0 / 604800.0 * n6,
              1.0 / 48.0 * n2 + 1.0 / 15.0 * n3 - 437.0 / 1440.0 * n4 +
                  46.0 / 105.0 * n5 - 1118711.0 / 3870720.0 * n6,
              17.0 / 480.0 * n3 - 37.0 / 840.0 * n4 - 209.0 / 4480.0 * n5 +
                  5569.0 / 90720.0 * n6,
              4397.0 / 161280.0 * n4 - 11.0 / 504.0 * n5 -
                  830251.0 / 7257600.0 * n6,
              4583.0 / 161280.0 * n5 - 108847.0 / 3991680.0 * n6,
              20648693.0 / 638668800.0 * n6};
    // The meridian arc M = A (φ + Σ arc_j sin 2jφ), and the footpoint
    // φ = μ + Σ footpoint_j sin 2jμ with μ = M / A (Helmert's series).
    s.arc = {-(1.5 * n - 9.0 / 16.0 * n3), 15.0 / 16.0 * n2 - 15.0 / 32.0 * n4,
             -35.0 / 48.0 * n3, 315.0 / 512.0 * n4};
    s.footpoint = {1.5 * n - 27.0 / 32.0 * n3,
                   21.0 / 16.0 * n2 - 55.0 / 32.0 * n4, 151.0 / 96.0 * n3,
                   1097.0 / 512.0 * n4};
    return s;
  }();
  return series;
}

/** The meridian arc from the equator to @p latRad, metres. */
double meridianArcM(double latRad)
{
  const Ellipsoid& s = wgs84Series();
  double sum = latRad;
  for (std::size_t j = 0; j < s.arc.size(); ++j) {
    sum += s.arc[j] * std::sin(2.0 * static_cast<double>(j + 1) * latRad);
  }
  return s.rectifyingRadius * sum;
}

/** The latitude whose meridian arc is @p arcM (the footpoint), radians. */
double footpointLatRad(double arcM)
{
  const Ellipsoid& s = wgs84Series();
  const double mu = arcM / s.rectifyingRadius;
  double lat = mu;
  for (std::size_t j = 0; j < s.footpoint.size(); ++j) {
    lat += s.footpoint[j] * std::sin(2.0 * static_cast<double>(j + 1) * mu);
  }
  return lat;
}

/** The tangent of the conformal latitude for tan φ = @p tau. */
double conformalTau(double tau)
{
  const double e = wgs84Series().e;
  const double sigma =
      std::sinh(e * std::atanh(e * tau / std::sqrt(1.0 + tau * tau)));
  return tau * std::sqrt(1.0 + sigma * sigma) -
         sigma * std::sqrt(1.0 + tau * tau);
}

/** tan φ from the tangent of the conformal latitude (Karney, eq. 19–21). */
double geographicTau(double taup)
{
  const double e2 = wgs84::eccentricitySquared;
  double tau = taup;
  for (int32_t i = 0; i < 6; ++i) {
    const double taupi = conformalTau(tau);
    const double dtau = (taup - taupi) / std::sqrt(1.0 + taupi * taupi) *
                        (1.0 + (1.0 - e2) * tau * tau) /
                        ((1.0 - e2) * std::sqrt(1.0 + tau * tau));
    tau += dtau;
    if (!(std::abs(dtau) >= 1e-14 * std::max(1.0, std::abs(tau)))) {
      break;
    }
  }
  return tau;
}

QByteArray number(double value)
{
  return QByteArray::number(value, 'g', 9);
}

ProjectionParameter centralMeridianParameter()
{
  return ProjectionParameter{
      QStringLiteral("centralMeridian"),
      QCoreApplication::translate("ts::Projection", "Central meridian"),
      0.0,
      -180.0,
      180.0,
      QStringLiteral("°"),
      4};
}

} // namespace

// --- Transverse Mercator -----------------------------------------------------

TransverseMercatorProjection::TransverseMercatorProjection(
    double centralMeridianDeg, double scaleFactor)
    : LocalExpansionProjection(centralMeridianDeg), m_scaleFactor(scaleFactor)
{
}

QString TransverseMercatorProjection::id() const
{
  return QStringLiteral("transverse-mercator");
}

QString TransverseMercatorProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Transverse Mercator");
}

QString TransverseMercatorProjection::projString() const
{
  return QStringLiteral("+proj=tmerc +lat_0=0 +lon_0=%1 +k_0=%2 +x_0=0 "
                        "+y_0=0 +ellps=WGS84")
      .arg(centralMeridianDeg(), 0, 'g', 17)
      .arg(m_scaleFactor, 0, 'g', 17);
}

std::vector<ProjectionParameter>
TransverseMercatorProjection::parameters() const
{
  return {centralMeridianParameter(),
          ProjectionParameter{
              QStringLiteral("scaleFactor"),
              QCoreApplication::translate(
                  "ts::Projection", "Scale factor on the central meridian"),
              1.0, 0.5, 1.5, QString(), 6}};
}

ProjectionParameterValues TransverseMercatorProjection::parameterValues() const
{
  return {{QStringLiteral("centralMeridian"), centralMeridianDeg()},
          {QStringLiteral("scaleFactor"), m_scaleFactor}};
}

QByteArray TransverseMercatorProjection::glslFunction() const
{
  return "tsProjectTransverseMercator";
}

MapPoint TransverseMercatorProjection::project(double latRad,
                                               double dLonRad) const
{
  // Karney (2011): the conformal latitude, the spherical transverse
  // Mercator of it (ξ', η'), then Krüger's series.
  const Ellipsoid& s = wgs84Series();
  const double taup = conformalTau(std::tan(latRad));
  const double cosLon = std::cos(dLonRad);
  const double xip = std::atan2(taup, cosLon);
  const double etap = std::asinh(std::sin(dLonRad) / std::hypot(taup, cosLon));
  double xi = xip;
  double eta = etap;
  for (std::size_t j = 0; j < s.alpha.size(); ++j) {
    const double k = 2.0 * static_cast<double>(j + 1);
    xi += s.alpha[j] * std::sin(k * xip) * std::cosh(k * etap);
    eta += s.alpha[j] * std::cos(k * xip) * std::sinh(k * etap);
  }
  const double scale = m_scaleFactor * s.rectifyingRadius;
  return MapPoint{scale * eta, scale * xi};
}

std::optional<std::array<double, 2>>
TransverseMercatorProjection::approximateInverse(MapPoint point) const
{
  const Ellipsoid& s = wgs84Series();
  const double scale = m_scaleFactor * s.rectifyingRadius;
  const double xi = point.yM / scale;
  const double eta = point.xM / scale;
  if (std::abs(xi) > 0.5 * pi * 1.01 || std::abs(eta) > 3.0) {
    return std::nullopt;
  }
  double xip = xi;
  double etap = eta;
  for (std::size_t j = 0; j < s.beta.size(); ++j) {
    const double k = 2.0 * static_cast<double>(j + 1);
    xip -= s.beta[j] * std::sin(k * xi) * std::cosh(k * eta);
    etap -= s.beta[j] * std::cos(k * xi) * std::sinh(k * eta);
  }
  const double sinhEta = std::sinh(etap);
  const double cosXi = std::cos(xip);
  const double taup = std::sin(xip) / std::hypot(sinhEta, cosXi);
  const double lon = std::atan2(sinhEta, cosXi);
  return std::array<double, 2>{std::atan(geographicTau(taup)), lon};
}

QByteArray TransverseMercatorProjection::glslForwardSource() const
{
  // Float: three terms of Krüger's series are plenty. cos φ is held off
  // zero so that the poles stay finite. tsAtan2 and tsAsinh: GLSL's own
  // can be out by tens of metres here.
  const Ellipsoid& s = wgs84Series();
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float s = sin(lonLatRad.y);\n"
         "  float c = max(cos(lonLatRad.y), 1e-6);\n"
         "  float tau = s / c;\n"
         "  float sigma = sinh(" +
         number(s.e) + " * tsAtanh(" + number(s.e) +
         " * s));\n"
         "  float taup = tau * sqrt(1.0 + sigma * sigma) - sigma / c;\n"
         "  float cl = cos(lonLatRad.x);\n"
         "  float xip = tsAtan2(taup, cl);\n"
         "  float etap = tsAsinh(sin(lonLatRad.x) / sqrt(taup * taup + cl * "
         "cl));\n"
         "  vec2 r = vec2(etap, xip);\n"
         "  r += " +
         number(s.alpha[0]) +
         " * vec2(cos(2.0 * xip) * sinh(2.0 * etap),\n"
         "                     sin(2.0 * xip) * cosh(2.0 * etap));\n"
         "  r += " +
         number(s.alpha[1]) +
         " * vec2(cos(4.0 * xip) * sinh(4.0 * etap),\n"
         "                     sin(4.0 * xip) * cosh(4.0 * etap));\n"
         "  r += " +
         number(s.alpha[2]) +
         " * vec2(cos(6.0 * xip) * sinh(6.0 * etap),\n"
         "                     sin(6.0 * xip) * cosh(6.0 * etap));\n"
         "  return p0.x * r;\n"
         "}\n";
}

std::array<std::array<float, 4>, 2>
TransverseMercatorProjection::glslParameters() const
{
  return {
      std::array<float, 4>{
          static_cast<float>(m_scaleFactor * wgs84Series().rectifyingRadius),
          0.0F, 0.0F, 0.0F},
      std::array<float, 4>{}};
}

// --- UTM
// -----------------------------------------------------------------------

UtmProjection::UtmProjection(int32_t zone)
    : TransverseMercatorProjection(
          zoneCentralMeridianDeg(std::clamp(zone, 1, 60)), 0.9996),
      m_zone(std::clamp(zone, 1, 60))
{
}

double UtmProjection::zoneCentralMeridianDeg(int32_t zone)
{
  return 6.0 * std::clamp(zone, 1, 60) - 183.0;
}

QString UtmProjection::id() const
{
  return QStringLiteral("utm");
}

QString UtmProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection",
                                     "Universal Transverse Mercator (UTM)");
}

std::vector<ProjectionParameter> UtmProjection::parameters() const
{
  return {
      ProjectionParameter{QStringLiteral("zone"),
                          QCoreApplication::translate("ts::Projection", "Zone"),
                          30.0, 1.0, 60.0, QString(), 0}};
}

ProjectionParameterValues UtmProjection::parameterValues() const
{
  return {{QStringLiteral("zone"), static_cast<double>(m_zone)}};
}

QByteArray UtmProjection::glslFunction() const
{
  return "tsProjectUtm";
}

// --- Cassini
// -------------------------------------------------------------------

CassiniProjection::CassiniProjection(double centralMeridianDeg)
    : LocalExpansionProjection(centralMeridianDeg)
{
}

QString CassiniProjection::id() const
{
  return QStringLiteral("cassini");
}

QString CassiniProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Cassini–Soldner");
}

QString CassiniProjection::projString() const
{
  return QStringLiteral("+proj=cass +lat_0=0 +lon_0=%1 +x_0=0 +y_0=0 "
                        "+ellps=WGS84")
      .arg(centralMeridianDeg(), 0, 'g', 17);
}

std::vector<ProjectionParameter> CassiniProjection::parameters() const
{
  return {centralMeridianParameter()};
}

ProjectionParameterValues CassiniProjection::parameterValues() const
{
  return {{QStringLiteral("centralMeridian"), centralMeridianDeg()}};
}

QByteArray CassiniProjection::glslFunction() const
{
  return "tsProjectCassini";
}

MapPoint CassiniProjection::project(double latRad, double dLonRad) const
{
  // Snyder (1987), eqs. 3-11 to 3-14, as PROJ's cass:
  //   x = N (A − T A³/6 − (8 − T + 8C) T A⁵/120)
  //   y = M + N tan φ (A²/2 + (5 − T + 6C) A⁴/24)
  const double es = wgs84::eccentricitySquared;
  const double sine = std::sin(latRad);
  const double cosine = std::cos(latRad);
  const double primeVertical =
      wgs84::semiMajorAxisM / std::sqrt(1.0 - es * sine * sine);
  const double tn = std::tan(latRad);
  const double t = tn * tn;
  const double a1 = dLonRad * cosine;
  const double c = es * cosine * cosine / (1.0 - es);
  const double a2 = a1 * a1;
  const double x =
      primeVertical * a1 *
      (1.0 - a2 * t * (1.0 / 6.0 + (8.0 - t + 8.0 * c) * a2 / 120.0));
  const double y =
      meridianArcM(latRad) +
      primeVertical * tn * a2 * (0.5 + (5.0 - t + 6.0 * c) * a2 / 24.0);
  return MapPoint{x, y};
}

std::optional<std::array<double, 2>>
CassiniProjection::approximateInverse(MapPoint point) const
{
  // Snyder's inverse series, as PROJ's.
  if (std::abs(point.yM) > 1.01 * meridianArcM(0.5 * pi) ||
      std::abs(point.xM) > wgs84::semiMajorAxisM * 2.0) {
    return std::nullopt;
  }
  const double es = wgs84::eccentricitySquared;
  const double ph1 = footpointLatRad(point.yM);
  const double tn = std::tan(ph1);
  const double t = tn * tn;
  const double sine = std::sin(ph1);
  double r = 1.0 / (1.0 - es * sine * sine);
  const double n = std::sqrt(r);
  r *= (1.0 - es) * n;
  const double dd = point.xM / (wgs84::semiMajorAxisM * n);
  const double d2 = dd * dd;
  const double lat =
      ph1 - (n * tn / r) * d2 * (0.5 - (1.0 + 3.0 * t) * d2 / 24.0);
  const double lon =
      dd * (1.0 + t * d2 * (-1.0 / 3.0 + (1.0 + 3.0 * t) * d2 / 15.0)) /
      std::cos(ph1);
  return std::array<double, 2>{lat, lon};
}

QByteArray CassiniProjection::glslForwardSource() const
{
  const Ellipsoid& s = wgs84Series();
  const double es = wgs84::eccentricitySquared;
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float phi = lonLatRad.y;\n"
         "  float s = sin(phi);\n"
         "  float c = max(cos(phi), 1e-6);\n"
         "  float n = " +
         number(wgs84::semiMajorAxisM) + " / sqrt(1.0 - " + number(es) +
         " * s * s);\n"
         "  float tn = s / c;\n"
         "  float t = tn * tn;\n"
         "  float a1 = lonLatRad.x * c;\n"
         "  float cc = " +
         number(es / (1.0 - es)) +
         " * c * c;\n"
         "  float a2 = a1 * a1;\n"
         "  float m = " +
         number(s.rectifyingRadius) + " * (phi + " + number(s.arc[0]) +
         " * sin(2.0 * phi) + " + number(s.arc[1]) +
         " * sin(4.0 * phi)\n"
         "            + " +
         number(s.arc[2]) +
         " * sin(6.0 * phi));\n"
         "  return vec2(n * a1 * (1.0 - a2 * t * (1.0 / 6.0\n"
         "                + (8.0 - t + 8.0 * cc) * a2 / 120.0)),\n"
         "              m + n * tn * a2 * (0.5 + (5.0 - t + 6.0 * cc) * a2 / "
         "24.0));\n"
         "}\n";
}

std::array<std::array<float, 4>, 2> CassiniProjection::glslParameters() const
{
  return {std::array<float, 4>{}, std::array<float, 4>{}};
}

} // namespace ts
