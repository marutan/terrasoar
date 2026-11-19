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
#include "ts/projection/nationalgrid.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/datumshift.h"

namespace ts
{

namespace
{

/** Airy 1830, the ellipsoid of OSGB36 (the Ordnance Survey's values). */
constexpr double airySemiMajorM = 6377563.396;
constexpr double airySemiMinorM = 6356256.909;

/** The National Grid's true origin, scale factor and false origin. */
constexpr double originLatDeg = 49.0;
constexpr double originLonDeg = -2.0;
constexpr double gridScaleFactor = 0.9996012717;
constexpr double falseEastingM = 400000.0;
constexpr double falseNorthingM = -100000.0;

/** EPSG:1314, OSGB36 to WGS 84 (6), position vector: metres, arc-seconds
 *  and parts per million. */
constexpr double helmertTxM = 446.448;
constexpr double helmertTyM = -125.157;
constexpr double helmertTzM = 542.06;
constexpr double helmertRxSec = 0.15;
constexpr double helmertRySec = 0.247;
constexpr double helmertRzSec = 0.842;
constexpr double helmertScalePpm = -20.489;

/** Radians in an arc-second. */
constexpr double arcSecondRad = pi / (180.0 * 3600.0);

/** The lettered squares: 100 km squares east and north. */
constexpr int32_t squaresEast = 7;
constexpr int32_t squaresNorth = 13;
constexpr double squareM = 100000.0;

double airyEccentricitySquared()
{
  const double f = 1.0 - airySemiMinorM / airySemiMajorM;
  return f * (2.0 - f);
}

/** The abridged Molodensky formulas' constants, WGS 84 to OSGB36. */
struct MolodenskyConstants {
  /** WGS 84's semi-major axis and eccentricity squared. */
  double a = 0.0;
  double e2 = 0.0;
  /** The translation, metres (the Helmert's, reversed). */
  double dx = 0.0;
  double dy = 0.0;
  double dz = 0.0;
  /** a Δf + f Δa, metres. */
  double k = 0.0;
};

MolodenskyConstants molodenskyConstants()
{
  MolodenskyConstants c;
  c.a = wgs84::semiMajorAxisM;
  c.e2 = wgs84::eccentricitySquared;
  c.dx = -helmertTxM;
  c.dy = -helmertTyM;
  c.dz = -helmertTzM;
  const double f = 1.0 - wgs84::semiMinorAxisM / wgs84::semiMajorAxisM;
  const double airyF = 1.0 - airySemiMinorM / airySemiMajorM;
  c.k = c.a * (airyF - f) + f * (airySemiMajorM - c.a);
  return c;
}

/**
 * The abridged Molodensky shift from WGS 84 to OSGB36 at latitude
 * @p latRad, longitude @p lonRad: {Δlatitude, Δlongitude}, radians. The
 * same formulas as the shader's, in double.
 */
// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> molodenskyShift(double latRad, double lonRad)
{
  const MolodenskyConstants c = molodenskyConstants();
  const double sp = std::sin(latRad);
  const double cp = std::max(std::cos(latRad), 1e-7);
  const double sl = std::sin(lonRad);
  const double cl = std::cos(lonRad);
  const double w = 1.0 - c.e2 * sp * sp;
  const double n = c.a / std::sqrt(w);
  const double m = n * (1.0 - c.e2) / w;
  return {(-c.dx * sp * cl - c.dy * sp * sl + c.dz * cp + 2.0 * c.k * sp * cp) /
              m,
          (-c.dx * sl + c.dy * cl) / (n * cp)};
}

QByteArray glslNumber(double value)
{
  return QByteArray::number(value, 'g', 9);
}

/** The index of grid letter @p letter in the alphabet without I. */
int32_t letterIndex(QChar letter)
{
  const int32_t index = letter.toUpper().toLatin1() - 'A';
  return index > 7 ? index - 1 : index;
}

/** Smoothstep of @p t clamped to [0, 1]. */
double smooth(double t)
{
  const double c = std::clamp(t, 0.0, 1.0);
  return c * c * (3.0 - 2.0 * c);
}

} // namespace

// --- OSGB36 ------------------------------------------------------------------

Osgb36Datum::Osgb36Datum(std::shared_ptr<const HorizontalShiftGrid> ostn15)
    : m_grid(std::move(ostn15))
{
}

// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> Osgb36Datum::helmertFromWgs84(double latRad,
                                                    double lonRad)
{
  // As PROJ: geocentric on WGS 84 at zero height, the inverse of the
  // position vector transform (rotations as small angles), then geodetic
  // on Airy; the height is dropped.
  const double es = wgs84::eccentricitySquared;
  const double sine = std::sin(latRad);
  const double primeVertical =
      wgs84::semiMajorAxisM / std::sqrt(1.0 - es * sine * sine);
  const double x = primeVertical * std::cos(latRad) * std::cos(lonRad);
  const double y = primeVertical * std::cos(latRad) * std::sin(lonRad);
  const double z = primeVertical * (1.0 - es) * sine;
  const double rx = helmertRxSec * arcSecondRad;
  const double ry = helmertRySec * arcSecondRad;
  const double rz = helmertRzSec * arcSecondRad;
  const double scale = 1.0 + helmertScalePpm * 1e-6;
  // X' = T + (1 + s) R X with R = [1 −rz ry; rz 1 −rx; −ry rx 1], so
  // X = Rᵀ (X' − T) / (1 + s).
  const double dx = x - helmertTxM;
  const double dy = y - helmertTyM;
  const double dz = z - helmertTzM;
  const double ox = (dx + rz * dy - ry * dz) / scale;
  const double oy = (-rz * dx + dy + rx * dz) / scale;
  const double oz = (ry * dx - rx * dy + dz) / scale;
  // Geodetic latitude on Airy: φ = atan2(z + e² N sin φ, p), iterated.
  const double airyEs = airyEccentricitySquared();
  const double p = std::hypot(ox, oy);
  double lat = std::atan2(oz, p * (1.0 - airyEs));
  for (int32_t i = 0; i < 10; ++i) {
    const double s = std::sin(lat);
    const double n = airySemiMajorM / std::sqrt(1.0 - airyEs * s * s);
    const double next = std::atan2(oz + airyEs * n * s, p);
    const bool done = std::abs(next - lat) < 1e-15;
    lat = next;
    if (done) {
      break;
    }
  }
  return {lat, std::atan2(oy, ox)};
}

// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> Osgb36Datum::fromWgs84(double latRad, double lonRad) const
{
  const std::array<double, 2> helmert = helmertFromWgs84(latRad, lonRad);
  if (!m_grid) {
    return helmert;
  }
  const double inside = m_grid->insideDeg(radToDeg(latRad), radToDeg(lonRad));
  if (!(inside > 0.0)) {
    return helmert;
  }
  // The inverse of OSGB36 + offset(OSGB36) = ETRS89, as PROJ's inverse
  // hgridshift: the offset is that at the OSGB36 point, found by
  // iteration (it changes by under a millimetre for the 100 m step).
  std::array<double, 2> osgb = {latRad, lonRad};
  for (int32_t i = 0; i < 10; ++i) {
    const std::array<double, 2> offset =
        m_grid->clampedOffsetRad(osgb[0], osgb[1]);
    const std::array<double, 2> next = {latRad - offset[0], lonRad - offset[1]};
    const bool converged = std::abs(next[0] - osgb[0]) < 1e-15 &&
                           std::abs(next[1] - osgb[1]) < 1e-15;
    osgb = next;
    if (converged) {
      break;
    }
  }
  const double w = smooth(inside / blendDeg());
  return {w * osgb[0] + (1.0 - w) * helmert[0],
          w * osgb[1] + (1.0 - w) * helmert[1]};
}

// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> Osgb36Datum::toWgs84(double latRad, double lonRad) const
{
  // fromWgs84() is the identity plus a slowly varying shift: iterate.
  std::array<double, 2> wgs = {latRad, lonRad};
  for (int32_t i = 0; i < 10; ++i) {
    const std::array<double, 2> osgb = fromWgs84(wgs[0], wgs[1]);
    const double dLat = latRad - osgb[0];
    const double dLon = lonRad - osgb[1];
    wgs = {wgs[0] + dLat, wgs[1] + dLon};
    if (std::abs(dLat) < 1e-14 && std::abs(dLon) < 1e-14) {
      break;
    }
  }
  return wgs;
}

QString Osgb36Datum::gridPath() const
{
  return m_grid ? m_grid->path() : QString();
}

// --- National Grid -----------------------------------------------------------

NationalGridProjection::NationalGridProjection(
    std::shared_ptr<const HorizontalShiftGrid> ostn15)
    : TransverseMercatorProjection(originLonDeg, gridScaleFactor,
                                   airySemiMajorM, airySemiMinorM),
      m_datum(std::move(ostn15)),
      m_originNorthingM(
          TransverseMercatorProjection::project(degToRad(originLatDeg), 0.0).yM)
{
}

