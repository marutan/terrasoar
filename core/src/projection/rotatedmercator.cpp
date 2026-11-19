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
#include "ts/projection/rotatedmercator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <QCoreApplication>

#include "ts/geo/geodesic.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** PROJ omerc's tolerance for the poles. */
constexpr double epsilon = 1e-10;

/** The sine of the largest rotated latitude computed (89.9°): beyond it
 *  the map is held finite. */
constexpr double sinRotatedLimit = 0.99999847691328769;

double halfPi()
{
  return 0.5 * pi;
}

/** Wraps @p angleRad to [−π, π]. */
double wrapRad(double angleRad)
{
  return std::remainder(angleRad, 2.0 * pi);
}

/** Snyder's t(φ) = exp(−ψ), ψ the isometric latitude (PROJ's pj_tsfn). */
// A latitude, then the eccentricity.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double tsfn(double phi, double e)
{
  const double s = std::sin(phi);
  const double c = std::cos(phi);
  return std::exp(e * std::atanh(e * s)) *
         (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c);
}

/** The latitude whose tsfn() is @p ts (PROJ's pj_phi2). */
double phiOfTs(double ts, double e)
{
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

QByteArray number(double value)
{
  return QByteArray::number(value, 'g', 9);
}

double eccentricity()
{
  return std::sqrt(wgs84::eccentricitySquared);
}

} // namespace

RotatedMercatorProjection::RotatedMercatorProjection(
    // Latitude, longitude, azimuth and scale: the order of PROJ's.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    double centreLatDeg, double centreLonDeg,
    // An angle, then a factor.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    double azimuthDeg, double scaleFactor)
    : LocalExpansionProjection(centreLonDeg), m_centreLatDeg(centreLatDeg),
      m_azimuthDeg(azimuthDeg), m_scaleFactor(scaleFactor)
{
  // PROJ's omerc set-up, with alpha and lonc given (gamma = alpha, the
  // u offset kept), on the unit ellipsoid.
  const double es = wgs84::eccentricitySquared;
  const double e = eccentricity();
  const double com = std::sqrt(1.0 - es);
  const double phi0 = degToRad(centreLatDeg);
  const double alpha = degToRad(azimuthDeg);
  double d = 1.0;
  double f = 1.0;
  if (std::abs(phi0) > epsilon) {
    const double sinph0 = std::sin(phi0);
    const double cosph0 = std::cos(phi0);
    const double con = 1.0 - es * sinph0 * sinph0;
    m_b = std::sqrt(1.0 + es * std::pow(cosph0, 4.0) / (1.0 - es));
    m_a = m_b * scaleFactor * com / con;
    d = m_b * com / (cosph0 * std::sqrt(con));
    f = d * d - 1.0;
    if (f <= 0.0) {
      f = 0.0;
    } else {
      f = std::sqrt(f);
      if (phi0 < 0.0) {
        f = -f;
      }
    }
    f += d;
    m_e = f * std::pow(tsfn(phi0, e), m_b);
  } else {
    m_b = 1.0 / com;
    m_a = scaleFactor;
    m_e = 1.0;
  }
  const double gamma0 = std::asin(std::clamp(std::sin(alpha) / d, -1.0, 1.0));
  const double lam0 =
      degToRad(centreLonDeg) -
      std::asin(std::clamp(0.5 * (f - 1.0 / f) * std::tan(gamma0), -1.0, 1.0)) /
          m_b;
  m_lam0OffsetRad = wrapRad(lam0 - degToRad(centreLonDeg));
  m_singam = std::sin(gamma0);
  m_cosgam = std::cos(gamma0);
  m_sinrot = std::sin(alpha);
  m_cosrot = std::cos(alpha);
  m_arb = m_a / m_b;
  m_u0 = std::abs(m_arb * std::atan(std::sqrt(std::max(d * d - 1.0, 0.0)) /
                                    std::cos(alpha)));
  if (phi0 < 0.0) {
    m_u0 = -m_u0;
  }
  m_centreAlongRad = m_u0 / m_arb;
  m_sinCentre = std::sin(m_centreAlongRad);
  m_cosCentre = std::cos(m_centreAlongRad);
  m_vPoleN = m_arb * std::log(std::tan(0.25 * pi - 0.5 * gamma0));
  m_vPoleS = m_arb * std::log(std::tan(0.25 * pi + 0.5 * gamma0));
}

QString RotatedMercatorProjection::id() const
{
  return QStringLiteral("rotated-mercator");
}

QString RotatedMercatorProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection",
                                     "Rotated Mercator (Oblique Mercator)");
}

