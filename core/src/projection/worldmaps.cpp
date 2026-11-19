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
#include "ts/projection/worldmaps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <QCoreApplication>

#include "ellipsoidal.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

using ellipsoidal::glslNumber;

namespace
{

double halfPi()
{
  return 0.5 * pi;
}

double radius()
{
  return wgs84::semiMajorAxisM;
}

// --- Robinson's table --------------------------------------------------------

/** One interval's cubic, in degrees from its start (PROJ's COEFS). */
using Cubic = std::array<float, 4>;

/** PROJ's coefficients (robin.cpp), floats as there, for each 5° from the
 *  equator: the parallel's length (X) and its distance from the equator
 *  (Y), as fractions. */
constexpr std::array<Cubic, 19> robinsonX = {{
    {1.0F, 2.2199e-17F, -7.15515e-05F, 3.1103e-06F},
    {0.9986F, -0.000482243F, -2.4897e-05F, -1.3309e-06F},
    {0.9954F, -0.00083103F, -4.48605e-05F, -9.86701e-07F},
    {0.99F, -0.00135364F, -5.9661e-05F, 3.6777e-06F},
    {0.9822F, -0.00167442F, -4.49547e-06F, -5.72411e-06F},
    {0.973F, -0.00214868F, -9.03571e-05F, 1.8736e-08F},
    {0.96F, -0.00305085F, -9.00761e-05F, 1.64917e-06F},
    {0.9427F, -0.00382792F, -6.53386e-05F, -2.6154e-06F},
    {0.9216F, -0.00467746F, -0.00010457F, 4.81243e-06F},
    {0.8962F, -0.00536223F, -3.23831e-05F, -5.43432e-06F},
    {0.8679F, -0.00609363F, -0.000113898F, 3.32484e-06F},
    {0.835F, -0.00698325F, -6.40253e-05F, 9.34959e-07F},
    {0.7986F, -0.00755338F, -5.00009e-05F, 9.35324e-07F},
    {0.7597F, -0.00798324F, -3.5971e-05F, -2.27626e-06F},
    {0.7186F, -0.00851367F, -7.01149e-05F, -8.6303e-06F},
    {0.6732F, -0.00986209F, -0.000199569F, 1.91974e-05F},
    {0.6213F, -0.010418F, 8.83923e-05F, 6.24051e-06F},
    {0.5722F, -0.00906601F, 0.000182F, 6.24051e-06F},
    {0.5322F, -0.00677797F, 0.000275608F, 6.24051e-06F},
}};

constexpr std::array<Cubic, 19> robinsonY = {{
    {-5.20417e-18F, 0.0124F, 1.21431e-18F, -8.45284e-11F},
    {0.062F, 0.0124F, -1.26793e-09F, 4.22642e-10F},
    {0.124F, 0.0124F, 5.07171e-09F, -1.60604e-09F},
    {0.186F, 0.0123999F, -1.90189e-08F, 6.00152e-09F},
    {0.248F, 0.0124002F, 7.10039e-08F, -2.24e-08F},
    {0.31F, 0.0123992F, -2.64997e-07F, 8.35986e-08F},
    {0.372F, 0.0124029F, 9.88983e-07F, -3.11994e-07F},
    {0.434F, 0.0123893F, -3.69093e-06F, -4.35621e-07F},
    {0.4958F, 0.0123198F, -1.02252e-05F, -3.45523e-07F},
    {0.5571F, 0.0121916F, -1.54081e-05F, -5.82288e-07F},
    {0.6176F, 0.0119938F, -2.41424e-05F, -5.25327e-07F},
    {0.6769F, 0.011713F, -3.20223e-05F, -5.16405e-07F},
    {0.7346F, 0.0113541F, -3.97684e-05F, -6.09052e-07F},
    {0.7903F, 0.0109107F, -4.89042e-05F, -1.04739e-06F},
    {0.8435F, 0.0103431F, -6.4615e-05F, -1.40374e-09F},
    {0.8936F, 0.00969686F, -6.4636e-05F, -8.547e-06F},
    {0.9394F, 0.00840947F, -0.000192841F, -4.2106e-06F},
    {0.9761F, 0.00616527F, -0.000256F, -4.2106e-06F},
    {1.0F, 0.00328947F, -0.000319159F, -4.2106e-06F},
}};

/** PROJ's FXC and FYC, and its intervals per radian (C1). */
constexpr double robinsonFx = 0.8487;
constexpr double robinsonFy = 1.3523;
constexpr double intervalsPerRad = 11.45915590261646417544;
constexpr std::size_t robinsonNodes = 18;

/** One interval's cubic in double. */
using SmoothCubic = std::array<double, 4>;

/**
 * PROJ's table made continuous: its float coefficients step by up to
 * 2.5 m at the 5° nodes (an interval's end isn't quite the next one's
 * start), which breaks the derivatives the renderer and the inverse take
 * there. Each interval's linear term takes up its step, so it ends at the
 * next one's start: within 2.5 m of PROJ, at the nodes.
 */
std::array<SmoothCubic, 19> smoothTable(const std::array<Cubic, 19>& table)
{
  std::array<SmoothCubic, 19> smooth{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    const Cubic& c = table.at(i);
    smooth.at(i) = {static_cast<double>(c[0]), static_cast<double>(c[1]),
                    static_cast<double>(c[2]), static_cast<double>(c[3])};
    if (i + 1 < table.size()) {
      const double end =
          smooth.at(i)[0] +
          5.0 * (smooth.at(i)[1] +
                 5.0 * (smooth.at(i)[2] + 5.0 * smooth.at(i)[3]));
      smooth.at(i)[1] += (static_cast<double>(table.at(i + 1)[0]) - end) / 5.0;
    }
  }
  return smooth;
}

const std::array<SmoothCubic, 19>& smoothX()
{
  static const std::array<SmoothCubic, 19> table = smoothTable(robinsonX);
  return table;
}

const std::array<SmoothCubic, 19>& smoothY()
{
  static const std::array<SmoothCubic, 19> table = smoothTable(robinsonY);
  return table;
}

double cubic(const SmoothCubic& c, double z)
{
  return c[0] + z * (c[1] + z * (c[2] + z * c[3]));
}

double cubicSlope(const SmoothCubic& c, double z)
{
  return c[1] + 2.0 * z * c[2] + 3.0 * z * z * c[3];
}

/** A GLSL array of one coefficient table. */
QByteArray glslTable(const char* name, const std::array<SmoothCubic, 19>& table)
{
  QByteArray glsl = QByteArray("const float ") + name + "[76] = float[76](";
  for (std::size_t i = 0; i < table.size(); ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      glsl += glslNumber(table.at(i).at(j));
      if (i + 1 < table.size() || j + 1 < 4) {
        glsl += ", ";
      }
    }
    glsl += i + 1 < table.size() ? "\n    " : "";
  }
  return glsl + ");\n";
}

