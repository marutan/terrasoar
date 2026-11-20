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
 * Satellites as the globe draws them: markers, tracks and footprints as
 * GlobeMarkers and SpaceLines.
 */
#ifndef TS_ORBITS_SATELLITEDRAWS_H
#define TS_ORBITS_SATELLITEDRAWS_H

#include <array>
#include <cstdint>
#include <vector>

#include "ts/globe/globerenderer.h"
#include "ts/orbits/satellitetrack.h"

namespace ts
{

/** How one satellite's track is drawn. */
struct TrackLook {
  /** Colour, 8-bit RGBA, not premultiplied. */
  std::array<std::uint8_t, 4> rgba = {255, 204, 0, 255};
  /** Width, logical pixels. */
  float widthPx = 1.5F;
  /** The opacity the past track fades to at its oldest end. */
  float oldestAlpha = 0.15F;
  /** Dashes of the future track per orbit. */
  double dashesPerOrbit = 60.0;
  /** The satellite's period, minutes (for the dashes). */
  double periodMinutes = 90.0;
};

/**
 * Returns the past part of @p track as a solid line fading with age (to
 * TrackLook::oldestAlpha at its start); nothing drawn for fewer than two
 * points.
 */
SpaceLine pastTrackLine(const SatelliteTrack& track, const TrackLook& look);

/** Returns the future part of @p track as a dashed line. */
SpaceLine futureTrackLine(const SatelliteTrack& track, const TrackLook& look);

/**
 * Returns the ground track below @p track (past and future, height 0), for
 * the globe only (the map draws the tracks there as ground tracks), dimmer.
 */
std::vector<SpaceLine> groundTrackLines(const SatelliteTrack& track,
                                        const TrackLook& look);

/**
 * Returns the footprint of a satellite at @p state seen above
 * @p minElevationDeg, as a closed line on the ground.
 */
SpaceLine footprintLine(const SatelliteState& state, double minElevationDeg,
                        const TrackLook& look);

/** Returns a marker at @p state's position. */
GlobeMarker satelliteMarker(const SatelliteState& state,
                            std::array<std::uint8_t, 4> rgba, double radiusPx);

} // namespace ts

#endif // TS_ORBITS_SATELLITEDRAWS_H
