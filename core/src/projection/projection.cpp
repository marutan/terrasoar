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

#include "ts/projection/projection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <tuple>

#include "ts/projection/azimuthal.h"
#include "ts/projection/conic.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/projection/datumshift.h"
#include "ts/projection/nationalgrid.h"
#include "ts/projection/normalcylindrical.h"
#include "ts/projection/rotatedmercator.h"
#include "ts/projection/transversemercator.h"
#include "ts/projection/worldmaps.h"

namespace ts
{

namespace
{

// atanh() of GLSL is commonly log((1 + x) / (1 − x)) / 2, which loses the
// small arguments that matter here (a metre is 1.6e-7 of the radius), so
// small arguments use the series instead.
//
// tsCylindricalDy: y(φt + d) − y(φt) on the unit sphere for the family
// of CylindricalProjection, y = c atanh(sin(s φ)) − e atanh(e sin(s φ)),
// with r0 = (sin(s φt), cos(s φt), least d, greatest d) and
// r1 = (1 − e² sin²(s φt), e, s, c). Uses
//   atanh(u) − atanh(v) = atanh((u − v) / (1 − u v)) and
//   sin(p + q) − sin(p) = 2 cos(p + q/2) sin(q/2),
// neither of which subtracts nearly equal numbers.
const char* const commonGlsl = R"(
float tsAtanh(float x)
{
  float x2 = x * x;
  if (x2 < 0.04) {
    return x * (1.0 + x2 * (1.0 / 3.0 + x2 * (1.0 / 5.0 + x2 * (1.0 / 7.0
           + x2 * (1.0 / 9.0 + x2 * (1.0 / 11.0))))));
  }
  return 0.5 * log((1.0 + x) / (1.0 - x));
}

// atan() and asinh() of GLSL may be far less precise than a float (Mesa's
// atan is out by about 1e-5), so projections that need them to a few
// units in the last place use these: the argument is halved twice by
// exact identities, then a short series finishes.
//   tan(u/2) = tan u / (1 + sqrt(1 + tan² u))
//   sinh(u/2) = sinh u / sqrt(2 (1 + sqrt(1 + sinh² u)))
float tsAtanReduced(float z)
{
  z = z / (1.0 + sqrt(1.0 + z * z));
  z = z / (1.0 + sqrt(1.0 + z * z));
  float z2 = z * z;
  return 4.0 * z * (1.0 - z2 * (1.0 / 3.0 - z2 * (1.0 / 5.0 - z2 * (1.0 / 7.0
         - z2 * (1.0 / 9.0 - z2 * (1.0 / 11.0 - z2 * (1.0 / 13.0)))))));
}

float tsAtan2(float y, float x)
{
  float ay = abs(y);
  float ax = abs(x);
  float r = ay <= ax ? tsAtanReduced(ay / max(ax, 1e-37))
                     : 1.5707963267948966 - tsAtanReduced(ax / ay);
  if (x < 0.0) {
    r = 3.141592653589793 - r;
  }
  return y < 0.0 ? -r : r;
}

float tsAsinh(float x)
{
  x = x / sqrt(2.0 * (1.0 + sqrt(1.0 + x * x)));
  x = x / sqrt(2.0 * (1.0 + sqrt(1.0 + x * x)));
  float x2 = x * x;
  return 4.0 * x * (1.0 - x2 * (1.0 / 6.0 - x2 * (3.0 / 40.0 - x2 * (5.0 / 112.0
         - x2 * (35.0 / 1152.0 - x2 * (63.0 / 2816.0))))));
}

float tsCylindricalDy(float dLatRad, vec4 r0, vec4 r1)
{
  float q = clamp(dLatRad, r0.z, r0.w) * r1.z;
  float sinHalf = sin(0.5 * q);
  float cosHalf = cos(0.5 * q);
  float dSin = 2.0 * (r0.y * cosHalf - r0.x * sinHalf) * sinHalf;
  float y = r1.w * tsAtanh(dSin / (r0.y * r0.y - r0.x * dSin));
  if (r1.y > 0.0) {
    y -= r1.y * tsAtanh(r1.y * dSin / (r1.x - r1.y * r1.y * r0.x * dSin));
  }
  return y;
}
)";

} // namespace

Projection::Projection(double centralMeridianDeg)
    : m_centralMeridianDeg(normaliseLonDeg(centralMeridianDeg))
{
}

Projection::~Projection() = default;

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double Projection::domainMarginDeg(double latDeg, double lonDeg) const
{
  Q_UNUSED(latDeg)
  Q_UNUSED(lonDeg)
  return std::numeric_limits<double>::infinity();
}

