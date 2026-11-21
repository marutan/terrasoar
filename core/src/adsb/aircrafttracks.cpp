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
#include "ts/adsb/aircrafttracks.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ts
{

namespace
{

/** The radius of the ISA's geopotential, metres. */
constexpr double isaRadiusM = 6356766.0;

/** Feet a minute in metres a second. */
constexpr double feetPerMinuteToMetresPerSecond = feetToMetres / 60.0;

} // namespace

double pressureAltitudeToHeightM(double pressureAltitudeFt)
{
  const double geopotentialM = pressureAltitudeFt * feetToMetres;
  return isaRadiusM * geopotentialM / (isaRadiusM - geopotentialM);
}

bool AircraftInfo::sameAs(const AircraftInfo& other) const
{
  return callsign == other.callsign && squawk == other.squawk &&
         registration == other.registration &&
         aircraftType == other.aircraftType && category == other.category &&
         emergency == other.emergency && onGround == other.onGround;
}

const AircraftInfo* AircraftTrack::infoAt(double timeS) const
{
  if (info.empty()) {
    return nullptr;
  }
  const std::vector<AircraftInfo>::const_iterator after = std::upper_bound(
      info.begin(), info.end(), timeS,
      [](double t, const AircraftInfo& i) { return t < i.timeS; });
  return after == info.begin() ? &info.front() : &*(after - 1);
}

QString AircraftTrack::lastCallsign() const
{
  for (std::vector<AircraftInfo>::const_reverse_iterator i = info.rbegin();
       i != info.rend(); ++i) {
    if (!i->callsign.isEmpty()) {
      return i->callsign;
    }
  }
  return QString();
}

AircraftTrackBuilder::AircraftTrackBuilder(AircraftHeightSettings settings)
    : m_settings(std::move(settings))
{
}

void AircraftTrackBuilder::add(const AircraftReport& r)
{
  ++m_reports;
  Aircraft& a = m_aircraft[r.address];
  const double t = r.timeS;
  if (r.baroAltitudeFt) {
    a.baroFt = r.baroAltitudeFt;
    a.baroTimeS = t;
  }
  if (r.geometricAltitudeFt) {
    a.geometricFt = r.geometricAltitudeFt;
    a.geometricTimeS = t;
  }
  if (r.groundSpeedKt || r.trackDeg || r.verticalRateFtMin) {
    if (r.groundSpeedKt) {
      a.speedKt = r.groundSpeedKt;
    }
    if (r.trackDeg) {
      a.trackDeg = r.trackDeg;
    }
    if (r.verticalRateFtMin) {
      a.verticalRateFtMin = r.verticalRateFtMin;
    }
    a.motionTimeS = t;
  }
  AircraftInfo info = a.current;
  if (r.callsign && !r.callsign->isEmpty()) {
    info.callsign = *r.callsign;
  }
  if (r.squawk) {
    info.squawk = *r.squawk;
  }
  if (r.registration) {
    info.registration = *r.registration;
  }
  if (r.aircraftType) {
    info.aircraftType = *r.aircraftType;
  }
  if (r.category) {
    info.category = *r.category;
  }
  if (r.emergency) {
    info.emergency = *r.emergency;
  }
  if (r.onGround) {
    info.onGround = r.onGround;
  }
  if (a.info.empty() || !info.sameAs(a.current)) {
    info.timeS = t;
    a.info.push_back(info);
    a.current = info;
  }
  if (!r.position) {
    return;
  }
  ++m_positions;
  Point p;
  p.point.timeS = t;
  p.point.latDeg = r.position->latDeg;
  p.point.lonDeg = r.position->lonDeg;
  const double kept = m_settings.altitudeKeptS;
  const bool baro = a.baroFt && std::fabs(t - a.baroTimeS) <= kept;
  const bool geometric =
      a.geometricFt && std::fabs(t - a.geometricTimeS) <= kept;
  if (baro) {
    p.altitudes.baroFt = static_cast<float>(*a.baroFt);
  }
  if (geometric) {
    p.altitudes.geometricFt = static_cast<float>(*a.geometricFt);
    p.point.heightM = *a.geometricFt * feetToMetres;
  } else if (baro) {
    double undulation =
        m_settings.undulation
            ? m_settings.undulation(p.point.latDeg, p.point.lonDeg)
            : 0.0;
    if (std::isnan(undulation)) {
      undulation = 0.0;
    }
    p.point.heightM = pressureAltitudeToHeightM(*a.baroFt) + undulation;
    p.point.heightApproximate = true;
  }
  if (std::fabs(t - a.motionTimeS) <= kept) {
    if (a.speedKt) {
      p.point.speedMS = static_cast<float>(*a.speedKt * knotsToMetresPerSecond);
    }
    if (a.trackDeg) {
      p.point.trackDeg = static_cast<float>(*a.trackDeg);
    }
    if (a.verticalRateFtMin) {
      p.point.verticalRateMS = static_cast<float>(
          *a.verticalRateFtMin * feetPerMinuteToMetresPerSecond);
    }
  }
  a.points.push_back(p);
}

AircraftTrack AircraftTrackBuilder::build(uint32_t address, Aircraft&& aircraft)
{
  std::vector<Point> points = std::move(aircraft.points);
  std::stable_sort(points.begin(), points.end(),
                   [](const Point& a, const Point& b) {
                     return a.point.timeS < b.point.timeS;
                   });
  AircraftTrack track;
  track.address = address;
  track.track.reserve(points.size());
  track.altitudes.reserve(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    // Duplicates: the same time to a millisecond as the one kept before.
    if (i > 0 && !track.track.isEmpty() &&
        std::fabs(points[i].point.timeS - track.track.lastTimeS()) < 0.0005) {
      continue;
    }
    track.track.add(points[i].point);
    track.altitudes.push_back(points[i].altitudes);
  }
  track.info = std::move(aircraft.info);
  std::stable_sort(track.info.begin(), track.info.end(),
                   [](const AircraftInfo& a, const AircraftInfo& b) {
                     return a.timeS < b.timeS;
                   });
  return track;
}

std::vector<AircraftTrack> AircraftTrackBuilder::finish()
{
  std::vector<uint32_t> addresses;
  addresses.reserve(m_aircraft.size());
  for (const std::pair<const uint32_t, Aircraft>& entry : m_aircraft) {
    addresses.push_back(entry.first);
  }
  std::sort(addresses.begin(), addresses.end());
  std::vector<AircraftTrack> tracks;
  tracks.reserve(addresses.size());
  for (const uint32_t address : addresses) {
    tracks.push_back(build(address, std::move(m_aircraft.at(address))));
    m_aircraft.erase(address);
  }
  m_reports = 0;
  m_positions = 0;
  return tracks;
}

} // namespace ts
