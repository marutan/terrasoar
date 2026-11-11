// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/projection/cylindricalprojection.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** Slack for "on the edge of the map", metres. */
constexpr double edgeToleranceM = 1e-6;

/** y / a of the family for latitude @p latRad. */
double unitNorthing(const CylindricalProjection::Parameters& p, double latRad)
{
  const double sine = std::sin(p.latitudeFactor * latRad);
  return p.scale * std::atanh(sine) -
         p.eccentricity * std::atanh(p.eccentricity * sine);
}

/** The latitude (radians) whose unitNorthing() is @p psi. */
double unitLatitude(const CylindricalProjection::Parameters& p, double psi)
{
  // sin(s φ) = tanh((ψ + e atanh(e sin(s φ))) / c); the right-hand side
  // changes with sin(s φ) by a factor of about e², so this converges in a
  // few steps. atan(sinh(u)) is asin(tanh(u)) without its loss of
  // precision near the poles.
  const double e = p.eccentricity;
  double theta = std::atan(std::sinh(psi / p.scale));
  for (int i = 0; i < 30 && e > 0.0; ++i) {
    const double next = std::atan(
        std::sinh((psi + e * std::atanh(e * std::sin(theta))) / p.scale));
    const bool converged = std::abs(next - theta) < 1e-16;
    theta = next;
    if (converged) {
      break;
    }
  }
  return theta / p.latitudeFactor;
}

/** The latitude at which the Mercator map of @p eccentricity is square. */
double squareMapLatDeg(double eccentricity)
{
  CylindricalProjection::Parameters p;
  p.eccentricity = eccentricity;
  return radToDeg(unitLatitude(p, pi));
}

double wgs84Eccentricity()
{
  return std::sqrt(wgs84::eccentricitySquared);
}

QString lonParameter(double centralMeridianDeg)
{
  return QStringLiteral(" +lon_0=%1").arg(centralMeridianDeg, 0, 'g', 17);
}

} // namespace

CylindricalProjection::CylindricalProjection(const Parameters& parameters,
                                             double centralMeridianDeg)
    : Projection(centralMeridianDeg), m_parameters(parameters)
{
}

double CylindricalProjection::worldWidthM()
{
  return 2.0 * pi * wgs84::semiMajorAxisM;
}

double CylindricalProjection::minimumLatDeg() const
{
  return -m_parameters.maximumLatDeg;
}

double CylindricalProjection::maximumLatDeg() const
{
  return m_parameters.maximumLatDeg;
}

double CylindricalProjection::northingM(double latDeg) const
{
  const double clamped = std::clamp(latDeg, minimumLatDeg(), maximumLatDeg());
  return wgs84::semiMajorAxisM * unitNorthing(m_parameters, degToRad(clamped));
}

double CylindricalProjection::latitudeDeg(double yM) const
{
  return radToDeg(unitLatitude(m_parameters, yM / wgs84::semiMajorAxisM));
}

MapPoint CylindricalProjection::forward(double latDeg, double lonDeg) const
{
  const double dLonDeg = normaliseLonDeg(lonDeg - centralMeridianDeg());
  return MapPoint{wgs84::semiMajorAxisM * degToRad(dLonDeg), northingM(latDeg)};
}

std::optional<Geodetic>
CylindricalProjection::inverse(const MapPoint& point) const
{
  const MapRect limits = bounds();
  if (!(point.xM >= limits.westM - edgeToleranceM &&
        point.xM <= limits.eastM + edgeToleranceM &&
        point.yM >= limits.southM - edgeToleranceM &&
        point.yM <= limits.northM + edgeToleranceM)) {
    return std::nullopt;
  }
  const double latDeg =
      std::clamp(latitudeDeg(point.yM), minimumLatDeg(), maximumLatDeg());
  const double lonDeg = normaliseLonDeg(
      centralMeridianDeg() + radToDeg(point.xM / wgs84::semiMajorAxisM));
  return Geodetic{latDeg, lonDeg, 0.0};
}

MapRect CylindricalProjection::bounds() const
{
  const double halfWidthM = 0.5 * worldWidthM();
  const double northM = northingM(maximumLatDeg());
  return MapRect{-halfWidthM, -northM, halfWidthM, northM};
}

