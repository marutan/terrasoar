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
 * The world maps: Robinson, Mollweide and Winkel Tripel.
 */
#ifndef TS_PROJECTION_WORLDMAPS_H
#define TS_PROJECTION_WORLDMAPS_H

#include "ts/projection/localexpansion.h"

namespace ts
{

/**
 * A map of the whole world in one piece, on the sphere of radius a as
 * PROJ makes these (its `robin`, `moll` and `wintri` ignore the
 * ellipsoid), centred on Greenwich. Its edge is the antimeridian, where
 * tiles end, so nothing is drawn across the map; another central
 * meridian would need tiles cut at its own antimeridian. The map ends at
 * that edge (it doesn't repeat).
 *
 * @threadsafe
 */
class WorldMapProjection : public LocalExpansionProjection
{
protected:
  WorldMapProjection() : LocalExpansionProjection(0.0) {}
};

/**
 * Robinson (PROJ's `robin`): a compromise for world maps, neither
 * conformal nor equal-area, interpolated from Robinson's table at 5°
 * intervals with PROJ's cubic coefficients.
 *
 * @threadsafe
 */
class RobinsonProjection : public WorldMapProjection
{
public:
  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
};

/**
 * Mollweide (PROJ's `moll`): equal-area, the world an ellipse twice as
 * wide as it is tall.
 *
 * @threadsafe
 */
class MollweideProjection : public WorldMapProjection
{
public:
  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  MapPoint project(double latRad, double dLonRad) const override;
  int32_t geometryLevelsBeyondTexels() const override { return 2; }

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
  double localExpansionLimitRadAt(double latRad, double dLonRad) const override;
};

/**
 * Winkel Tripel (PROJ's `wintri`): the mean of Aitoff and Equirectangular
 * with standard parallel acos(2/π), the compromise of many world atlases.
 *
 * @threadsafe
 */
class WinkelTripelProjection : public WorldMapProjection
{
public:
  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
};

} // namespace ts

#endif // TS_PROJECTION_WORLDMAPS_H
