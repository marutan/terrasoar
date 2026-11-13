// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/elevationprofile.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <utility>

#include "ts/elevation/heightgrid.h"
#include "ts/geo/geodesic.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

namespace
{

/** Metres in a degree of latitude, near enough to choose a level. */
constexpr double metresPerDegree = 111320.0;

/** Samples along a height tile's side, as the providers deliver them. */
constexpr int tileSamples = 65;

} // namespace

std::vector<std::pair<double, GeoPoint>>
samplePath(const std::vector<GeoPoint>& vertices, int count)
{
  std::vector<std::pair<double, GeoPoint>> samples;
  if (vertices.size() < 2 || count < 2) {
    return samples;
  }
  std::vector<GeodesicInverse> legs;
  std::vector<double> starts;
  double length = 0.0;
  for (std::size_t i = 1; i < vertices.size(); ++i) {
    legs.push_back(geodesicInverse(vertices[i - 1].latDeg,
                                   vertices[i - 1].lonDeg, vertices[i].latDeg,
                                   vertices[i].lonDeg));
    starts.push_back(length);
    length += legs.back().distanceM;
  }
  samples.reserve(static_cast<std::size_t>(count));
  std::size_t leg = 0;
  for (int i = 0; i < count; ++i) {
    const double distance = i == count - 1 ? length : length * i / (count - 1);
    while (leg + 1 < legs.size() && distance > starts[leg + 1]) {
      ++leg;
    }
    if (i == count - 1) {
      samples.emplace_back(distance, vertices.back());
      continue;
    }
    const GeoPoint& from = vertices[leg];
    const GeodesicDirect at =
        geodesicDirect(from.latDeg, from.lonDeg, legs[leg].azimuth1Deg,
                       distance - starts[leg]);
    samples.emplace_back(distance, GeoPoint{at.latDeg, at.lonDeg});
  }
  return samples;
}

// A spacing, then a level cap: different things, different types.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int profileLevel(double spacingM, int maximumLevel)
{
  for (int level = 0; level < maximumLevel; ++level) {
    const double sampleM =
        TileKey::sizeDeg(level) / (tileSamples - 1) * metresPerDegree;
    if (sampleM <= spacingM) {
      return level;
    }
  }
  return std::max(maximumLevel, 0);
}

std::optional<ElevationProfile>
computeElevationProfile(ElevationProvider& provider,
                        const std::vector<GeoPoint>& vertices, int count,
                        const std::atomic<bool>& cancelled)
{
  const std::vector<std::pair<double, GeoPoint>> path =
      samplePath(vertices, count);
  if (path.empty() || path.back().first <= 0.0) {
    return std::nullopt;
  }
  ElevationProfile profile;
  profile.lengthM = path.back().first;
  profile.level =
      profileLevel(profile.lengthM / (count - 1), provider.maximumLevel());
  std::unordered_map<TileKey, std::shared_ptr<const HeightTile>, TileKeyHash>
      tiles;
  for (const std::pair<double, GeoPoint>& sample : path) {
    if (cancelled.load()) {
      return std::nullopt;
    }
    const GeoPoint& at = sample.second;
    const TileKey key =
        TileKey::containing(profile.level, Geodetic{at.latDeg, at.lonDeg, 0.0});
    std::unordered_map<TileKey, std::shared_ptr<const HeightTile>,
                       TileKeyHash>::iterator tile = tiles.find(key);
    if (tile == tiles.end()) {
      std::optional<HeightTile> loaded = provider.loadHeights(key, cancelled);
      tile = tiles
                 .emplace(key, loaded ? std::make_shared<const HeightTile>(
                                            std::move(*loaded))
                                      : nullptr)
                 .first;
    }
    if (!tile->second) {
      return std::nullopt;
    }
    // A point on a tile's edge may round outside it: clamp into it.
    const GeoRect bounds = tile->first.bounds();
    const double lat = std::clamp(at.latDeg, bounds.southDeg, bounds.northDeg);
    const double lon = std::clamp(at.lonDeg, bounds.westDeg, bounds.eastDeg);
    const std::optional<double> height = sampleHeightM(*tile->second, lat, lon);
    if (!height || std::isnan(*height)) {
      return std::nullopt;
    }
    ProfilePoint point;
    point.distanceM = sample.first;
    point.position = at;
    point.ellipsoidalM = *height;
    point.seaLevelM = sampleSeaLevelM(*tile->second, lat, lon).value_or(0.0);
    profile.points.push_back(point);
  }
  profile.minimumM = profile.points.front().aboveSeaLevelM();
  profile.maximumM = profile.minimumM;
  for (std::size_t i = 0; i < profile.points.size(); ++i) {
    const double height = profile.points[i].aboveSeaLevelM();
    profile.minimumM = std::min(profile.minimumM, height);
    profile.maximumM = std::max(profile.maximumM, height);
    if (i > 0) {
      const double change = height - profile.points[i - 1].aboveSeaLevelM();
      (change > 0.0 ? profile.ascentM : profile.descentM) += std::abs(change);
    }
  }
  return profile;
}

} // namespace ts
