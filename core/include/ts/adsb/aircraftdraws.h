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
 * Aircraft as the globe draws them: markers, drop lines and the colours
 * of trails by altitude and vertical rate.
 */
#ifndef TS_ADSB_AIRCRAFTDRAWS_H
#define TS_ADSB_AIRCRAFTDRAWS_H

#include <array>
#include <cstdint>
#include <vector>

#include "ts/globe/globerenderer.h"
#include "ts/tracks/movingtrack.h"

namespace ts
{

/** What a trail's colour shows. */
enum class TrailColouring : uint8_t {
  Aircraft,     ///< The aircraft's (layer's) colour.
  Altitude,     ///< The height, by altitudeColour().
  VerticalRate, ///< Climbing, level or descending, by verticalRateColour().
};

/** One stop of the altitude colour scale. */
struct AltitudeStop {
  /** Height, metres. */
  double heightM = 0.0;
  /** Colour there, RGBA. */
  std::array<std::uint8_t, 4> rgba{};
};

/** Returns the altitude colour scale's stops, low to high (the legend). */
const std::vector<AltitudeStop>& altitudeStops();

/**
 * Returns the colour of a height (metres above the ellipsoid; NaN, on the
 * ground, is grey), interpolated between altitudeStops().
 */
std::array<std::uint8_t, 4> altitudeColour(double heightM);

/**
 * Returns the colour of a vertical rate (metres a second): blue
 * descending, grey level, orange climbing, full at 10 m/s (about 2000 ft a
 * minute); grey if NaN.
 */
std::array<std::uint8_t, 4> verticalRateColour(double rateMS);

/** Returns the colour of trail point @p p for @p colouring. */
std::array<std::uint8_t, 4> trailColour(const TrackSample& p,
                                        TrailColouring colouring,
                                        std::array<std::uint8_t, 4> rgba);

/**
 * Returns an aircraft-shaped marker at @p at (its height, or @p groundM
 * on the ground), nose along its track.
 */
GlobeMarker aircraftMarker(const TrackSample& at, double groundM,
                           std::array<std::uint8_t, 4> rgba, double radiusPx);

/**
 * Returns a line straight down from @p at to @p groundM (dimmer), which
 * makes heights readable in 3D; drawn on the globe only.
 */
SpaceLine dropLine(const TrackSample& at, double groundM,
                   std::array<std::uint8_t, 4> rgba);

} // namespace ts

#endif // TS_ADSB_AIRCRAFTDRAWS_H