// --- Mollweide ---------------------------------------------------------------

/** PROJ's C_x, C_y and C_p for p = π/2. */
double mollweideCx()
{
  return 2.0 * std::sqrt(2.0) / pi;
}

double mollweideCy()
{
  return std::sqrt(2.0);
}

/**
 * A start for Newton's method on t = 2θ, t + sin t = π sin φ: φ, but near
 * the poles, where Newton's method is slow (the derivative vanishes),
 * t ≈ π − ∛(6 (π − |k|)) from t + sin t ≈ π − (π − t)³ / 6.
 */
double startingAngle(double latRad)
{
  const double k = pi * std::sin(latRad);
  if (std::abs(std::sin(latRad)) < 0.9) {
    return latRad;
  }
  const double u = std::cbrt(6.0 * std::max(pi - std::abs(k), 0.0));
  return std::copysign(pi - u, latRad);
}

/** The auxiliary angle θ of latitude @p latRad: 2θ + sin 2θ = π sin φ. */
double mollweideTheta(double latRad)
{
  // Newton on t = 2θ, as PROJ, to full precision; it converges slowly
  // only within a hair of the poles, where θ = φ = ±90°.
  const double k = pi * std::sin(latRad);
  double t = startingAngle(latRad);
  for (int32_t i = 0; i < 60; ++i) {
    const double v = (t + std::sin(t) - k) / (1.0 + std::cos(t));
    if (!std::isfinite(v)) {
      break;
    }
    t -= v;
    if (std::abs(v) < 1e-15) {
      break;
    }
  }
  return std::clamp(0.5 * t, -halfPi(), halfPi());
}

