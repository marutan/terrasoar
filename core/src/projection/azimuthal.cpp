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
#include "ts/projection/azimuthal.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>

#include <GeographicLib/AzimuthalEquidistant.hpp>
#include <GeographicLib/Geodesic.hpp>

#include "ellipsoidal.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

using ellipsoidal::glslNumber;

namespace
{

const GeographicLib::AzimuthalEquidistant& wgs84Aeqd()
{
  // Immutable once made: shared by every thread.
  static const GeographicLib::AzimuthalEquidistant aeqd(
      GeographicLib::Geodesic::WGS84());
  return aeqd;
}

double halfPi()
{
  return 0.5 * pi;
}

} // namespace

// --- Centred azimuthals ------------------------------------------------------

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
CentredAzimuthalProjection::CentredAzimuthalProjection(double centreLatDeg,
                                                       double centreLonDeg)
    : LocalExpansionProjection(centreLonDeg), m_centreLatDeg(centreLatDeg)
{
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double CentredAzimuthalProjection::angleFromCentreDeg(double latDeg,
                                                      double lonDeg) const
{
  const double lat0 = degToRad(m_centreLatDeg);
  const double lat = degToRad(latDeg);
  const double cosC = std::sin(lat0) * std::sin(lat) +
                      std::cos(lat0) * std::cos(lat) *
                          std::cos(degToRad(lonDeg - centralMeridianDeg()));
  return radToDeg(std::acos(std::clamp(cosC, -1.0, 1.0)));
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double CentredAzimuthalProjection::domainMarginDeg(double latDeg,
                                                   double lonDeg) const
{
  return domainLimitDeg() - angleFromCentreDeg(latDeg, lonDeg);
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Geodetic CentredAzimuthalProjection::clampToDomain(double latDeg,
                                                   double lonDeg) const
{
  if (domainMarginDeg(latDeg, lonDeg) >= 0.0) {
    return Geodetic{latDeg, normaliseLonDeg(lonDeg), 0.0};
  }
  // Along the great circle from the centre, to the edge.
  const double lat0 = degToRad(m_centreLatDeg);
  const double lat = degToRad(latDeg);
  const double dLon = degToRad(lonDeg - centralMeridianDeg());
  const double bearing =
      std::atan2(std::sin(dLon) * std::cos(lat),
                 std::cos(lat0) * std::sin(lat) -
                     std::sin(lat0) * std::cos(lat) * std::cos(dLon));
  const double reach = degToRad(domainLimitDeg() - 1e-9);
  const double lat2 =
      std::asin(std::sin(lat0) * std::cos(reach) +
                std::cos(lat0) * std::sin(reach) * std::cos(bearing));
  const double lon2 =
      std::atan2(std::sin(bearing) * std::sin(reach) * std::cos(lat0),
                 std::cos(reach) - std::sin(lat0) * std::sin(lat2));
  return Geodetic{radToDeg(lat2),
                  normaliseLonDeg(centralMeridianDeg() + radToDeg(lon2)), 0.0};
}

std::array<float, 8> CentredAzimuthalProjection::glslDomainParameters() const
{
  const double lat0 = degToRad(m_centreLatDeg);
  return {static_cast<float>(std::sin(lat0)),
          static_cast<float>(std::cos(lat0)),
          static_cast<float>(degToRad(centralMeridianDeg())),
          static_cast<float>(std::cos(degToRad(domainLimitDeg()))),
          0.0F,
          0.0F,
          0.0F,
          0.0F};
}

QByteArray CentredAzimuthalProjection::glslDomainSource() const
{
  // d0: sin and cos of the centre's latitude, its longitude (radians),
  // the cosine of the limit. The CPU's test, in float.
  return "\nbool " + glslFunction() +
         "Outside(vec2 lonLatRad, vec4 d0, vec4 d1)\n{\n"
         "  float cosC = d0.x * sin(lonLatRad.y)\n"
         "             + d0.y * cos(lonLatRad.y) * cos(lonLatRad.x - d0.z);\n"
         "  return cosC < d0.w;\n"
         "}\n";
}

std::vector<ProjectionParameter> CentredAzimuthalProjection::parameters() const
{
  const std::array<double, 2> centre = defaultCentreDeg();
  return {ProjectionParameter{
              QStringLiteral("centreLatitude"),
              QCoreApplication::translate("ts::Projection", "Centre latitude"),
              centre[0], -90.0, 90.0, QStringLiteral("°"), 4},
          ProjectionParameter{
              QStringLiteral("centreLongitude"),
              QCoreApplication::translate("ts::Projection", "Centre longitude"),
              centre[1], -180.0, 180.0, QStringLiteral("°"), 4}};
}

ProjectionParameterValues CentredAzimuthalProjection::parameterValues() const
{
  return {{QStringLiteral("centreLatitude"), m_centreLatDeg},
          {QStringLiteral("centreLongitude"), centralMeridianDeg()}};
}

// --- Azimuthal Equidistant ---------------------------------------------------

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
AzimuthalEquidistantProjection::AzimuthalEquidistantProjection(
    double centreLatDeg, double centreLonDeg)
    : CentredAzimuthalProjection(centreLatDeg, centreLonDeg)
{
}

QString AzimuthalEquidistantProjection::id() const
{
  return QStringLiteral("azimuthal-equidistant");
}

QString AzimuthalEquidistantProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Azimuthal Equidistant");
}

QString AzimuthalEquidistantProjection::projString() const
{
  return QStringLiteral("+proj=aeqd +lat_0=%1 +lon_0=%2 +x_0=0 +y_0=0 "
                        "+ellps=WGS84")
      .arg(centreLatDeg(), 0, 'g', 17)
      .arg(centralMeridianDeg(), 0, 'g', 17);
}

QByteArray AzimuthalEquidistantProjection::glslFunction() const
{
  return "tsProjectAzimuthalEquidistant";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint AzimuthalEquidistantProjection::project(double latRad,
                                                 double dLonRad) const
{
  // The geodesic from the centre: its length along its azimuth there.
  double x = 0.0;
  double y = 0.0;
  wgs84Aeqd().Forward(centreLatDeg(), centralMeridianDeg(),
                      std::clamp(radToDeg(latRad), -90.0, 90.0),
                      centralMeridianDeg() + radToDeg(dLonRad), x, y);
  return MapPoint{x, y};
}

std::optional<std::array<double, 2>>
AzimuthalEquidistantProjection::approximateInverse(MapPoint point) const
{
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  double lat = 0.0;
  double lon = 0.0;
  wgs84Aeqd().Reverse(centreLatDeg(), centralMeridianDeg(), point.xM, point.yM,
                      lat, lon);
  return std::array<double, 2>{
      degToRad(lat), degToRad(normaliseLonDeg(lon - centralMeridianDeg()))};
}

QByteArray AzimuthalEquidistantProjection::glslForwardSource() const
{
  // Vincenty's inverse geodesic from the centre, in float: about a metre
  // at the far side of the Earth, against texels of kilometres there.
  // p0: sin and cos of the centre's reduced latitude.
  const double a = wgs84::semiMajorAxisM;
  const double b = wgs84::semiMinorAxisM;
  const double f = 1.0 - b / a;
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  const float f = " +
         glslNumber(f) +
         ";\n"
         "  float phi = lonLatRad.y;\n"
         "  float t = (1.0 - f) * sin(phi);\n"
         "  float h = inversesqrt(cos(phi) * cos(phi) + t * t);\n"
         "  float sinU2 = t * h;\n"
         "  float cosU2 = cos(phi) * h;\n"
         "  float sinU1 = p0.x;\n"
         "  float cosU1 = p0.y;\n"
         "  float lambda = lonLatRad.x;\n"
         "  float sinSigma = 0.0;\n"
         "  float cosSigma = 1.0;\n"
         "  float sigma = 0.0;\n"
         "  float cos2Alpha = 1.0;\n"
         "  float cos2SigmaM = 0.0;\n"
         "  for (int i = 0; i < 10; ++i) {\n"
         "    float sl = sin(lambda);\n"
         "    float cl = cos(lambda);\n"
         "    float q = cosU1 * sinU2 - sinU1 * cosU2 * cl;\n"
         "    sinSigma = max(sqrt(cosU2 * sl * cosU2 * sl + q * q), 1e-30);\n"
         "    cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cl;\n"
         "    sigma = tsAtan2(sinSigma, cosSigma);\n"
         "    float sinAlpha = cosU1 * cosU2 * sl / sinSigma;\n"
         "    cos2Alpha = 1.0 - sinAlpha * sinAlpha;\n"
         "    cos2SigmaM = cos2Alpha > 1e-12\n"
         "        ? cosSigma - 2.0 * sinU1 * sinU2 / cos2Alpha : 0.0;\n"
         "    float c = f / 16.0 * cos2Alpha * (4.0 + f * (4.0 - 3.0 * "
         "cos2Alpha));\n"
         "    lambda = lonLatRad.x + (1.0 - c) * f * sinAlpha\n"
         "        * (sigma + c * sinSigma * (cos2SigmaM\n"
         "           + c * cosSigma * (-1.0 + 2.0 * cos2SigmaM * "
         "cos2SigmaM)));\n"
         "  }\n"
         "  float u2 = cos2Alpha * " +
         glslNumber((a * a - b * b) / (b * b)) +
         ";\n"
         "  float bigA = 1.0 + u2 / 16384.0 * (4096.0 + u2 * (-768.0\n"
         "               + u2 * (320.0 - 175.0 * u2)));\n"
         "  float bigB = u2 / 1024.0 * (256.0 + u2 * (-128.0 + u2 * (74.0 - "
         "47.0 * u2)));\n"
         "  float m2 = cos2SigmaM * cos2SigmaM;\n"
         "  float dSigma = bigB * sinSigma * (cos2SigmaM + bigB / 4.0\n"
         "      * (cosSigma * (-1.0 + 2.0 * m2) - bigB / 6.0 * cos2SigmaM\n"
         "         * (-3.0 + 4.0 * sinSigma * sinSigma) * (-3.0 + 4.0 * "
         "m2)));\n"
         "  float s = " +
         glslNumber(b) +
         " * bigA * (sigma - dSigma);\n"
         "  float sl = sin(lambda);\n"
         "  float cl = cos(lambda);\n"
         "  float azimuth = tsAtan2(cosU2 * sl, cosU1 * sinU2 - sinU1 * cosU2 "
         "* cl);\n"
         "  return s * vec2(sin(azimuth), cos(azimuth));\n"
         "}\n";
}

std::array<std::array<float, 4>, 2>
AzimuthalEquidistantProjection::glslParameters() const
{
  const double f = 1.0 - wgs84::semiMinorAxisM / wgs84::semiMajorAxisM;
  const double u1 = std::atan((1.0 - f) * std::tan(degToRad(centreLatDeg())));
  return {std::array<float, 4>{static_cast<float>(std::sin(u1)),
                               static_cast<float>(std::cos(u1)), 0.0F, 0.0F},
          std::array<float, 4>{}};
}

// --- Gnomonic ----------------------------------------------------------------

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
GnomonicProjection::GnomonicProjection(double centreLatDeg, double centreLonDeg)
    : CentredAzimuthalProjection(centreLatDeg, centreLonDeg)
{
}

QString GnomonicProjection::id() const
{
  return QStringLiteral("gnomonic");
}

QString GnomonicProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Gnomonic");
}

QString GnomonicProjection::projString() const
{
  return QStringLiteral("+proj=gnom +lat_0=%1 +lon_0=%2 +x_0=0 +y_0=0 "
                        "+R=6378137")
      .arg(centreLatDeg(), 0, 'g', 17)
      .arg(centralMeridianDeg(), 0, 'g', 17);
}

QByteArray GnomonicProjection::glslFunction() const
{
  return "tsProjectGnomonic";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint GnomonicProjection::project(double latRad, double dLonRad) const
{
  // Snyder 22-4 to 22-6 on the sphere of radius a; the far hemisphere,
  // outside the domain, held finite.
  const double lat0 = degToRad(centreLatDeg());
  const double cosC =
      std::max(std::sin(lat0) * std::sin(latRad) +
                   std::cos(lat0) * std::cos(latRad) * std::cos(dLonRad),
               1e-3);
  const double r = wgs84::semiMajorAxisM / cosC;
  return MapPoint{r * std::cos(latRad) * std::sin(dLonRad),
                  r * (std::cos(lat0) * std::sin(latRad) -
                       std::sin(lat0) * std::cos(latRad) * std::cos(dLonRad))};
}

std::optional<std::array<double, 2>>
GnomonicProjection::approximateInverse(MapPoint point) const
{
  // Snyder 20-14, 20-15 (exact).
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const double lat0 = degToRad(centreLatDeg());
  const double rho = std::hypot(point.xM, point.yM);
  if (rho == 0.0) {
    return std::array<double, 2>{lat0, 0.0};
  }
  const double c = std::atan(rho / wgs84::semiMajorAxisM);
  const double lat =
      std::asin(std::clamp(std::cos(c) * std::sin(lat0) +
                               point.yM * std::sin(c) * std::cos(lat0) / rho,
                           -1.0, 1.0));
  const double lon = std::atan2(point.xM * std::sin(c),
                                rho * std::cos(lat0) * std::cos(c) -
                                    point.yM * std::sin(lat0) * std::sin(c));
  return std::array<double, 2>{lat, lon};
}

QByteArray GnomonicProjection::glslForwardSource() const
{
  // p0: sin and cos of the centre's latitude.
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float s = sin(lonLatRad.y);\n"
         "  float c = cos(lonLatRad.y);\n"
         "  float cl = cos(lonLatRad.x);\n"
         "  float cosC = max(p0.x * s + p0.y * c * cl, 1e-3);\n"
         "  return " +
         glslNumber(wgs84::semiMajorAxisM) +
         " / cosC * vec2(c * sin(lonLatRad.x), p0.y * s - p0.x * c * cl);\n"
         "}\n";
}

double
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
GnomonicProjection::localExpansionLimitRadAt(double latRad,
                                             double dLonRad) const
{
  // 1 / cos c curves ever faster towards c = 90°: a tile uses the
  // expansion only if it is small beside its distance from there.
  const double fromInfinity =
      halfPi() -
      degToRad(angleFromCentreDeg(radToDeg(latRad),
                                  centralMeridianDeg() + radToDeg(dLonRad)));
  return std::min(localExpansionLimitRad(), 0.02 * std::max(fromInfinity, 0.0));
}

std::array<std::array<float, 4>, 2> GnomonicProjection::glslParameters() const
{
  const double lat0 = degToRad(centreLatDeg());
  return {std::array<float, 4>{static_cast<float>(std::sin(lat0)),
                               static_cast<float>(std::cos(lat0)), 0.0F, 0.0F},
          std::array<float, 4>{}};
}

// --- Polar Stereographic -----------------------------------------------------

PolarStereographicProjection::PolarStereographicProjection(
    // Hemisphere, latitude of true scale, then meridian.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool north, double trueScaleLatDeg, double centralMeridianDeg)
    : LocalExpansionProjection(centralMeridianDeg), m_north(north),
      m_trueScaleLatDeg(trueScaleLatDeg)
{
  // Snyder 21-33 and 21-34 (PROJ's stere, polar, with lat_ts): in the
  // map's own hemisphere, latitudes counted towards its pole.
  const double e = ellipsoidal::eccentricity();
  const double latTs = degToRad(north ? trueScaleLatDeg : -trueScaleLatDeg);
  if (std::abs(latTs - halfPi()) < 1e-10) {
    m_rhoPerT =
        2.0 * wgs84::semiMajorAxisM /
        std::sqrt(std::pow(1.0 + e, 1.0 + e) * std::pow(1.0 - e, 1.0 - e));
  } else {
    m_rhoPerT = wgs84::semiMajorAxisM * ellipsoidal::msfn(latTs) /
                ellipsoidal::tsfn(latTs);
  }
}

QString PolarStereographicProjection::id() const
{
  return m_north ? QStringLiteral("polar-stereographic-north")
                 : QStringLiteral("polar-stereographic-south");
}

QString PolarStereographicProjection::displayName() const
{
  return m_north ? QCoreApplication::translate("ts::Projection",
                                               "Polar Stereographic (North)")
                 : QCoreApplication::translate("ts::Projection",
                                               "Polar Stereographic (South)");
}

QString PolarStereographicProjection::projString() const
{
  return QStringLiteral("+proj=stere +lat_0=%1 +lat_ts=%2 +lon_0=%3 +x_0=0 "
                        "+y_0=0 +ellps=WGS84")
      .arg(m_north ? 90 : -90)
      .arg(m_trueScaleLatDeg, 0, 'g', 17)
      .arg(centralMeridianDeg(), 0, 'g', 17);
}

std::vector<ProjectionParameter>
PolarStereographicProjection::parameters() const
{
  // EPSG:3413 in the north, EPSG:3031 in the south.
  return {ProjectionParameter{QStringLiteral("trueScaleLatitude"),
                              QCoreApplication::translate(
                                  "ts::Projection", "Latitude of true scale"),
                              m_north ? 70.0 : -71.0, m_north ? 60.0 : -90.0,
                              m_north ? 90.0 : -60.0, QStringLiteral("°"), 4},
          ProjectionParameter{
              QStringLiteral("centralMeridian"),
              QCoreApplication::translate("ts::Projection", "Central meridian"),
              m_north ? -45.0 : 0.0, -180.0, 180.0, QStringLiteral("°"), 4}};
}

ProjectionParameterValues PolarStereographicProjection::parameterValues() const
{
  return {{QStringLiteral("trueScaleLatitude"), m_trueScaleLatDeg},
          {QStringLiteral("centralMeridian"), centralMeridianDeg()}};
}

QByteArray PolarStereographicProjection::glslFunction() const
{
  return m_north ? "tsProjectPolarStereographicNorth"
                 : "tsProjectPolarStereographicSouth";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint PolarStereographicProjection::project(double latRad,
                                               double dLonRad) const
{
  // The central meridian straight down from the north pole, straight up
  // from the south pole.
  const double towardsPole = m_north ? latRad : -latRad;
  const double rho = std::abs(towardsPole - halfPi()) < 1e-10
                         ? 0.0
                         : m_rhoPerT * ellipsoidal::tsfn(towardsPole);
  return MapPoint{rho * std::sin(dLonRad),
                  (m_north ? -rho : rho) * std::cos(dLonRad)};
}

std::optional<std::array<double, 2>>
PolarStereographicProjection::approximateInverse(MapPoint point) const
{
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const double rho = std::hypot(point.xM, point.yM);
  const double towardsPole =
      rho == 0.0 ? halfPi() : ellipsoidal::phiOfTs(rho / m_rhoPerT);
  const double lon =
      rho == 0.0 ? 0.0 : std::atan2(point.xM, m_north ? -point.yM : point.yM);
  return std::array<double, 2>{m_north ? towardsPole : -towardsPole, lon};
}

QByteArray PolarStereographicProjection::glslForwardSource() const
{
  // p0: ρ per t, and the sign of y's cos λ term (−1 north, +1 south).
  const QByteArray towardsPole = m_north ? "lonLatRad.y" : "-lonLatRad.y";
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float phi = clamp(" +
         towardsPole +
         ", -1.5707953, 1.5707953);\n"
         "  float s = sin(phi);\n"
         "  float c = cos(phi);\n"
         "  float rho = p0.x * " +
         ellipsoidal::glslTsfn() +
         ";\n"
         "  return rho * vec2(sin(lonLatRad.x), p0.y * cos(lonLatRad.x));\n"
         "}\n";
}

std::array<std::array<float, 4>, 2>
PolarStereographicProjection::glslParameters() const
{
  return {std::array<float, 4>{static_cast<float>(m_rhoPerT),
                               m_north ? -1.0F : 1.0F, 0.0F, 0.0F},
          std::array<float, 4>{}};
}

} // namespace ts