QString NationalGridProjection::id() const
{
  return QStringLiteral("uk-national-grid");
}

QString NationalGridProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection",
                                     "UK National Grid (OSGB36)");
}

QString NationalGridProjection::projString() const
{
  const QString tmerc =
      QStringLiteral("+proj=tmerc +lat_0=49 +lon_0=-2 +k_0=0.9996012717 "
                     "+x_0=400000 +y_0=-100000 +a=6377563.396 "
                     "+b=6356256.909");
  if (m_datum.hasGrid()) {
    return QStringLiteral("+proj=pipeline +step +inv +proj=hgridshift "
                          "+grids=%1 +step %2")
        .arg(m_datum.gridPath(), tmerc);
  }
  return QStringLiteral(
             "+proj=pipeline +step +proj=push +v_3 +step +proj=cart "
             "+ellps=WGS84 +step +inv +proj=helmert +x=446.448 +y=-125.157 "
             "+z=542.06 +rx=0.15 +ry=0.247 +rz=0.842 +s=-20.489 "
             "+convention=position_vector +step +inv +proj=cart "
             "+a=6377563.396 +b=6356256.909 +step +proj=pop +v_3 +step %1")
      .arg(tmerc);
}

std::vector<ProjectionParameter> NationalGridProjection::parameters() const
{
  return {};
}

ProjectionParameterValues NationalGridProjection::parameterValues() const
{
  return {};
}

QByteArray NationalGridProjection::glslFunction() const
{
  return "tsProjectNationalGrid";
}