bool Projection::contains(double latDeg, double lonDeg) const
{
  return containsLatitude(latDeg) &&
         std::abs(normaliseLonDeg(lonDeg - centralMeridianDeg())) <=
             maximumLonOffsetDeg() &&
         (!hasOwnDomain() || domainMarginDeg(latDeg, lonDeg) >= 0.0);
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Geodetic Projection::clampToDomain(double latDeg, double lonDeg) const
{
  return Geodetic{std::clamp(latDeg, minimumLatDeg(), maximumLatDeg()),
                  normaliseLonDeg(clampLonDeg(lonDeg)), 0.0};
}

double Projection::clampLonDeg(double lonDeg) const
{
  const double limit = maximumLonOffsetDeg();
  const double offset =
      std::clamp(normaliseLonDeg(lonDeg - centralMeridianDeg()), -limit, limit);
  return centralMeridianDeg() + offset;
}

std::optional<Geodetic> Projection::nearestInDomain(MapPoint point) const
{
  return inverse(point);
}

MapScale Projection::scaleAtPoint(double latDeg, double /*lonDeg*/) const
{
  return scaleAt(latDeg);
}

ProjectionShaderReference
Projection::shaderReferenceFor(double latDeg, double lonDeg,
                               double /*halfSpanRad*/) const
{
  return shaderReference(latDeg, lonDeg);
}

const std::vector<const Projection*>& projections()
{
  // To add a projection: write its class and list it here (see
  // docs/design/projections.md).
  static const MercatorProjection mercator;
  static const WebMercatorProjection webMercator;
  static const MillerProjection miller;
  static const EquidistantCylindricalProjection equidistantCylindrical;
  static const GallStereographicProjection gallStereographic;
  static const GallPetersProjection gallPeters;
  static const TransverseMercatorProjection transverseMercator;
  static const UtmProjection utm;
  static const CassiniProjection cassini;
  static const NationalGridProjection nationalGrid;
  static const RotatedMercatorProjection rotatedMercator;
  static const LambertConformalConicProjection lambertConformalConic;
  static const PolyconicProjection polyconic;
  static const AzimuthalEquidistantProjection azimuthalEquidistant;
  static const GnomonicProjection gnomonic;
  static const PolarStereographicProjection polarNorth(true, 70.0, -45.0);
  static const PolarStereographicProjection polarSouth(false, -71.0, 0.0);
  static const RobinsonProjection robinson;
  static const MollweideProjection mollweide;
  static const WinkelTripelProjection winkelTripel;
  static const std::vector<const Projection*> all = {&mercator,
                                                     &webMercator,
                                                     &miller,
                                                     &equidistantCylindrical,
                                                     &gallStereographic,
                                                     &gallPeters,
                                                     &transverseMercator,
                                                     &utm,
                                                     &cassini,
                                                     &nationalGrid,
                                                     &rotatedMercator,
                                                     &lambertConformalConic,
                                                     &polyconic,
                                                     &azimuthalEquidistant,
                                                     &gnomonic,
                                                     &polarNorth,
                                                     &polarSouth,
                                                     &robinson,
                                                     &mollweide,
                                                     &winkelTripel};
  return all;
}

namespace
{

/** Makes an instance of a projection with parameters, by id. */
std::unique_ptr<Projection>
makeProjection(const QString& id, const ProjectionParameterValues& values,
               const std::shared_ptr<const HorizontalShiftGrid>& grid)
{
  if (id == QLatin1String("uk-national-grid")) {
    return std::make_unique<NationalGridProjection>(grid);
  }
  if (id == QLatin1String("equidistant-cylindrical")) {
    return std::make_unique<EquidistantCylindricalProjection>(
        values.at(QStringLiteral("standardParallel")));
  }
  if (id == QLatin1String("transverse-mercator")) {
    return std::make_unique<TransverseMercatorProjection>(
        values.at(QStringLiteral("centralMeridian")),
        values.at(QStringLiteral("scaleFactor")));
  }
  if (id == QLatin1String("utm")) {
    return std::make_unique<UtmProjection>(
        static_cast<int32_t>(std::lround(values.at(QStringLiteral("zone")))));
  }
  if (id == QLatin1String("lambert-conformal-conic")) {
    return std::make_unique<LambertConformalConicProjection>(
        values.at(QStringLiteral("standardParallel1")),
        values.at(QStringLiteral("standardParallel2")),
        values.at(QStringLiteral("originLatitude")),
        values.at(QStringLiteral("centralMeridian")));
  }
  if (id == QLatin1String("azimuthal-equidistant")) {
    return std::make_unique<AzimuthalEquidistantProjection>(
        values.at(QStringLiteral("centreLatitude")),
        values.at(QStringLiteral("centreLongitude")));
  }
  if (id == QLatin1String("gnomonic")) {
    return std::make_unique<GnomonicProjection>(
        values.at(QStringLiteral("centreLatitude")),
        values.at(QStringLiteral("centreLongitude")));
  }
  if (id == QLatin1String("polar-stereographic-north") ||
      id == QLatin1String("polar-stereographic-south")) {
    return std::make_unique<PolarStereographicProjection>(
        id == QLatin1String("polar-stereographic-north"),
        values.at(QStringLiteral("trueScaleLatitude")),
        values.at(QStringLiteral("centralMeridian")));
  }
  if (id == QLatin1String("polyconic")) {
    return std::make_unique<PolyconicProjection>(
        values.at(QStringLiteral("centralMeridian")),
        values.at(QStringLiteral("originLatitude")));
  }
  if (id == QLatin1String("rotated-mercator")) {
    return std::make_unique<RotatedMercatorProjection>(
        values.at(QStringLiteral("centreLatitude")),
        values.at(QStringLiteral("centreLongitude")),
        values.at(QStringLiteral("azimuth")),
        values.at(QStringLiteral("scaleFactor")));
  }
  if (id == QLatin1String("cassini")) {
    return std::make_unique<CassiniProjection>(
        values.at(QStringLiteral("centralMeridian")));
  }
  return nullptr;
}

} // namespace

const Projection* projectionWith(const QString& id,
                                 const ProjectionParameterValues& values)
{
  const Projection* standard = projectionById(id);
  if (standard == nullptr) {
    return nullptr;
  }
  // Every parameter, clamped to its range; the default where missing.
  ProjectionParameterValues complete;
  bool allDefault = true;
  for (const ProjectionParameter& parameter : standard->parameters()) {
    const ProjectionParameterValues::const_iterator given =
        values.find(parameter.key);
    const double value =
        given == values.end() || !std::isfinite(given->second)
            ? parameter.defaultValue
            : std::clamp(given->second, parameter.minimum, parameter.maximum);
    complete[parameter.key] = value;
    allDefault = allDefault && value == parameter.defaultValue;
  }
  const QString gridId = standard->datumShiftGridId();
  const std::shared_ptr<const HorizontalShiftGrid> grid =
      gridId.isEmpty() ? nullptr : datumShiftGrid(gridId);
  if (allDefault && !grid) {
    return standard;
  }
  // Instances live for the whole run: the camera and the renderers keep
  // plain pointers to them. Each holds its grid, so a grid's address
  // isn't reused while it is a key.
  static std::mutex mutex;
  static std::map<std::tuple<QString, ProjectionParameterValues, const void*>,
                  std::unique_ptr<Projection>>
      made;
  const std::lock_guard<std::mutex> lock(mutex);
  std::unique_ptr<Projection>& instance =
      made[{id, complete, static_cast<const void*>(grid.get())}];
  if (!instance) {
    instance = makeProjection(id, complete, grid);
  }
  return instance ? instance.get() : standard;
}

const Projection* projectionById(const QString& id)
{
  for (const Projection* projection : projections()) {
    if (projection->id() == id) {
      return projection;
    }
  }
  return nullptr;
}

int32_t projectionIndex(const Projection* projection)
{
  if (projection == nullptr) {
    return -1;
  }
  // By id: an instance with parameters (projectionWith()) shares its
  // projection's GLSL; the parameters travel in the tile's uniforms.
  const std::vector<const Projection*>& all = projections();
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i] == projection || all[i]->id() == projection->id()) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

