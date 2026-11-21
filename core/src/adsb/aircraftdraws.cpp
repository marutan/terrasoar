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
#include "ts/adsb/aircraftdraws.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "ts/tracks/trackdraws.h"

namespace ts
{

namespace
{

constexpr std::array<std::uint8_t, 4> groundGrey = {150, 150, 150, 255};

std::array<std::uint8_t, 4> mix(const std::array<std::uint8_t, 4>& a,
                                const std::array<std::uint8_t, 4>& b, double f)
{
  std::array<std::uint8_t, 4> c{};
  for (std::size_t i = 0; i < 4; ++i) {
    c[i] = static_cast<std::uint8_t>(std::lround(
        static_cast<double>(a[i]) +
        (static_cast<double>(b[i]) - static_cast<double>(a[i])) * f));
  }
  return c;
}

} // namespace

const std::vector<AltitudeStop>& altitudeStops()
{
  static const std::vector<AltitudeStop> stops = {
      {0.0, {230, 80, 40, 255}},      {1500.0, {245, 160, 40, 255}},
      {3000.0, {210, 210, 50, 255}},  {6000.0, {70, 200, 110, 255}},
      {9000.0, {40, 160, 235, 255}},  {12000.0, {140, 100, 240, 255}},
      {15000.0, {220, 80, 210, 255}},
  };
  return stops;
}

std::array<std::uint8_t, 4> altitudeColour(double heightM)
{
  if (std::isnan(heightM)) {
    return groundGrey;
  }
  const std::vector<AltitudeStop>& stops = altitudeStops();
  if (heightM <= stops.front().heightM) {
    return stops.front().rgba;
  }
  for (std::size_t i = 1; i < stops.size(); ++i) {
    if (heightM <= stops[i].heightM) {
      const double f = (heightM - stops[i - 1].heightM) /
                       (stops[i].heightM - stops[i - 1].heightM);
      return mix(stops[i - 1].rgba, stops[i].rgba, f);
    }
  }
  return stops.back().rgba;
}

std::array<std::uint8_t, 4> verticalRateColour(double rateMS)
{
  if (std::isnan(rateMS)) {
    return groundGrey;
  }
  const std::array<std::uint8_t, 4> level = {200, 200, 200, 255};
  const double f = std::clamp(std::fabs(rateMS) / 10.0, 0.0, 1.0);
  return rateMS < 0.0 ? mix(level, {60, 120, 255, 255}, f)
                      : mix(level, {255, 140, 30, 255}, f);
}

std::array<std::uint8_t, 4> trailColour(const TrackSample& p,
                                        TrailColouring colouring,
                                        std::array<std::uint8_t, 4> rgba)
{
  switch (colouring) {
  case TrailColouring::Aircraft:
    break;
  case TrailColouring::Altitude:
    return altitudeColour(p.heightM);
  case TrailColouring::VerticalRate:
    return verticalRateColour(static_cast<double>(p.verticalRateMS));
  }
  return rgba;
}

GlobeMarker aircraftMarker(const TrackSample& at, double groundM,
                           std::array<std::uint8_t, 4> rgba, double radiusPx)
{
  GlobeMarker marker;
  marker.positionEcef = trackSampleEcef(at, groundM);
  marker.rgba = rgba;
  marker.radiusPx = radiusPx;
  marker.shape = MarkerShape::Aircraft;
  marker.headingDeg = static_cast<double>(at.trackDeg);
  return marker;
}

SpaceLine dropLine(const TrackSample& at, double groundM,
                   std::array<std::uint8_t, 4> rgba)
{
  SpaceLine line;
  rgba[3] = static_cast<std::uint8_t>(rgba[3] / 2);
  line.rgba = rgba;
  line.widthPx = 1.0F;
  line.globeOnly = true;
  SpaceLinePoint top;
  top.positionEcef = trackSampleEcef(at, groundM);
  top.latDeg = at.latDeg;
  top.lonDeg = at.lonDeg;
  SpaceLinePoint bottom = top;
  TrackSample ground = at;
  ground.heightM = groundM;
  bottom.positionEcef = trackSampleEcef(ground, groundM);
  line.points = {top, bottom};
  return line;
}

} // namespace ts