QString NationalGridProjection::datumShiftGridId() const
{
  return QStringLiteral("ostn15");
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString NationalGridProjection::gridReference(double latDeg,
                                              double lonDeg) const
{
  if (!contains(latDeg, lonDeg)) {
    return QString();
  }
  const MapPoint grid = forward(latDeg, lonDeg);
  return formatGridReference(grid.xM, grid.yM);
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint NationalGridProjection::gridOf(double osgbLatRad,
                                        double osgbLonRad) const
{
  const MapPoint p = TransverseMercatorProjection::project(
      osgbLatRad, osgbLonRad - degToRad(originLonDeg));
  return MapPoint{p.xM + falseEastingM,
                  p.yM - m_originNorthingM + falseNorthingM};
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint NationalGridProjection::project(double latRad, double dLonRad) const
{
  const std::array<double, 2> osgb =
      m_datum.fromWgs84(latRad, degToRad(originLonDeg) + dLonRad);
  return gridOf(osgb[0], osgb[1]);
}

std::optional<std::array<double, 2>>
NationalGridProjection::approximateInverse(MapPoint point) const
{
  const std::optional<std::array<double, 2>> osgb =
      TransverseMercatorProjection::approximateInverse(
          MapPoint{point.xM - falseEastingM,
                   point.yM - falseNorthingM + m_originNorthingM});
  if (!osgb) {
    return std::nullopt;
  }
  const std::array<double, 2> wgs =
      m_datum.toWgs84((*osgb)[0], (*osgb)[1] + degToRad(originLonDeg));
  return std::array<double, 2>{wgs[0], wgs[1] - degToRad(originLonDeg)};
}

QByteArray NationalGridProjection::glslForwardPrologue() const
{
  // The datum shift in float by the abridged Molodensky formulas (the
  // Helmert's translation and the change of ellipsoid), plus p1.xy, the
  // difference between the exact shift and these at the tile's reference
  // (glslParametersAt()): exact there, and what is left out (the
  // rotations and scale, OSTN15's detail) changes little across a tile.
  const MolodenskyConstants c = molodenskyConstants();
  return "  {\n"
         "    float sp = sin(lonLatRad.y);\n"
         "    float cp = max(cos(lonLatRad.y), 1e-7);\n"
         "    float la = lonLatRad.x + " +
         glslNumber(degToRad(originLonDeg)) +
         ";\n"
         "    float sl = sin(la);\n"
         "    float cl = cos(la);\n"
         "    float w = 1.0 - " +
         glslNumber(c.e2) +
         " * sp * sp;\n"
         "    float n = " +
         glslNumber(c.a) +
         " / sqrt(w);\n"
         "    float m = n * " +
         glslNumber(1.0 - c.e2) +
         " / w;\n"
         "    lonLatRad += vec2((" +
         glslNumber(-c.dx) + " * sl + " + glslNumber(c.dy) +
         " * cl) / (n * cp),\n"
         "                      (" +
         glslNumber(-c.dx) + " * sp * cl - " + glslNumber(c.dy) +
         " * sp * sl + " + glslNumber(c.dz) + " * cp + " +
         glslNumber(2.0 * c.k) +
         " * sp * cp) / m)\n"
         "                 + p1.xy;\n"
         "  }\n";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<std::array<float, 4>, 2>
NationalGridProjection::glslParametersAt(double latRad, double dLonRad) const
{
  std::array<std::array<float, 4>, 2> parameters = glslParameters();
  const double lonRad = degToRad(originLonDeg) + dLonRad;
  const std::array<double, 2> exact = m_datum.fromWgs84(latRad, lonRad);
  const std::array<double, 2> approximate = molodenskyShift(latRad, lonRad);
  parameters[1] = {static_cast<float>(exact[1] - lonRad - approximate[1]),
                   static_cast<float>(exact[0] - latRad - approximate[0]), 0.0F,
                   0.0F};
  return parameters;
}

// --- Grid references ---------------------------------------------------------

// Easting then northing, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString formatGridReference(double eastingM, double northingM, int32_t digits)
{
  if (!(eastingM >= 0.0 && eastingM < squaresEast * squareM &&
        northingM >= 0.0 && northingM < squaresNorth * squareM) ||
      digits < 0 || digits > 10 || digits % 2 != 0) {
    return QString();
  }
  const int32_t east = static_cast<int32_t>(std::floor(eastingM / squareM));
  const int32_t north = static_cast<int32_t>(std::floor(northingM / squareM));
  // The 500 km square, then the 100 km square within it: 5 × 5 letters
  // each, A to Z without I, from the north-west; the false origin is in
  // SV.
  int32_t first = (19 - north) - (19 - north) % 5 + (east + 10) / 5;
  int32_t second = (19 - north) * 5 % 25 + east % 5;
  first += first > 7 ? 1 : 0;
  second += second > 7 ? 1 : 0;
  QString reference;
  reference += QLatin1Char(static_cast<char>('A' + first));
  reference += QLatin1Char(static_cast<char>('A' + second));
  if (digits == 0) {
    return reference;
  }
  const int32_t each = digits / 2;
  const double unit = std::pow(10.0, 5 - each);
  const int64_t e =
      static_cast<int64_t>(std::floor((eastingM - east * squareM) / unit));
  const int64_t n =
      static_cast<int64_t>(std::floor((northingM - north * squareM) / unit));
  return QStringLiteral("%1 %2 %3")
      .arg(reference)
      .arg(e, each, 10, QLatin1Char('0'))
      .arg(n, each, 10, QLatin1Char('0'));
}

std::optional<MapPoint> parseGridReference(const QString& text)
{
  static const QRegularExpression pattern(
      QStringLiteral("^\\s*([A-HJ-Z])([A-HJ-Z])\\s*(\\d*)\\s*(\\d*)\\s*$"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = pattern.match(text);
  if (!match.hasMatch()) {
    return std::nullopt;
  }
  const int32_t first = letterIndex(match.captured(1).at(0));
  const int32_t second = letterIndex(match.captured(2).at(0));
  const int32_t east = (first - 2) % 5 * 5 + second % 5;
  const int32_t north = 19 - first / 5 * 5 - second / 5;
  if (first < 2 || east < 0 || east >= squaresEast || north < 0 ||
      north >= squaresNorth) {
    return std::nullopt;
  }
  QString eastDigits = match.captured(3);
  QString northDigits = match.captured(4);
  if (northDigits.isEmpty()) {
    // "TQ3008080729": half each.
    if (eastDigits.size() % 2 != 0) {
      return std::nullopt;
    }
    northDigits = eastDigits.mid(eastDigits.size() / 2);
    eastDigits.truncate(eastDigits.size() / 2);
  }
  if (eastDigits.size() != northDigits.size() || eastDigits.size() > 5) {
    return std::nullopt;
  }
  const double unit =
      std::pow(10.0, static_cast<double>(5 - eastDigits.size()));
  const double e = eastDigits.isEmpty() ? 0.0 : eastDigits.toDouble();
  const double n = northDigits.isEmpty() ? 0.0 : northDigits.toDouble();
  return MapPoint{east * squareM + e * unit, north * squareM + n * unit};
}

} // namespace ts