// --- Winkel Tripel -----------------------------------------------------------

/** cos φ1 of Winkel's standard parallel, 2 / π (PROJ's default). */
constexpr double winkelCosPhi1 = 0.636619772367581343;

} // namespace

// --- Robinson ----------------------------------------------------------------

QString RobinsonProjection::id() const
{
  return QStringLiteral("robinson");
}

QString RobinsonProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Robinson");
}

QString RobinsonProjection::projString() const
{
  return QStringLiteral("+proj=robin +lon_0=0 +x_0=0 +y_0=0 +R=6378137");
}

QByteArray RobinsonProjection::glslFunction() const
{
  return "tsProjectRobinson";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint RobinsonProjection::project(double latRad, double dLonRad) const
{
  const double absLat = std::min(std::abs(latRad), halfPi());
  const std::size_t i = std::min(
      static_cast<std::size_t>(std::floor(absLat * intervalsPerRad + 1e-15)),
      robinsonNodes);
  const double z = radToDeg(absLat - static_cast<double>(i) / intervalsPerRad);
  const double y = cubic(smoothY().at(i), z) * robinsonFy * radius();
  return MapPoint{cubic(smoothX().at(i), z) * robinsonFx * dLonRad * radius(),
                  latRad < 0.0 ? -y : y};
}

std::optional<std::array<double, 2>>
RobinsonProjection::approximateInverse(MapPoint point) const
{
  // PROJ's robin inverse: the interval by Y, Newton's method within it.
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const double lam = point.xM / radius() / robinsonFx;
  const double target = std::abs(point.yM / radius() / robinsonFy);
  if (target >= 1.0) {
    return std::array<double, 2>{
        point.yM < 0.0 ? -halfPi() : halfPi(),
        lam / static_cast<double>(robinsonX.at(robinsonNodes)[0])};
  }
  std::size_t i =
      std::min(static_cast<std::size_t>(std::floor(target * robinsonNodes)),
               robinsonNodes - 1);
  while (i > 0 && static_cast<double>(robinsonY.at(i)[0]) > target) {
    --i;
  }
  while (i + 1 < robinsonNodes &&
         static_cast<double>(robinsonY.at(i + 1)[0]) <= target) {
    ++i;
  }
  const SmoothCubic& c = smoothY().at(i);
  double t = 5.0 * (target - c[0]) / (smoothY().at(i + 1)[0] - c[0]);
  for (int32_t iteration = 0; iteration < 100; ++iteration) {
    const double step = (cubic(c, t) - target) / cubicSlope(c, t);
    t -= step;
    if (std::abs(step) < 1e-12) {
      break;
    }
  }
  const double lat = degToRad(5.0 * static_cast<double>(i) + t);
  return std::array<double, 2>{point.yM < 0.0 ? -lat : lat,
                               lam / cubic(smoothX().at(i), t)};
}

QByteArray RobinsonProjection::glslForwardSource() const
{
  return "\n" + glslTable("tsRobinsonX", smoothX()) +
         glslTable("tsRobinsonY", smoothY()) +
         "\nfloat tsRobinsonCubic(float table[76], int i, float z)\n{\n"
         "  return table[4 * i] + z * (table[4 * i + 1] + z * (table[4 * i + "
         "2]\n"
         "         + z * table[4 * i + 3]));\n"
         "}\n"
         "\nvec2 " +
         glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float absLat = min(abs(lonLatRad.y), 1.57079633);\n"
         "  int i = min(int(floor(absLat * " +
         glslNumber(intervalsPerRad) +
         ")), 18);\n"
         "  float z = degrees(absLat - float(i) / " +
         glslNumber(intervalsPerRad) +
         ");\n"
         "  float y = tsRobinsonCubic(tsRobinsonY, i, z) * " +
         glslNumber(robinsonFy * radius()) +
         ";\n"
         "  return vec2(tsRobinsonCubic(tsRobinsonX, i, z) * " +
         glslNumber(robinsonFx * radius()) +
         " * lonLatRad.x,\n"
         "              lonLatRad.y < 0.0 ? -y : y);\n"
         "}\n";
}

std::array<std::array<float, 4>, 2> RobinsonProjection::glslParameters() const
{
  return {std::array<float, 4>{}, std::array<float, 4>{}};
}

// --- Mollweide ---------------------------------------------------------------

QString MollweideProjection::id() const
{
  return QStringLiteral("mollweide");
}

QString MollweideProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Mollweide");
}

