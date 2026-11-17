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
 * Elevation profiles: the heights along a path over the ground.
 */
#ifndef TS_ELEVATION_ELEVATIONPROFILE_H
#define TS_ELEVATION_ELEVATIONPROFILE_H

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ts/providers/tileprovider.h"

namespace ts
{

/** One point of an elevation profile. */
struct ProfilePoint {
  /** Distance along the path from its start, metres on the ellipsoid. */
  double distanceM = 0.0;
  /** Where it is. */
  GeoPoint position{};
  /** The ground's height above the WGS 84 ellipsoid, metres. */
  double ellipsoidalM = 0.0;
  /** Sea level (the geoid) above the ellipsoid there, metres. */
  double seaLevelM = 0.0;
  /** Returns the height above sea level, metres. */
  double aboveSeaLevelM() const { return ellipsoidalM - seaLevelM; }
};

/** The heights along a path (computeElevationProfile()). */
struct ElevationProfile {
  /** The points, evenly spaced along the path, first and last included. */
  std::vector<ProfilePoint> points;
  /** The path's length, metres. */
  double lengthM = 0.0;
  /** The level of the height tiles sampled. */
  int32_t level = 0;
  /** Lowest height above sea level, metres. */
  double minimumM = 0.0;
  /** Highest height above sea level, metres. */
  double maximumM = 0.0;
  /** Sum of the rises between points, metres. */
  double ascentM = 0.0;
  /** Sum of the falls between points, metres (positive). */
  double descentM = 0.0;
};

/**
 * Returns @p count points evenly spaced along @p vertices (at least two),
 * each leg a geodesic on WGS 84, with their distances from the start; the
 * vertices themselves are where the spacing puts points, not necessarily
 * among them. Empty for fewer than two vertices or @p count < 2.
 */
std::vector<std::pair<double, GeoPoint>>
samplePath(const std::vector<GeoPoint>& vertices, int32_t count);

/**
 * Returns the level of height tiles whose samples are about @p spacingM
 * apart (the coarsest not wider), at most @p maximumLevel.
 */
// A spacing, then a level cap: different things, different types.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int32_t profileLevel(double spacingM, int32_t maximumLevel);

/**
 * Samples @p provider at @p count points along @p vertices (see
 * samplePath()), from tiles of the level matching their spacing
 * (profileLevel(), at most the provider's deepest level), loaded once
 * each. Blocking: call it on a worker thread. Returns nothing if the path
 * is too short, a tile has no heights, or it was cancelled.
 */
std::optional<ElevationProfile>
computeElevationProfile(ElevationProvider& provider,
                        const std::vector<GeoPoint>& vertices, int32_t count,
                        const std::atomic<bool>& cancelled);

} // namespace ts

#endif // TS_ELEVATION_ELEVATIONPROFILE_H
