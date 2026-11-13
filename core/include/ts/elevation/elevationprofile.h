// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Elevation profiles: the heights along a path over the ground.
 */
#ifndef TS_ELEVATION_ELEVATIONPROFILE_H
#define TS_ELEVATION_ELEVATIONPROFILE_H

#include <atomic>
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
  int level = 0;
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
samplePath(const std::vector<GeoPoint>& vertices, int count);

/**
 * Returns the level of height tiles whose samples are about @p spacingM
 * apart (the coarsest not wider), at most @p maximumLevel.
 */
// A spacing, then a level cap: different things, different types.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int profileLevel(double spacingM, int maximumLevel);

/**
 * Samples @p provider at @p count points along @p vertices (see
 * samplePath()), from tiles of the level matching their spacing
 * (profileLevel(), at most the provider's deepest level), loaded once
 * each. Blocking: call it on a worker thread. Returns nothing if the path
 * is too short, a tile has no heights, or it was cancelled.
 */
std::optional<ElevationProfile>
computeElevationProfile(ElevationProvider& provider,
                        const std::vector<GeoPoint>& vertices, int count,
                        const std::atomic<bool>& cancelled);

} // namespace ts

#endif // TS_ELEVATION_ELEVATIONPROFILE_H