QString RotatedMercatorProjection::projString() const
{
  return QStringLiteral("+proj=omerc +lat_0=%1 +lonc=%2 +alpha=%3 +k_0=%4 "
                        "+x_0=0 +y_0=0 +ellps=WGS84")
      .arg(m_centreLatDeg, 0, 'g', 17)
      .arg(centralMeridianDeg(), 0, 'g', 17)
      .arg(m_azimuthDeg, 0, 'g', 17)
      .arg(m_scaleFactor, 0, 'g', 17);
}

std::vector<ProjectionParameter> RotatedMercatorProjection::parameters() const
{
  return {
      ProjectionParameter{
          QStringLiteral("centreLatitude"),
          QCoreApplication::translate("ts::Projection", "Centre latitude"),
          45.0, -80.0, 80.0, QStringLiteral("°"), 4},
      ProjectionParameter{
          QStringLiteral("centreLongitude"),
          QCoreApplication::translate("ts::Projection", "Centre longitude"),
          0.0, -180.0, 180.0, QStringLiteral("°"), 4},
      ProjectionParameter{QStringLiteral("azimuth"),
                          QCoreApplication::translate(
                              "ts::Projection", "Azimuth of the centre line"),
                          45.0, -90.0, 90.0, QStringLiteral("°"), 4},
      ProjectionParameter{
          QStringLiteral("scaleFactor"),
          QCoreApplication::translate("ts::Projection",
                                      "Scale factor on the centre line"),
          1.0, 0.5, 1.5, QString(), 6}};
}

ProjectionParameterValues RotatedMercatorProjection::parameterValues() const
{
  return {{QStringLiteral("centreLatitude"), m_centreLatDeg},
          {QStringLiteral("centreLongitude"), centralMeridianDeg()},
          {QStringLiteral("azimuth"), m_azimuthDeg},
          {QStringLiteral("scaleFactor"), m_scaleFactor}};
}

QByteArray RotatedMercatorProjection::glslFunction() const
{
  return "tsProjectRotatedMercator";
}

