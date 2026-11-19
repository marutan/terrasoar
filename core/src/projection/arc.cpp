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
#include "ts/projection/arc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <QCoreApplication>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** MIL-A-89007 (ADRG) appendix 70, table III, for each northern zone:
 *  its latitudes and the longitudinal pixel constant A at 1:1M (pixels
 *  in 360°). The latitudinal constant B is the same in every zone. */
struct ZoneDefinition {
  double southDeg;
  double northDeg;
  double a;
};

constexpr std::array<ZoneDefinition, 9> zoneTable = {{
    {0.0, 32.0, 369664.0},
    {32.0, 48.0, 302592.0},
    {48.0, 56.0, 245760.0},
    {56.0, 64.0, 199168.0},
    {64.0, 68.0, 163328.0},
    {68.0, 72.0, 137216.0},
    {72.0, 76.0, 110080.0},
    {76.0, 80.0, 82432.0},
    {80.0, 90.0, 400384.0},
}};

constexpr double tableB = 400384.0;
constexpr double referenceScale = 1.0e6;
constexpr double adrgBlock = 512.0;
constexpr double cadrgBlock = 256.0;
/** CADRG's pitch over ADRG's (MIL-C-89038 60.1). */
constexpr double pitchRatio = 1.5;

/** The table's row for @p zone (1–18). */
const ZoneDefinition& definition(int32_t zone)
{
  return zoneTable.at(static_cast<std::size_t>((zone - 1) % 9));
}

/** ADRG's pixel constant: @p constant at 1:1M scaled and rounded up to
 *  whole 512-pixel blocks. */
double adrgConstant(double constant, double reciprocalScale)
{
  return std::ceil(constant * referenceScale / reciprocalScale / adrgBlock) *
         adrgBlock;
}

} // namespace

namespace arc
{

int32_t zoneAt(double latDeg)
{
  const double absLat = std::min(std::abs(latDeg), 90.0);
  int32_t zone = 9;
  for (std::size_t i = 0; i < zoneTable.size(); ++i) {
    if (absLat <= zoneTable.at(i).northDeg) {
      zone = static_cast<int32_t>(i) + 1;
      break;
    }
  }
  return latDeg < 0.0 ? zone + 9 : zone;
}

bool isPolar(int32_t zone)
{
  return zone == 9 || zone == 18;
}

std::array<double, 2> zoneLatitudesDeg(int32_t zone)
{
  const ZoneDefinition& d = definition(std::clamp(zone, 1, zoneCount));
  if (zone > 9) {
    return {-d.northDeg, -d.southDeg};
  }
  return {d.southDeg, d.northDeg};
}

// A zone, then a scale: an integer and a real, not to be mixed up.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> pixelSizeDeg(int32_t zone, double reciprocalScale)
{
  // As GDAL's CADRG writer (rpfframewriter.cpp), MIL-C-89038 60.1.1 on.
  const ZoneDefinition& d = definition(std::clamp(zone, 1, zoneCount));
  const double lonConstant =
      std::round(adrgConstant(d.a, reciprocalScale) / pitchRatio / cadrgBlock) *
      cadrgBlock;
  const double latConstant = std::round(adrgConstant(tableB, reciprocalScale) /
                                        pitchRatio / 4.0 / cadrgBlock) *
                             cadrgBlock;
  return {360.0 / lonConstant, 90.0 / latConstant};
}

double polarPixelSizeM(double reciprocalScale)
{
  // MIL-C-89038 60.4.
  const double polarConstant =
      std::round(adrgConstant(tableB, reciprocalScale) * 20.0 / 360.0 /
                 pitchRatio / adrgBlock) *
      adrgBlock * 360.0 / 20.0;
  return wgs84::semiMajorAxisM * 2.0 * pi / polarConstant;
}

char zoneCode(int32_t zone)
{
  if (zone <= 9) {
    return static_cast<char>('0' + zone);
  }
  return zone == 18 ? 'J' : static_cast<char>('A' + (zone - 10));
}

} // namespace arc

