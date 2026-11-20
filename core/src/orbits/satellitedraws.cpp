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
#include "ts/orbits/satellitedraws.h"

#include <algorithm>

#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

// An opacity, then a distance along.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SpaceLinePoint linePoint(const TrackPoint& p, float alpha, float along)
{
  SpaceLinePoint point;
  point.positionEcef = p.positionEcefM;
  point.latDeg = p.geodetic.latDeg;
  point.lonDeg = p.geodetic.lonDeg;
  point.alpha = alpha;
  point.along = along;
  return point;
}

SpaceLine lineOf(const TrackLook& look)
{
  SpaceLine line;
  line.rgba = look.rgba;
  line.widthPx = look.widthPx;
  return line;
}

} // namespace

SpaceLine pastTrackLine(const SatelliteTrack& track, const TrackLook& look)
{
  SpaceLine line = lineOf(look);
  if (track.past.size() < 2) {
    return line;
  }
  const double oldest = track.past.front().minutes;
  for (const TrackPoint& p : track.past) {
    // Full at the time, fading linearly to the oldest end.
    const double age = oldest < 0.0 ? p.minutes / oldest : 0.0;
    const float alpha = static_cast<float>(
        1.0 - (1.0 - static_cast<double>(look.oldestAlpha)) * age);
    line.points.push_back(linePoint(p, alpha, 0.0F));
  }
  return line;
}

SpaceLine futureTrackLine(const SatelliteTrack& track, const TrackLook& look)
{
  SpaceLine line = lineOf(look);
  line.dashed = true;
  if (track.future.size() < 2) {
    return line;
  }
  const double dashMinutes =
      std::max(look.periodMinutes, 1.0) / std::max(look.dashesPerOrbit, 1.0);
  for (const TrackPoint& p : track.future) {
    line.points.push_back(
        linePoint(p, 1.0F, static_cast<float>(p.minutes / dashMinutes)));
  }
  return line;
}

std::vector<SpaceLine> groundTrackLines(const SatelliteTrack& track,
                                        const TrackLook& look)
{
  std::vector<SpaceLine> lines;
  for (const std::vector<TrackPoint>* part : {&track.past, &track.future}) {
    SpaceLine line = lineOf(look);
    line.globeOnly = true;
    line.dashed = part == &track.future;
    line.widthPx = std::max(1.0F, look.widthPx - 0.5F);
    const double dashMinutes =
        std::max(look.periodMinutes, 1.0) / std::max(look.dashesPerOrbit, 1.0);
    for (const TrackPoint& p : *part) {
      Geodetic below = p.geodetic;
      below.heightM = 0.0;
      TrackPoint ground = p;
      ground.positionEcefM = geodeticToEcef(below);
      line.points.push_back(
          linePoint(ground, 0.6F, static_cast<float>(p.minutes / dashMinutes)));
    }
    if (line.points.size() >= 2) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

SpaceLine footprintLine(const SatelliteState& state, double minElevationDeg,
                        const TrackLook& look)
{
  SpaceLine line = lineOf(look);
  line.closed = true;
  if (!state.ok()) {
    return line;
  }
  for (const LonLat& p : footprint(state.geodetic, minElevationDeg, 90)) {
    SpaceLinePoint point;
    point.latDeg = p.latDeg;
    point.lonDeg = p.lonDeg;
    point.positionEcef = geodeticToEcef(Geodetic{p.latDeg, p.lonDeg, 0.0});
    point.alpha = 0.8F;
    line.points.push_back(point);
  }
  return line;
}

GlobeMarker satelliteMarker(const SatelliteState& state,
                            std::array<std::uint8_t, 4> rgba, double radiusPx)
{
  GlobeMarker marker;
  marker.positionEcef = state.positionEcefM;
  marker.rgba = rgba;
  marker.radiusPx = radiusPx;
  return marker;
}

} // namespace ts