QString MollweideProjection::projString() const
{
  return QStringLiteral("+proj=moll +lon_0=0 +x_0=0 +y_0=0 +R=6378137");
}

QByteArray MollweideProjection::glslFunction() const
{
  return "tsProjectMollweide";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint MollweideProjection::project(double latRad, double dLonRad) const
{
  const double theta = mollweideTheta(latRad);
  return MapPoint{mollweideCx() * dLonRad * std::cos(theta) * radius(),
                  mollweideCy() * std::sin(theta) * radius()};
}

std::optional<std::array<double, 2>>
MollweideProjection::approximateInverse(MapPoint point) const
{
  // PROJ's moll inverse (exact).
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const double theta =
      std::asin(std::clamp(point.yM / radius() / mollweideCy(), -1.0, 1.0));
  const double cosTheta = std::cos(theta);
  const double lam =
      cosTheta > 1e-15 ? point.xM / radius() / (mollweideCx() * cosTheta) : 0.0;
  const double lat = std::asin(
      std::clamp((2.0 * theta + std::sin(2.0 * theta)) / pi, -1.0, 1.0));
  return std::array<double, 2>{lat, lam};
}

QByteArray MollweideProjection::glslForwardSource() const
{
  // Newton on t = 2θ in float, but within 26° of a pole (|sin φ| ≥ 0.9),
  // where sin φ rounds to 1 and Newton is slow, on u = π − |t| from the
  // distance to the pole δ: u − sin u = π (1 − sin |φ|) = 2π sin²(δ/2),
  // u − sin u by its series where it cancels; cos θ = sin(u/2) then.
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float s = sin(lonLatRad.y);\n"
         "  float cosTheta;\n"
         "  float sinTheta;\n"
         "  if (abs(s) < 0.9) {\n"
         "    float k = 3.14159265 * s;\n"
         "    float t = lonLatRad.y;\n"
         "    for (int i = 0; i < 8; ++i) {\n"
         "      t -= (t + sin(t) - k) / (1.0 + cos(t));\n"
         "    }\n"
         "    cosTheta = cos(0.5 * t);\n"
         "    sinTheta = sin(0.5 * t);\n"
         "  } else {\n"
         "    float d = max(1.57079633 - abs(lonLatRad.y), 0.0);\n"
         "    float g = 6.28318531 * sin(0.5 * d) * sin(0.5 * d);\n"
         "    float u = pow(6.0 * g, 1.0 / 3.0);\n"
         "    for (int i = 0; i < 8; ++i) {\n"
         "      float h = u < 0.5\n"
         "          ? u * u * u * (1.0 / 6.0 - u * u * (1.0 / 120.0 - u * u / "
         "5040.0))\n"
         "          : u - sin(u);\n"
         "      float dh = 2.0 * sin(0.5 * u) * sin(0.5 * u);\n"
         "      u -= (h - g) / max(dh, 1e-30);\n"
         "    }\n"
         "    cosTheta = sin(0.5 * u);\n"
         "    sinTheta = sign(lonLatRad.y) * cos(0.5 * u);\n"
         "  }\n"
         "  return vec2(" +
         glslNumber(mollweideCx() * radius()) +
         " * lonLatRad.x * cosTheta,\n"
         "              " +
         glslNumber(mollweideCy() * radius()) +
         " * sinTheta);\n"
         "}\n";
}

double
// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MollweideProjection::localExpansionLimitRadAt(double latRad,
                                              double dLonRad) const
{
  // Near a pole y goes as the distance from it to the power 4/3: not
  // smooth there, so a tile uses the expansion only if it is small beside
  // that distance.
  Q_UNUSED(dLonRad)
  return std::min(localExpansionLimitRad(),
                  0.02 * std::max(halfPi() - std::abs(latRad), 0.0));
}

