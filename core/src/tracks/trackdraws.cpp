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
#include "ts/tracks/trackdraws.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

/** The distance of @p p from the segment from @p a to @p b, metres. */
double distanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b)
{
  const Vec3 ab = b - a;
  const double ab2 = dot(ab, ab);
  const double t = ab2 > 0.0 ? std::clamp(dot(p - a, ab) / ab2, 0.0, 1.0) : 0.0;
  return length(p - (a + ab * t));
}

} // namespace

Vec3 trackSampleEcef(const TrackSample& point, double groundM)
{
  return geodeticToEcef(
      Geodetic{point.latDeg, point.lonDeg,
               std::isnan(point.heightM) ? groundM : point.heightM});
}

std::vector<std::size_t> simplifyTrack(const std::vector<TrackSample>& points,
                                       std::size_t first, std::size_t last,
                                       double toleranceM, double groundM)
{
  std::vector<std::size_t> kept;
  if (last < first || last >= points.size()) {
    return kept;
  }
  kept.push_back(first);
  if (toleranceM <= 0.0) {
    for (std::size_t i = first + 1; i <= last; ++i) {
      kept.push_back(i);
    }
    return kept;
  }
  // Greedy: from the last kept point, go on while every point skipped
  // stays within the tolerance of the line to the next candidate.
  Vec3 anchor = trackSampleEcef(points[first], groundM);
  std::size_t from = first;
  std::size_t i = first + 1;
  while (i < last) {
    const Vec3 next = trackSampleEcef(points[i + 1], groundM);
    bool within = true;
    for (std::size_t k = from + 1; k <= i; ++k) {
      if (distanceToSegment(trackSampleEcef(points[k], groundM), anchor, next) >
          toleranceM) {
        within = false;
        break;
      }
    }
    // Long runs are cut regardless, so the check stays cheap.
    if (!within || i - from >= 64) {
      kept.push_back(i);
      anchor = trackSampleEcef(points[i], groundM);
      from = i;
    }
    ++i;
  }
  if (last > first) {
    kept.push_back(last);
  }
  return kept;
}

std::vector<SpaceLine> trailLines(const MovingTrack& track,
                                  const TrackTiming& timing,
                                  const TrailSettings& trail,
                                  const TrailLook& look)
{
  std::vector<SpaceLine> lines;
  if (trail.mode == TrailMode::Off) {
    return lines;
  }
  const std::vector<TrackSample>& points = track.points();
  for (const TrackPiece& piece : track.pieces(timing)) {
    if (piece.last == piece.first) {
      continue;
    }
    SpaceLine line;
    line.rgba = look.rgba;
    line.widthPx = look.widthPx;
    line.timed = true;
    line.shownS = trail.shownS();
    line.pointColours = static_cast<bool>(look.colourOf);
    const std::vector<std::size_t> kept = simplifyTrack(
        points, piece.first, piece.last, look.simplifyM, look.groundHeightM);
    line.points.reserve(kept.size());
    for (const std::size_t i : kept) {
      const TrackSample& p = points[i];
      SpaceLinePoint point;
      point.positionEcef = trackSampleEcef(p, look.groundHeightM);
      point.latDeg = p.latDeg;
      point.lonDeg = p.lonDeg;
      point.timeS = p.timeS;
      if (look.colourOf) {
        point.rgba = look.colourOf(p);
      }
      line.points.push_back(point);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

} // namespace ts