namespace
{

/** The cylindrical zone a parameter asks for: 1–8 or 10–17 (zone 1's
 *  formulas when following the view; a polar zone's neighbour, which
 *  makeProjection() doesn't ask for). */
int32_t cylindricalZone(int32_t zone)
{
  const int32_t z = std::clamp(zone, 0, arc::zoneCount);
  if (z == 0) {
    return 1;
  }
  return arc::isPolar(z) ? z - 1 : z;
}

double xScaleOf(int32_t zone, double reciprocalScale)
{
  const std::array<double, 2> pixel =
      arc::pixelSizeDeg(cylindricalZone(zone), reciprocalScale);
  return pixel[1] / pixel[0];
}

QString arcScaleLabel()
{
  return QCoreApplication::translate("ts::Projection", "Product scale, 1 to");
}

} // namespace

// --- Zones outside the poles -------------------------------------------------

ArcZoneProjection::ArcZoneProjection(int32_t zone, double reciprocalScale)
    : NormalCylindricalProjection(
          xScaleOf(zone, reciprocalScale),
          arc::zoneLatitudesDeg(cylindricalZone(zone))[0],
          arc::zoneLatitudesDeg(cylindricalZone(zone))[1]),
      m_zone(zone == 0 ? 0 : cylindricalZone(zone)),
      m_reciprocalScale(reciprocalScale)
{
}

QString ArcZoneProjection::id() const
{
  return QStringLiteral("rpf-arc");
}

QString ArcZoneProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "ARC Zones (RPF)");
}

QString ArcZoneProjection::projString() const
{
  // Equirectangular with the pixels' ratio as the cosine of its standard
  // parallel, on the sphere of radius a.
  return QStringLiteral("+proj=eqc +lat_ts=%1 +lon_0=0 +R=6378137")
      .arg(radToDeg(std::acos(xScale())), 0, 'g', 17);
}

std::vector<ProjectionParameter> ArcZoneProjection::parameters() const
{
  return {
      ProjectionParameter{QStringLiteral("zone"),
                          QCoreApplication::translate(
                              "ts::Projection",
                              "Zone (0 follows the view; 9 and 18 are polar)"),
                          0.0, 0.0, 18.0, QString(), 0},
      ProjectionParameter{QStringLiteral("scale"), arcScaleLabel(), 1.0e6,
                          1.0e4, 2.0e7, QString(), 0}};
}

ProjectionParameterValues ArcZoneProjection::parameterValues() const
{
  return {{QStringLiteral("zone"), static_cast<double>(m_zone)},
          {QStringLiteral("scale"), m_reciprocalScale}};
}

QByteArray ArcZoneProjection::glslFunction() const
{
  return "tsProjectArcZone";
}

QByteArray ArcZoneProjection::glslSource() const
{
  // r0.z = k. Not held to the band: beyond it the fragments are
  // discarded (glslDomainSource()), not squashed onto its edges.
  return "\nvec2 " + glslFunction() +
         "(vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)\n{\n"
         "  return 6378137.0 * vec2(r0.z * dLonLatRad.x, dLonLatRad.y);\n"
         "}\n";
}

ProjectionShaderReference
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ArcZoneProjection::shaderReference(double latDeg, double lonDeg) const
{
  Q_UNUSED(lonDeg)
  const double lat =
      degToRad(std::clamp(latDeg, minimumLatDeg(), maximumLatDeg()));
  const std::array<float, 2> span = latitudeSpan(lat);
  ProjectionShaderReference reference;
  reference.r0 = {span[0], span[1], static_cast<float>(xScale()), 0.0F};
  return reference;
}