MapScale CylindricalProjection::scaleAt(double latDeg) const
{
  // The ground is the WGS 84 ellipsoid whatever the formulas assume: a
  // degree of longitude is N cos φ long and a degree of latitude M.
  const double lat = degToRad(latDeg);
  const double sine = std::sin(lat);
  const double w2 = 1.0 - wgs84::eccentricitySquared * sine * sine;
  const double primeVertical = wgs84::semiMajorAxisM / std::sqrt(w2);
  const double meridional = wgs84::semiMajorAxisM *
                            (1.0 - wgs84::eccentricitySquared) /
                            (w2 * std::sqrt(w2));
  const double s = m_parameters.latitudeFactor;
  const double e = m_parameters.eccentricity;
  const double sinS = std::sin(s * lat);
  const double cosS = std::cos(s * lat);
  // dy/dφ of the family.
  const double dyDLat =
      wgs84::semiMajorAxisM * s *
      (m_parameters.scale / cosS - e * e * cosS / (1.0 - e * e * sinS * sinS));
  return MapScale{wgs84::semiMajorAxisM / (primeVertical * std::cos(lat)),
                  dyDLat / meridional};
}

ProjectionShaderReference
CylindricalProjection::shaderReference(double latDeg, double /*lonDeg*/) const
{
  const double limit = maximumLatDeg();
  const double lat = degToRad(std::clamp(latDeg, -limit, limit));
  const double s = m_parameters.latitudeFactor;
  const double e = m_parameters.eccentricity;
  const double sine = std::sin(s * lat);
  ProjectionShaderReference reference;
  reference.r0 = {static_cast<float>(sine),
                  static_cast<float>(std::cos(s * lat)),
                  static_cast<float>(degToRad(-limit) - lat),
                  static_cast<float>(degToRad(limit) - lat)};
  reference.r1 = {static_cast<float>(1.0 - e * e * sine * sine),
                  static_cast<float>(e), static_cast<float>(s),
                  static_cast<float>(m_parameters.scale)};
  return reference;
}

QByteArray CylindricalProjection::glslSource() const
{
  // 6378137 is the WGS 84 semi-major axis.
  return "\nvec2 " + glslFunction() +
         "(vec2 dLonLatRad, vec4 r0, vec4 r1)\n{\n"
         "  return 6378137.0\n"
         "       * vec2(dLonLatRad.x, tsCylindricalDy(dLonLatRad.y, r0, r1));\n"
         "}\n";
}

// --- Mercator ----------------------------------------------------------------

MercatorProjection::MercatorProjection(double centralMeridianDeg)
    : CylindricalProjection(Parameters{1.0, 1.0, wgs84Eccentricity(),
                                       squareMapLatDeg(wgs84Eccentricity())},
                            centralMeridianDeg)
{
}

QString MercatorProjection::id() const
{
  return QStringLiteral("mercator");
}

QString MercatorProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Mercator");
}

QString MercatorProjection::projString() const
{
  return QStringLiteral("+proj=merc +ellps=WGS84") +
         lonParameter(centralMeridianDeg());
}

QByteArray MercatorProjection::glslFunction() const
{
  return QByteArrayLiteral("tsProjectMercator");
}

// --- Web Mercator ------------------------------------------------------------

WebMercatorProjection::WebMercatorProjection(double centralMeridianDeg)
    : CylindricalProjection(Parameters{1.0, 1.0, 0.0, squareMapLatDeg(0.0)},
                            centralMeridianDeg)
{
}

QString WebMercatorProjection::id() const
{
  return QStringLiteral("web-mercator");
}

QString WebMercatorProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Web Mercator");
}

QString WebMercatorProjection::projString() const
{
  return QStringLiteral("+proj=webmerc +ellps=WGS84") +
         lonParameter(centralMeridianDeg());
}

QByteArray WebMercatorProjection::glslFunction() const
{
  return QByteArrayLiteral("tsProjectWebMercator");
}

// --- Miller ------------------------------------------------------------------

MillerProjection::MillerProjection(double centralMeridianDeg)
    : CylindricalProjection(Parameters{0.8, 1.25, 0.0, 90.0},
                            centralMeridianDeg)
{
}

QString MillerProjection::id() const
{
  return QStringLiteral("miller");
}

QString MillerProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Miller Cylindrical");
}

QString MillerProjection::projString() const
{
  return QStringLiteral("+proj=mill +ellps=WGS84") +
         lonParameter(centralMeridianDeg());
}

QByteArray MillerProjection::glslFunction() const
{
  return QByteArrayLiteral("tsProjectMiller");
}

} // namespace ts
