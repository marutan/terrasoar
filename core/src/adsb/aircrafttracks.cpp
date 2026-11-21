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

namespace
{

/** @p info with what @p r says of the aircraft. */
AircraftInfo withReport(AircraftInfo info, const AircraftReport& r)
{
  if (r.callsign && !r.callsign->isEmpty()) {
    info.callsign = *r.callsign;
  }
  info.squawk = r.squawk.value_or(info.squawk);
  info.registration = r.registration.value_or(info.registration);
  info.aircraftType = r.aircraftType.value_or(info.aircraftType);
  info.category = r.category.value_or(info.category);
  info.emergency = r.emergency.value_or(info.emergency);
  if (r.onGround) {
    info.onGround = r.onGround;
  }
  return info;
}

} // namespace

void AircraftTrackBuilder::add(const AircraftReport& r)
{
  ++m_reports;
  Aircraft& a = m_aircraft[r.address];
  const double t = r.timeS;
  a.lastS = std::max(a.lastS, t);
  m_changed.insert(r.address);
  if (r.baroAltitudeFt) {
    a.baroFt = r.baroAltitudeFt;
    a.baroTimeS = t;
  }
  if (r.geometricAltitudeFt) {
    a.geometricFt = r.geometricAltitudeFt;
    a.geometricTimeS = t;
  }
  if (r.groundSpeedKt || r.trackDeg || r.verticalRateFtMin) {
    a.speedKt = r.groundSpeedKt ? r.groundSpeedKt : a.speedKt;
    a.trackDeg = r.trackDeg ? r.trackDeg : a.trackDeg;
    a.verticalRateFtMin =
        r.verticalRateFtMin ? r.verticalRateFtMin : a.verticalRateFtMin;
    a.motionTimeS = t;
  }
  AircraftInfo info = withReport(a.current, r);
  if (a.info.empty() || !info.sameAs(a.current)) {
    info.timeS = t;
    a.info.push_back(info);
    a.current = info;
  }
  if (r.position) {
    ++m_positions;
    a.points.push_back(pointOf(a, t, *r.position));
  }
}

AircraftTrackBuilder::Point
AircraftTrackBuilder::pointOf(const Aircraft& a, double t,
                              CprPosition position) const
{
  Point p;
  p.point.timeS = t;
  p.point.latDeg = position.latDeg;
  p.point.lonDeg = position.lonDeg;
  const double kept = m_settings.altitudeKeptS;
  const std::optional<double> baro =
      std::fabs(t - a.baroTimeS) <= kept ? a.baroFt : std::nullopt;
  const std::optional<double> geometric =
      std::fabs(t - a.geometricTimeS) <= kept ? a.geometricFt : std::nullopt;
  if (baro) {
    p.altitudes.baroFt = static_cast<float>(*baro);
  }
  if (geometric) {
    p.altitudes.geometricFt = static_cast<float>(*geometric);
    p.point.heightM = *geometric * feetToMetres;
  } else if (baro) {
    double undulation =
        m_settings.undulation
            ? m_settings.undulation(p.point.latDeg, p.point.lonDeg)
            : 0.0;
    if (std::isnan(undulation)) {
      undulation = 0.0;
    }
    p.point.heightM = pressureAltitudeToHeightM(*baro) + undulation;
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
  return p;
}

AircraftTrack AircraftTrackBuilder::build(uint32_t address, Aircraft aircraft)
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
  m_changed.clear();
  return tracks;
}

std::vector<AircraftTrack> AircraftTrackBuilder::buildChanged()
{
  std::vector<AircraftTrack> tracks;
  tracks.reserve(m_changed.size());
  for (const uint32_t address : m_changed) {
    const std::unordered_map<uint32_t, Aircraft>::const_iterator found =
        m_aircraft.find(address);
    if (found != m_aircraft.end()) {
      tracks.push_back(build(address, found->second));
    }
  }
  m_changed.clear();
  return tracks;
}

void AircraftTrackBuilder::forgetBefore(double timeS)
{
  for (std::unordered_map<uint32_t, Aircraft>::iterator i = m_aircraft.begin();
       i != m_aircraft.end();) {
    if (i->second.lastS < timeS) {
      m_changed.erase(i->first);
      i = m_aircraft.erase(i);
    } else {
      ++i;
    }
  }
}

} // namespace ts