const Projection*
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ArcZoneProjection::effectiveAt(double latDeg, double lonDeg) const
{
  Q_UNUSED(lonDeg)
  if (m_zone != 0) {
    return this;
  }
  const int32_t zone = arc::zoneAt(latDeg);
  const ProjectionParameterValues scale = {
      {QStringLiteral("scale"), m_reciprocalScale}};
  if (arc::isPolar(zone)) {
    return projectionWith(zone == 9 ? QStringLiteral("rpf-arc-north")
                                    : QStringLiteral("rpf-arc-south"),
                          scale);
  }
  return projectionWith(QStringLiteral("rpf-arc"),
                        {{QStringLiteral("zone"), static_cast<double>(zone)},
                         {QStringLiteral("scale"), m_reciprocalScale}});
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double ArcZoneProjection::domainMarginDeg(double latDeg, double lonDeg) const
{
  Q_UNUSED(lonDeg)
  return std::min(latDeg - minimumLatDeg(), maximumLatDeg() - latDeg);
}

std::array<float, 8> ArcZoneProjection::glslDomainParameters() const
{
  return {static_cast<float>(degToRad(minimumLatDeg())),
          static_cast<float>(degToRad(maximumLatDeg())),
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.0F};
}

QByteArray ArcZoneProjection::glslDomainSource() const
{
  // d0: the band's latitudes, radians.
  return "\nbool " + glslFunction() +
         "Outside(vec2 lonLatRad, vec4 d0, vec4 d1)\n{\n"
         "  return lonLatRad.y < d0.x || lonLatRad.y > d0.y;\n"
         "}\n";
}

double ArcZoneProjection::unitNorthing(double latRad) const
{
  return latRad;
}

double ArcZoneProjection::unitLatitude(double unitY) const
{
  return unitY;
}

double ArcZoneProjection::unitNorthingDerivative(double latRad) const
{
  Q_UNUSED(latRad)
  return 1.0;
}

// --- Polar zones -------------------------------------------------------------

ArcPolarProjection::ArcPolarProjection(bool north, double reciprocalScale)
    : LocalExpansionProjection(0.0), m_north(north),
      m_reciprocalScale(reciprocalScale)
{
}

QString ArcPolarProjection::id() const
{
  return m_north ? QStringLiteral("rpf-arc-north")
                 : QStringLiteral("rpf-arc-south");
}

QString ArcPolarProjection::displayName() const
{
  return m_north ? QCoreApplication::translate("ts::Projection",
                                               "ARC North Polar Zone (RPF)")
                 : QCoreApplication::translate("ts::Projection",
                                               "ARC South Polar Zone (RPF)");
}

QString ArcPolarProjection::projString() const
{
  return QStringLiteral("+proj=aeqd +lat_0=%1 +lon_0=0 +x_0=0 +y_0=0 "
                        "+R=6378137")
      .arg(m_north ? 90 : -90);
}

std::vector<ProjectionParameter> ArcPolarProjection::parameters() const
{
  return {ProjectionParameter{QStringLiteral("scale"), arcScaleLabel(), 1.0e6,
                              1.0e4, 2.0e7, QString(), 0}};
}

ProjectionParameterValues ArcPolarProjection::parameterValues() const
{
  return {{QStringLiteral("scale"), m_reciprocalScale}};
}

QByteArray ArcPolarProjection::glslFunction() const
{
  return m_north ? "tsProjectArcNorthPole" : "tsProjectArcSouthPole";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint ArcPolarProjection::project(double latRad, double dLonRad) const
{
  // The distance from the pole along the meridian, on the sphere.
  const double rho =
      wgs84::semiMajorAxisM * (0.5 * pi - (m_north ? latRad : -latRad));
  return MapPoint{rho * std::sin(dLonRad),
                  (m_north ? -rho : rho) * std::cos(dLonRad)};
}

std::optional<std::array<double, 2>>
ArcPolarProjection::approximateInverse(MapPoint point) const
{
  // Exact.
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const double rho = std::hypot(point.xM, point.yM);
  const double fromPole = rho / wgs84::semiMajorAxisM;
  const double lon =
      rho == 0.0 ? 0.0 : std::atan2(point.xM, m_north ? -point.yM : point.yM);
  return std::array<double, 2>{
      m_north ? 0.5 * pi - fromPole : fromPole - 0.5 * pi, lon};
}

QByteArray ArcPolarProjection::glslForwardSource() const
{
  // p0.x: the sign of y's cos λ term (−1 north, +1 south).
  const QByteArray fromPole =
      m_north ? "1.57079633 - lonLatRad.y" : "1.57079633 + lonLatRad.y";
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float rho = 6378137.0 * (" +
         fromPole +
         ");\n"
         "  return rho * vec2(sin(lonLatRad.x), p0.x * cos(lonLatRad.x));\n"
         "}\n";
}

std::array<std::array<float, 4>, 2> ArcPolarProjection::glslParameters() const
{
  return {std::array<float, 4>{m_north ? -1.0F : 1.0F, 0.0F, 0.0F, 0.0F},
          std::array<float, 4>{}};
}

} // namespace ts
