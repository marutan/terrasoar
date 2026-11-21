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
 * Vessels as the globe draws them: symbols by kind and status, turned to
 * the heading (or course), hulls to scale, course vectors, and the
 * colours of ship types and speeds.
 */
#ifndef TS_AIS_VESSELDRAWS_H
#define TS_AIS_VESSELDRAWS_H

#include <array>
#include <cstdint>
#include <vector>

#include <QString>

#include "ts/ais/vesseltracks.h"
#include "ts/globe/globerenderer.h"

namespace ts
{

/** The groups of ship types coloured alike (the usual AIS scheme). */
enum class ShipTypeGroup : uint8_t {
  Unknown,    ///< Not known (0).
  Cargo,      ///< 70–79.
  Tanker,     ///< 80–89.
  Passenger,  ///< 60–69.
  HighSpeed,  ///< 40–49.
  TugSpecial, ///< Tugs, pilots, special craft: 31, 32, 50–59.
  Fishing,    ///< 30.
  Pleasure,   ///< Sailing and pleasure craft: 36, 37.
  Other,      ///< Everything else.
  Navigation, ///< Aids to navigation and base stations.
};

/** Returns the group of AIS ship type @p shipType. */
ShipTypeGroup shipTypeGroup(int32_t shipType);

/** Returns the colour of @p group, RGBA. */
std::array<std::uint8_t, 4> shipTypeColour(ShipTypeGroup group);

/** Returns the groups in the legend's order. */
const std::vector<ShipTypeGroup>& shipTypeLegend();

/** What a vessel's trail colour shows. */
enum class VesselColouring : uint8_t {
  Vessel, ///< The layer's colour.
  Type,   ///< The ship type's.
  Speed,  ///< The speed, by speedColour().
};

/**
 * Returns the colour of a speed (metres a second): grey stopped, then
 * blue, green, yellow and red at 25 knots and more; grey if NaN.
 */
std::array<std::uint8_t, 4> speedColour(double speedMS);

/**
 * Returns the direction a vessel points: its heading, or its course when
 * it has none (NaN if neither).
 */
double vesselDirectionDeg(const TrackSample& at, const VesselMotion& motion);

/**
 * Returns the marker of station @p info at @p at (at @p seaLevelM unless
 * it has a height): a ship pointing along its direction (round when moored
 * or at anchor or not moving and without a heading), an aircraft for SAR
 * aircraft, a diamond for aids to navigation, a square for base stations;
 * the hull to scale from the dimensions when known.
 */
GlobeMarker vesselMarker(const TrackSample& at, const VesselMotion& motion,
                         const VesselInfo& info, double seaLevelM,
                         std::array<std::uint8_t, 4> rgba, double radiusPx);

/**
 * Returns a line from @p at to where it will be in @p minutes at its speed
 * and course (nothing drawn if it has neither, or isn't moving).
 */
SpaceLine courseVector(const TrackSample& at, double minutes, double seaLevelM,
                       std::array<std::uint8_t, 4> rgba);

} // namespace ts

#endif // TS_AIS_VESSELDRAWS_H
