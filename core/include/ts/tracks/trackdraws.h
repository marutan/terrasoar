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
 * Tracks as the globe draws them: trails as timed SpaceLines.
 */
#ifndef TS_TRACKS_TRACKDRAWS_H
#define TS_TRACKS_TRACKDRAWS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "ts/globe/globerenderer.h"
#include "ts/tracks/movingtrack.h"

namespace ts
{

/** Gives a trail point's colour from its report (RGBA). */
using TrailColourFunction =
    std::function<std::array<std::uint8_t, 4>(const TrackSample&)>;

/** How a trail is drawn. */
struct TrailLook {
  /** Colour, if colourOf is not set. */
  std::array<std::uint8_t, 4> rgba = {255, 255, 255, 255};
  /** Each point's colour (by altitude, vertical rate…), if set. */
  TrailColourFunction colourOf;
  /** Width, logical pixels. */
  float widthPx = 2.0F;
  /**
   * Points no farther than this from the line through their neighbours
   * are left out, metres (0: none): a straight leg needs only its ends.
   */
  double simplifyM = 10.0;
  /** The height of reports without one (on the ground), metres. */
  double groundHeightM = 0.0;
};

/**
 * Returns @p track's trail as timed SpaceLines, one for each run of
 * reports without a gap (@p timing), shown @p trail's span back from the
 * renderer's line time. Every report is in them (less those @p look's
 * simplification leaves out), so the line time can move anywhere in the
 * track with nothing rebuilt; the shader cuts each line at the time and
 * the span. Nothing for TrailMode::Off.
 */
std::vector<SpaceLine> trailLines(const MovingTrack& track, TrackTiming timing,
                                  TrailSettings trail, const TrailLook& look);

/** Returns the ECEF position of @p point (its height, or @p groundM). */
Vec3 trackSampleEcef(const TrackSample& point, double groundM);

/**
 * Returns the indices of @p points kept when those within @p toleranceM
 * of the line between the kept ones around them are left out (the first
 * and last are always kept).
 */
std::vector<std::size_t> simplifyTrack(const std::vector<TrackSample>& points,
                                       std::size_t first, std::size_t last,
                                       double toleranceM, double groundM);

} // namespace ts

#endif // TS_TRACKS_TRACKDRAWS_H
