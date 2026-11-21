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
#include "ts/ais/vesseldraws.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"
#include "ts/tracks/trackdraws.h"

namespace ts
{

namespace
{

/** The Earth's mean radius, metres. */
constexpr double earthRadiusM = 6371008.8;

std::array<std::uint8_t, 4> mix(std::array<std::uint8_t, 4> a,
                                std::array<std::uint8_t, 4> b, double f)
{
  std::array<std::uint8_t, 4> c{};
  for (std::size_t i = 0; i < 4; ++i) {
    c.at(i) = static_cast<std::uint8_t>(std::lround(
        static_cast<double>(a.at(i)) +
        (static_cast<double>(b.at(i)) - static_cast<double>(a.at(i))) * f));
  }
  return c;
}

} // namespace

ShipTypeGroup shipTypeGroup(int32_t shipType)
{
  const int32_t tens = shipType / 10;
  switch (tens) {
  case 4:
    return ShipTypeGroup::HighSpeed;
  case 5:
    return ShipTypeGroup::TugSpecial;
  case 6:
    return ShipTypeGroup::Passenger;
  case 7:
    return ShipTypeGroup::Cargo;
  case 8:
    return ShipTypeGroup::Tanker;
  default:
    break;
  }
  switch (shipType) {
  case 0:
    return ShipTypeGroup::Unknown;
  case 30:
    return ShipTypeGroup::Fishing;
  case 31:
  case 32:
    return ShipTypeGroup::TugSpecial;
  case 36:
  case 37:
    return ShipTypeGroup::Pleasure;
  default:
    break;
  }
  return ShipTypeGroup::Other;
}

std::array<std::uint8_t, 4> shipTypeColour(ShipTypeGroup group)
{
  switch (group) {
  case ShipTypeGroup::Unknown:
    break;
  case ShipTypeGroup::Cargo:
    return {80, 190, 80, 255};
  case ShipTypeGroup::Tanker:
    return {225, 60, 60, 255};
  case ShipTypeGroup::Passenger:
    return {70, 120, 240, 255};
  case ShipTypeGroup::HighSpeed:
    return {245, 220, 40, 255};
  case ShipTypeGroup::TugSpecial:
    return {60, 205, 225, 255};
  case ShipTypeGroup::Fishing:
    return {245, 150, 60, 255};
  case ShipTypeGroup::Pleasure:
    return {215, 85, 205, 255};
  case ShipTypeGroup::Other:
    return {175, 175, 175, 255};
  case ShipTypeGroup::Navigation:
    return {235, 235, 235, 255};
  }
  return {125, 125, 125, 255};
}

const std::vector<ShipTypeGroup>& shipTypeLegend()
{
  static const std::vector<ShipTypeGroup> legend = {
      ShipTypeGroup::Cargo,      ShipTypeGroup::Tanker,
      ShipTypeGroup::Passenger,  ShipTypeGroup::HighSpeed,
      ShipTypeGroup::TugSpecial, ShipTypeGroup::Fishing,
      ShipTypeGroup::Pleasure,   ShipTypeGroup::Other,
      ShipTypeGroup::Unknown,    ShipTypeGroup::Navigation,
  };
  return legend;
}

std::array<std::uint8_t, 4> speedColour(double speedMS)
{
  if (std::isnan(speedMS)) {
    return {150, 150, 150, 255};
  }
  const double knots = speedMS / (1852.0 / 3600.0);
  const std::array<std::array<std::uint8_t, 4>, 5> stops = {{
      {150, 150, 150, 255},
      {60, 110, 240, 255},
      {70, 200, 90, 255},
      {245, 215, 40, 255},
      {230, 55, 45, 255},
  }};
  // Grey at rest, blue at 2 kn, green at 8, yellow at 15, red at 25.
  const std::array<double, 5> at = {0.0, 2.0, 8.0, 15.0, 25.0};
  if (knots >= at.back()) {
    return stops.back();
  }
  for (std::size_t i = 1; i < at.size(); ++i) {
    if (knots <= at.at(i)) {
      return mix(stops.at(i - 1), stops.at(i),
                 (std::max(knots, 0.0) - at.at(i - 1)) /
                     (at.at(i) - at.at(i - 1)));
    }
  }
  return stops.back();
}

double vesselDirectionDeg(const TrackSample& at, VesselMotion motion)
{
  if (!std::isnan(motion.headingDeg)) {
    return static_cast<double>(motion.headingDeg);
  }
  return static_cast<double>(at.trackDeg);
}

GlobeMarker vesselMarker(const TrackSample& at, VesselMotion motion,
                         const VesselInfo& info, double seaLevelM,
                         std::array<std::uint8_t, 4> rgba, double radiusPx)
{
  GlobeMarker marker;
  marker.positionEcef = trackSampleEcef(at, seaLevelM);
  marker.rgba = rgba;
  marker.radiusPx = radiusPx;
  marker.headingDeg = vesselDirectionDeg(at, motion);
  switch (info.kind) {
  case AisStationKind::SarAircraft:
    marker.shape = MarkerShape::Aircraft;
    return marker;
  case AisStationKind::AidToNavigation:
    marker.shape = MarkerShape::Diamond;
    marker.headingDeg = std::numeric_limits<double>::quiet_NaN();
    return marker;
  case AisStationKind::BaseStation:
    marker.shape = MarkerShape::Square;
    marker.headingDeg = std::numeric_limits<double>::quiet_NaN();
    return marker;
  case AisStationKind::Vessel:
    break;
  }
  const bool stopped = info.status == AisNavStatus::Moored ||
                       info.status == AisNavStatus::AtAnchor ||
                       info.status == AisNavStatus::Aground;
  const bool pointing = std::isfinite(marker.headingDeg);
  marker.shape =
      stopped || !pointing ? MarkerShape::ShipStopped : MarkerShape::Ship;
  const AisDimensions& d = info.dimensions;
  if (d.isKnown() && pointing) {
    marker.hullLengthM = d.toBow + d.toStern;
    marker.hullWidthM = d.toPort + d.toStarboard;
    marker.hullForwardM = 0.5 * (d.toBow - d.toStern);
    marker.hullStarboardM = 0.5 * (d.toStarboard - d.toPort);
  }
  return marker;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
SpaceLine courseVector(const TrackSample& at, double minutes, double seaLevelM,
                       std::array<std::uint8_t, 4> rgba)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  SpaceLine line;
  line.rgba = rgba;
  line.widthPx = 1.5F;
  const double speed = static_cast<double>(at.speedMS);
  const double course = static_cast<double>(at.trackDeg);
  if (std::isnan(speed) || std::isnan(course) || speed <= 0.05) {
    return line;
  }
  // Along the great circle from the point, the distance at the speed.
  const double d = speed * minutes * 60.0 / earthRadiusM;
  const double lat = degToRad(at.latDeg);
  const double lon = degToRad(at.lonDeg);
  const double c = degToRad(course);
  const double lat2 = std::asin(std::sin(lat) * std::cos(d) +
                                std::cos(lat) * std::sin(d) * std::cos(c));
  const double lon2 =
      lon + std::atan2(std::sin(c) * std::sin(d) * std::cos(lat),
                       std::cos(d) - std::sin(lat) * std::sin(lat2));
  TrackSample end = at;
  end.latDeg = radToDeg(lat2);
  end.lonDeg = normaliseLonDeg(radToDeg(lon2));
  for (const TrackSample& p : {at, end}) {
    SpaceLinePoint point;
    point.positionEcef = trackSampleEcef(p, seaLevelM);
    point.latDeg = p.latDeg;
    point.lonDeg = p.lonDeg;
    line.points.push_back(point);
  }
  return line;
}

} // namespace ts
