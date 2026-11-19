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
/**
 * @file
 * The ARC (Equal Arc-second Raster Chart) system of the Raster Product
 * Format (RPF): CADRG maps and CIB imagery are made in its zones.
 */
#ifndef TS_PROJECTION_ARC_H
#define TS_PROJECTION_ARC_H

#include <array>
#include <cstdint>

#include "ts/projection/localexpansion.h"
#include "ts/projection/normalcylindrical.h"

namespace ts::arc
{

/** Zones: 1 to 9 north (9 polar), 10 to 18 south (18 polar; RPF writes
 *  them A to H and J). */
constexpr int32_t zoneCount = 18;

/** Returns the zone of latitude @p latDeg (on a boundary, the one nearer
 *  the equator, as RPF assigns frames). */
int32_t zoneAt(double latDeg);

/** Returns true if @p zone (1–18) is a polar zone (9 or 18). */
bool isPolar(int32_t zone);

/** Returns the latitudes @p zone covers, {south, north}, degrees. */
std::array<double, 2> zoneLatitudesDeg(int32_t zone);

/**
 * Returns the size of a pixel of a CADRG product of scale 1 :
 * @p reciprocalScale in non-polar @p zone, {east–west, north–south},
 * degrees (MIL-C-89038 60.1, from MIL-A-89007 appendix 70 table III's
 * pixel constants at 1:1M: rounded up to ADRG's 512-pixel blocks, then to
 * CADRG's 256-pixel ones at 1.5 times the ADRG pitch).
 */
std::array<double, 2> pixelSizeDeg(int32_t zone, double reciprocalScale);

/**
 * Returns the size of a pixel of a CADRG product of scale 1 :
 * @p reciprocalScale in a polar zone, metres (square).
 */
double polarPixelSizeM(double reciprocalScale);

/** Returns the RPF letter or digit of @p zone ('1'…'9', 'A'…'H', 'J'). */
char zoneCode(int32_t zone);

} // namespace ts::arc

namespace ts
{

/**
 * An ARC zone outside the poles (1–8 north, 10–17 south): equirectangular
 * over the zone's band of latitude with the east–west to north–south
 * ratio of a CADRG product's pixels at its scale, so the product's frames
 * are drawn pixel for pixel. y = a φ (true along the meridians), x =
 * a k λ with k the ratio of the pixel's height to its width in degrees.
 * The map repeats east–west like any cylindrical one. Its band is a domain
 * of its own: beyond it the shaders discard (nothing is squashed onto
 * the band's edges, which would draw the rest of the world along them).
 *
 * Zone 0, the default, follows the view: effectiveAt() is the zone of the
 * view's centre (a polar one north of 80° or south of −80°), so the map
 * changes zone as the view moves. Its own formulas are zone 1's.
 *
 * @threadsafe
 */
class ArcZoneProjection : public NormalCylindricalProjection
{
public:
  /**
   * Creates zone @p zone (1–8, 10–17; 0 to follow the view) for products
   * of scale 1 : @p reciprocalScale.
   */
  explicit ArcZoneProjection(int32_t zone = 0, double reciprocalScale = 1.0e6);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  QByteArray glslSource() const override;
  ProjectionShaderReference shaderReference(double latDeg,
                                            double lonDeg) const override;
  const Projection* effectiveAt(double latDeg, double lonDeg) const override;
  double domainMarginDeg(double latDeg, double lonDeg) const override;
  bool hasOwnDomain() const override { return true; }
  std::array<float, 8> glslDomainParameters() const override;
  QByteArray glslDomainSource() const override;

  /** Returns the zone, 0 when following the view. */
  int32_t zone() const { return m_zone; }

protected:
  double unitNorthing(double latRad) const override;
  double unitLatitude(double unitY) const override;
  double unitNorthingDerivative(double latRad) const override;

private:
  int32_t m_zone;
  double m_reciprocalScale;
};

/**
 * An ARC polar zone (9 north, 18 south): the azimuthal equidistant about
 * the pole on the sphere of radius a (as GDAL georeferences CADRG polar
 * frames), the prime meridian straight down from the north pole (up from
 * the south). Shown north of 80° N (south of 80° S); the map goes all the
 * way round the pole.
 *
 * @threadsafe
 */
class ArcPolarProjection : public LocalExpansionProjection
{
public:
  /** Creates the north (@p north) or south polar zone for products of
   *  scale 1 : @p reciprocalScale. */
  explicit ArcPolarProjection(bool north = true,
                              double reciprocalScale = 1.0e6);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double minimumLatDeg() const override { return m_north ? 80.0 : -90.0; }
  double maximumLatDeg() const override { return m_north ? 90.0 : -80.0; }
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;

private:
  bool m_north;
  double m_reciprocalScale;
};

} // namespace ts

#endif // TS_PROJECTION_ARC_H