std::array<std::array<float, 4>, 2> MollweideProjection::glslParameters() const
{
  return {std::array<float, 4>{}, std::array<float, 4>{}};
}

// --- Winkel Tripel -----------------------------------------------------------

QString WinkelTripelProjection::id() const
{
  return QStringLiteral("winkel-tripel");
}

QString WinkelTripelProjection::displayName() const
{
  return QCoreApplication::translate("ts::Projection", "Winkel Tripel");
}

QString WinkelTripelProjection::projString() const
{
  return QStringLiteral("+proj=wintri +lon_0=0 +x_0=0 +y_0=0 +R=6378137");
}

QByteArray WinkelTripelProjection::glslFunction() const
{
  return "tsProjectWinkelTripel";
}

// Latitude then longitude, as everywhere in Projection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MapPoint WinkelTripelProjection::project(double latRad, double dLonRad) const
{
  // PROJ's aitoff forward in its Winkel Tripel mode: Aitoff, then the mean
  // with the equirectangular. d = acos(cos φ cos c) by its half-angle,
  // sin²(d/2) = a + b − 2ab with a = sin²(φ/2), b = sin²(c/2): PROJ's acos
  // rounds to 0 for offsets of 1e-8 or so, and its x to the wrong slope.
  const double c = 0.5 * dLonRad;
  const double a = std::pow(std::sin(0.5 * latRad), 2.0);
  const double b = std::pow(std::sin(0.5 * c), 2.0);
  const double halfD =
      std::asin(std::sqrt(std::clamp(a + b - 2.0 * a * b, 0.0, 1.0)));
  double x = 0.0;
  double y = 0.0;
  if (halfD != 0.0) {
    const double k = halfD / (std::sin(halfD) * std::cos(halfD));
    x = 2.0 * k * std::cos(latRad) * std::sin(c);
    y = k * std::sin(latRad);
  }
  return MapPoint{0.5 * (x + dLonRad * winkelCosPhi1) * radius(),
                  0.5 * (y + latRad) * radius()};
}

std::optional<std::array<double, 2>>
WinkelTripelProjection::approximateInverse(MapPoint point) const
{
  // Near the centre x ≈ (1 + cos φ1) / 2 · λ cos φ-ish and y ≈ φ: a
  // first guess for Newton's method on project().
  if (!(std::isfinite(point.xM) && std::isfinite(point.yM))) {
    return std::nullopt;
  }
  const double lat = std::clamp(point.yM / radius(), -halfPi(), halfPi());
  const double across = 0.5 * (std::cos(lat) + winkelCosPhi1);
  const double lon =
      across > 1e-6 ? std::clamp(point.xM / radius() / across, -pi, pi) : 0.0;
  return std::array<double, 2>{lat, lon};
}

QByteArray WinkelTripelProjection::glslForwardSource() const
{
  // d / sin d by d's half-angle, as on the CPU: precise near d = 0.
  return "\nvec2 " + glslFunction() +
         "Forward(vec2 lonLatRad, vec4 p0, vec4 p1)\n{\n"
         "  float c = 0.5 * lonLatRad.x;\n"
         "  float sa = sin(0.5 * lonLatRad.y);\n"
         "  float sb = sin(0.5 * c);\n"
         "  float a = sa * sa;\n"
         "  float b = sb * sb;\n"
         "  float sh = sqrt(clamp(a + b - 2.0 * a * b, 0.0, 1.0));\n"
         "  float ch = sqrt(max(1.0 - sh * sh, 0.0));\n"
         "  float k = sh > 1e-6 ? tsAtan2(sh, ch) / (sh * ch) : 1.0;\n"
         "  vec2 aitoff = k * vec2(2.0 * cos(lonLatRad.y) * sin(c), "
         "sin(lonLatRad.y));\n"
         "  return " +
         glslNumber(0.5 * radius()) + " * (aitoff + vec2(lonLatRad.x * " +
         glslNumber(winkelCosPhi1) +
         ", lonLatRad.y));\n"
         "}\n";
}

std::array<std::array<float, 4>, 2>
WinkelTripelProjection::glslParameters() const
{
  return {std::array<float, 4>{}, std::array<float, 4>{}};
}

} // namespace ts