RotatedMercatorProjection::Native
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RotatedMercatorProjection::native(double latRad, double dLonRad) const
{
  // PROJ's omerc forward on the unit ellipsoid, its own case at the
  // geographic poles included; the rotated latitude held within 89.9° so
  // the map stays finite.
  Native result;
  if (std::abs(std::abs(latRad) - halfPi()) <= epsilon) {
    result.v = latRad > 0.0 ? m_vPoleN : m_vPoleS;
    result.sinAcross = -std::tanh(result.v / m_arb);
    result.alongRad = wrapRad(latRad - m_centreAlongRad);
    result.u = m_arb * result.alongRad;
    return result;
  }
  const double phi = latRad;
  const double lam = wrapRad(dLonRad - m_lam0OffsetRad);
  const double w = m_e / std::pow(tsfn(phi, eccentricity()), m_b);
  const double s = 0.5 * (w - 1.0 / w);
  const double t = 0.5 * (w + 1.0 / w);
  const double v = std::sin(m_b * lam);
  const double u = std::clamp((s * m_singam - v * m_cosgam) / t,
                              -sinRotatedLimit, sinRotatedLimit);
  result.sinAcross = u;
  result.v = 0.5 * m_arb * std::log((1.0 - u) / (1.0 + u));
  // The angle along the line from the centre, so that the cut (where it
  // passes ±180°) is opposite the centre: PROJ's angle, measured from
  // where the line crosses the aposphere's equator, less the centre's.
  // Always atan2: PROJ's u = A λ where cos Bλ is nearly 0 puts both sides
  // of the rotated pole on its meridian at the same place.
  const double y = s * m_cosgam + v * m_singam;
  const double x = std::cos(m_b * lam);
  result.alongRad = std::atan2(y * m_cosCentre - x * m_sinCentre,
                               x * m_cosCentre + y * m_sinCentre);
  result.lamRad = lam;
  result.u = m_arb * result.alongRad;
  return result;
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint RotatedMercatorProjection::project(double latRad, double dLonRad) const
{
  // PROJ's u − u0 is n.u, a whole turn of the line apart beyond PROJ's
  // cut (alongTurnM()).
  const Native n = native(latRad, dLonRad);
  const double u = n.u;
  return MapPoint{wgs84::semiMajorAxisM * (n.v * m_cosrot + u * m_sinrot),
                  wgs84::semiMajorAxisM * (u * m_cosrot - n.v * m_sinrot)};
}

std::optional<std::array<double, 2>>
RotatedMercatorProjection::approximateInverse(MapPoint point) const
{
  // PROJ's omerc inverse (exact but for the latitude's iteration).
  const double x = point.xM / wgs84::semiMajorAxisM;
  const double y = point.yM / wgs84::semiMajorAxisM;
  const double v = x * m_cosrot - y * m_sinrot;
  const double u = y * m_cosrot + x * m_sinrot + m_u0;
  const double bra = 1.0 / m_arb;
  const double q = std::exp(-bra * v);
  if (!std::isfinite(q) || q <= 0.0) {
    return std::nullopt;
  }
  const double sp = 0.5 * (q - 1.0 / q);
  const double tp = 0.5 * (q + 1.0 / q);
  const double vp = std::sin(bra * u);
  const double up = (vp * m_cosgam + sp * m_singam) / tp;
  if (std::abs(std::abs(up) - 1.0) < epsilon) {
    return std::array<double, 2>{up < 0.0 ? -halfPi() : halfPi(), 0.0};
  }
  const double ts = m_e / std::sqrt((1.0 + up) / (1.0 - up));
  const double lat = phiOfTs(std::pow(ts, 1.0 / m_b), eccentricity());
  const double lam =
      -std::atan2(sp * m_cosgam - vp * m_singam, std::cos(bra * u)) / m_b;
  return std::array<double, 2>{lat, wrapRad(lam + m_lam0OffsetRad)};
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> RotatedMercatorProjection::rotatedDeg(double latDeg,
                                                            double lonDeg) const
{
  const Native n =
      native(degToRad(latDeg),
             degToRad(normaliseLonDeg(lonDeg - centralMeridianDeg())));
  return {radToDeg(-std::asin(n.sinAcross)), radToDeg(n.alongRad)};
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double RotatedMercatorProjection::domainMarginDeg(double latDeg,
                                                  double lonDeg) const
{
  const Native n =
      native(degToRad(latDeg),
             degToRad(normaliseLonDeg(lonDeg - centralMeridianDeg())));
  const double acrossRad = std::asin(n.sinAcross);
  const double across = acrossLimitDeg() - radToDeg(std::abs(acrossRad));
  // Along the line, as an angle on the Earth: shorter nearer the poles.
  const double along =
      (alongLimitDeg() - radToDeg(std::abs(n.alongRad))) * std::cos(acrossRad);
  // B > 1: the aposphere's longitude Bλ wraps round before λ does, and
  // beyond π / B the map would cover itself again.
  const double wrap =
      radToDeg(pi / m_b - std::abs(n.lamRad)) * std::cos(degToRad(latDeg));
  return std::min({across, along, wrap});
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Geodetic RotatedMercatorProjection::clampToDomain(double latDeg,
                                                  double lonDeg) const
{
  if (domainMarginDeg(latDeg, lonDeg) >= 0.0) {
    return Geodetic{latDeg, normaliseLonDeg(lonDeg), 0.0};
  }
  // Clamped in the rotated frame, then back: u and v before the
  // rectification.
  const std::array<double, 2> rotated = rotatedDeg(latDeg, lonDeg);
  const double across =
      degToRad(std::clamp(rotated[0], -acrossLimitDeg(), acrossLimitDeg()));
  const double along =
      degToRad(std::clamp(rotated[1], -alongLimitDeg(), alongLimitDeg()));
  const double sinAcross = -std::sin(across);
  const double v =
      0.5 * m_arb * std::log((1.0 - sinAcross) / (1.0 + sinAcross));
  const double u = m_arb * along;
  const std::optional<std::array<double, 2>> point = approximateInverse(
      MapPoint{wgs84::semiMajorAxisM * (v * m_cosrot + u * m_sinrot),
               wgs84::semiMajorAxisM * (u * m_cosrot - v * m_sinrot)});
  if (!point) {
    return Geodetic{latDeg, normaliseLonDeg(lonDeg), 0.0};
  }
  return Geodetic{radToDeg((*point)[0]),
                  normaliseLonDeg(centralMeridianDeg() + radToDeg((*point)[1])),
                  0.0};
}

MapPoint RotatedMercatorProjection::alongTurnM() const
{
  const double turn = wgs84::semiMajorAxisM * m_arb * 2.0 * pi;
  return MapPoint{turn * m_sinrot, turn * m_cosrot};
}

MapRect RotatedMercatorProjection::bounds() const
{
  // The domain is a rectangle in u and v: its corners, turned.
  const double vLimit = 0.5 * m_arb *
                        std::log((1.0 + std::sin(degToRad(acrossLimitDeg()))) /
                                 (1.0 - std::sin(degToRad(acrossLimitDeg()))));
  const double uLimit = m_arb * degToRad(alongLimitDeg());
  MapRect box{std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max(),
              std::numeric_limits<double>::lowest(),
              std::numeric_limits<double>::lowest()};
  for (const double v : {-vLimit, vLimit}) {
    for (const double along : {-uLimit, uLimit}) {
      const double u = along;
      const double x = wgs84::semiMajorAxisM * (v * m_cosrot + u * m_sinrot);
      const double y = wgs84::semiMajorAxisM * (u * m_cosrot - v * m_sinrot);
      box.westM = std::min(box.westM, x);
      box.eastM = std::max(box.eastM, x);
      box.southM = std::min(box.southM, y);
      box.northM = std::max(box.northM, y);
    }
  }
  return box;
}

std::array<float, 8> RotatedMercatorProjection::glslDomainParameters() const
{
  return {static_cast<float>(m_b),
          static_cast<float>(m_e),
          static_cast<float>(m_singam),
          static_cast<float>(m_cosgam),
          static_cast<float>(
              wrapRad(degToRad(centralMeridianDeg()) + m_lam0OffsetRad)),
          static_cast<float>(std::sin(degToRad(acrossLimitDeg()))),
          static_cast<float>(degToRad(alongLimitDeg())),
          static_cast<float>(m_centreAlongRad)};
}

QByteArray RotatedMercatorProjection::glslDomainSource() const
{
  // d0: B, E, sin γ0, cos γ0; d1: λ0 (absolute), the sine of the limit
  // across, the limit along and the centre's angle along (radians). The test
  // the CPU makes, in float.
  return "\nbool " + glslFunction() +
         "Outside(vec2 lonLatRad, vec4 d0, vec4 d1)\n{\n"
         "  float lam = lonLatRad.x - d1.x;\n"
         "  lam -= 6.28318531 * floor((lam + 3.14159265) / 6.28318531);\n"
         "  float phi = clamp(lonLatRad.y, -1.5707953, 1.5707953);\n"
         "  float s = sin(phi);\n"
         "  float c = cos(phi);\n"
         "  float es = " +
         number(eccentricity()) +
         " * s;\n"
         "  float ts = exp(" +
         number(0.5 * eccentricity()) +
         " * log((1.0 + es) / (1.0 - es)))\n"
         "           * (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c);\n"
         "  float w = d0.y / pow(max(ts, 1e-20), d0.x);\n"
         "  float sh = 0.5 * (w - 1.0 / w);\n"
         "  float ch = 0.5 * (w + 1.0 / w);\n"
         "  float v = sin(d0.x * lam);\n"
         "  if (abs(lam) > 3.14159265 / d0.x\n"
         "      || abs((sh * d0.z - v * d0.w) / ch) > d1.y) {\n"
         "    return true;\n"
         "  }\n"
         "  float y = sh * d0.w + v * d0.z;\n"
         "  float x = cos(d0.x * lam);\n"
         "  float sc = sin(d1.w);\n"
         "  float cc = cos(d1.w);\n"
         "  return abs(atan(y * cc - x * sc, x * cc + y * sc)) > d1.z;\n"
         "}\n";
}

QByteArray RotatedMercatorProjection::glslForwardSource() const
{
  // p0: B, E, A / B, λ0 minus the centre's longitude; p1: γ0, the
  // rectification and the centre's angle along the line (radians). The
  // map's offset cancels in the differences the shader takes. tsAtanh and
  // tsAtan2: GLSL's own may be far from float precision.
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float lam = lonLatRad.x - p0.w;\n"
         "  lam -= 6.28318531 * floor((lam + 3.14159265) / 6.28318531);\n"
         "  float phi = clamp(lonLatRad.y, -1.5707953, 1.5707953);\n"
         "  float s = sin(phi);\n"
         "  float c = cos(phi);\n"
         "  float ts = exp(" +
         number(eccentricity()) + " * tsAtanh(" + number(eccentricity()) +
         " * s))\n"
         "           * (s > 0.0 ? c / (1.0 + s) : (1.0 - s) / c);\n"
         "  float w = p0.y / pow(max(ts, 1e-20), p0.x);\n"
         "  float sh = 0.5 * (w - 1.0 / w);\n"
         "  float ch = 0.5 * (w + 1.0 / w);\n"
         "  float v = sin(p0.x * lam);\n"
         "  float sg = sin(p1.x);\n"
         "  float cg = cos(p1.x);\n"
         "  float u = clamp((sh * sg - v * cg) / ch, -" +
         number(sinRotatedLimit) + ", " + number(sinRotatedLimit) +
         ");\n"
         "  float across = -p0.z * tsAtanh(u);\n"
         "  float y = sh * cg + v * sg;\n"
         "  float x = cos(p0.x * lam);\n"
         "  float sc = sin(p1.z);\n"
         "  float cc = cos(p1.z);\n"
         "  float along = p0.z * tsAtan2(y * cc - x * sc, x * cc + y * sc);\n"
         "  float sr = sin(p1.y);\n"
         "  float cr = cos(p1.y);\n"
         "  return " +
         number(wgs84::semiMajorAxisM) +
         " * vec2(across * cr + along * sr, along * cr - across * sr);\n"
         "}\n";
}

std::array<std::array<float, 4>, 2>
RotatedMercatorProjection::glslParameters() const
{
  return {
      std::array<float, 4>{static_cast<float>(m_b), static_cast<float>(m_e),
                           static_cast<float>(m_arb),
                           static_cast<float>(m_lam0OffsetRad)},
      std::array<float, 4>{static_cast<float>(std::atan2(m_singam, m_cosgam)),
                           static_cast<float>(std::atan2(m_sinrot, m_cosrot)),
                           static_cast<float>(m_centreAlongRad), 0.0F}};
}

std::optional<ProjectionParameterValues>
// Two points, each latitude then longitude.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RotatedMercatorProjection::throughPoints(double lat1Deg, double lon1Deg,
                                         double lat2Deg, double lon2Deg)
{
  const GeodesicInverse line =
      geodesicInverse(lat1Deg, lon1Deg, lat2Deg, lon2Deg);
  // Coincident points have no line; nearly antipodal ones no single one.
  if (!std::isfinite(line.distanceM) || line.distanceM <= 1.0 ||
      line.distanceM >= 0.99 * pi * wgs84::semiMinorAxisM) {
    return std::nullopt;
  }
  const GeodesicDirect middle =
      geodesicDirect(lat1Deg, lon1Deg, line.azimuth1Deg, 0.5 * line.distanceM);
  // A line has two directions; the azimuth that points east of north
  // (or due north) is the one in the parameter's range.
  double azimuth = middle.azimuth2Deg;
  if (azimuth > 90.0) {
    azimuth -= 180.0;
  } else if (azimuth <= -90.0) {
    azimuth += 180.0;
  }
  return ProjectionParameterValues{
      {QStringLiteral("centreLatitude"), middle.latDeg},
      {QStringLiteral("centreLongitude"), middle.lonDeg},
      {QStringLiteral("azimuth"), azimuth},
      {QStringLiteral("scaleFactor"), 1.0}};
}

} // namespace ts
