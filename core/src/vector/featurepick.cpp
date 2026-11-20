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
#include "ts/vector/featurepick.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ts
{

namespace
{

constexpr double pi = 3.14159265358979323846;

// The point, then the segment's ends, then the scale of longitude.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double segmentDistance(GeoPoint p, GeoPoint a, GeoPoint b, double lonScale)
{
  const double ax = (a.lonDeg - p.lonDeg) * lonScale;
  const double ay = a.latDeg - p.latDeg;
  const double bx = (b.lonDeg - p.lonDeg) * lonScale;
  const double by = b.latDeg - p.latDeg;
  const double dx = bx - ax;
  const double dy = by - ay;
  const double length2 = dx * dx + dy * dy;
  const double t = length2 > 0.0
                       ? std::clamp(-(ax * dx + ay * dy) / length2, 0.0, 1.0)
                       : 0.0;
  return std::hypot(ax + t * dx, ay + t * dy);
}

/** True if @p p is inside ring @p ring (even-odd). */
bool insideRing(GeoPoint p, const std::vector<GeoPoint>& ring)
{
  bool inside = false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const GeoPoint& a = ring[i];
    const GeoPoint& b = ring[j];
    if ((a.latDeg > p.latDeg) != (b.latDeg > p.latDeg) &&
        p.lonDeg < (b.lonDeg - a.lonDeg) * (p.latDeg - a.latDeg) /
                           (b.latDeg - a.latDeg) +
                       a.lonDeg) {
      inside = !inside;
    }
  }
  return inside;
}

/** Degrees of longitude to degrees of distance at @p p. */
double lonScaleAt(GeoPoint p)
{
  return std::max(std::cos(p.latDeg * pi / 180.0), 0.01);
}

bool nearPoints(const Feature& feature, GeoPoint p, double tolerance)
{
  const double lonScale = lonScaleAt(p);
  for (const std::vector<GeoPoint>& part : feature.parts) {
    for (const GeoPoint& point : part) {
      if (segmentDistance(p, point, point, lonScale) <= tolerance) {
        return true;
      }
    }
  }
  return false;
}

bool nearLines(const Feature& feature, GeoPoint p, double tolerance)
{
  const double lonScale = lonScaleAt(p);
  for (const std::vector<GeoPoint>& part : feature.parts) {
    for (std::size_t i = 0; i + 1 < part.size(); ++i) {
      if (segmentDistance(p, part[i], part[i + 1], lonScale) <= tolerance) {
        return true;
      }
    }
  }
  return false;
}

bool insidePolygon(const Feature& feature, GeoPoint p)
{
  if (feature.parts.empty() || feature.parts.front().size() < 3 ||
      !insideRing(p, feature.parts.front())) {
    return false;
  }
  return std::none_of(feature.parts.begin() + 1, feature.parts.end(),
                      [p](const std::vector<GeoPoint>& hole) {
                        return hole.size() >= 3 && insideRing(p, hole);
                      });
}

bool underPoint(const Feature& feature, GeoPoint p, double tolerance)
{
  switch (feature.type) {
  case Feature::Type::Points:
    return nearPoints(feature, p, tolerance);
  case Feature::Type::LineStrings:
    return nearLines(feature, p, tolerance);
  case Feature::Type::Polygon:
    return insidePolygon(feature, p);
  }
  return false;
}

} // namespace

bool featureUnder(const Feature& feature, GeoPoint point, double toleranceDeg)
{
  return underPoint(feature, point, toleranceDeg);
}

} // namespace ts