QByteArray projectionDomainGlsl()
{
  QByteArray glsl;
  QByteArray dispatch =
      "\nbool tsOutsideDomain(int index, vec2 lonLatDeg, vec4 d0, vec4 d1)\n"
      "{\n";
  const std::vector<const Projection*>& all = projections();
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (!all[i]->hasOwnDomain()) {
      continue;
    }
    glsl += all[i]->glslDomainSource();
    dispatch += "  if (index == " + QByteArray::number(static_cast<qint64>(i)) +
                ") {\n    return " + all[i]->glslFunction() +
                "Outside(radians(lonLatDeg), d0, d1);\n  }\n";
  }
  return glsl + dispatch + "  return false;\n}\n";
}

QByteArray projectionGlsl()
{
  QByteArray glsl = commonGlsl;
  const std::vector<const Projection*>& all = projections();
  for (const Projection* projection : all) {
    glsl += projection->glslSource();
  }
  glsl += "\nvec2 tsProject(int index, vec2 dLonLatRad, vec4 r0, vec4 r1, "
          "vec4 r2, vec4 r3)\n{\n";
  for (std::size_t i = 1; i < all.size(); ++i) {
    glsl += "  if (index == " + QByteArray::number(static_cast<int32_t>(i)) +
            ") {\n    return " + all[i]->glslFunction() +
            "(dLonLatRad, r0, r1, r2, r3);\n  }\n";
  }
  glsl += "  return " + all.front()->glslFunction() +
          "(dLonLatRad, r0, r1, r2, r3);\n}\n";
  return glsl;
}

} // namespace ts
