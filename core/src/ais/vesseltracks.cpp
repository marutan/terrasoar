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
#include "ts/ais/vesseltracks.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ts
{

namespace
{

/** Knots in metres a second. */
constexpr double knotsToMetresPerSecond = 1852.0 / 3600.0;

/** A float, or NaN. */
float floatOr(std::optional<double> value)
{
  return value ? static_cast<float>(*value)
               : std::numeric_limits<float>::quiet_NaN();
}

/** @p info with what @p r says. */
VesselInfo withReport(VesselInfo info, const VesselReport& r)
{
  info.kind = r.kind;
  info.classB = r.classB.value_or(info.classB);
  info.name = r.name.value_or(info.name);
  info.callsign = r.callsign.value_or(info.callsign);
  info.imo = r.imo.value_or(info.imo);
  info.shipType = r.shipType.value_or(info.shipType);
  info.dimensions = r.dimensions.value_or(info.dimensions);
  info.draughtM = r.draughtM.value_or(info.draughtM);
  info.destination = r.destination.value_or(info.destination);
  if (r.eta) {
    info.eta = r.eta;
  }
  info.status = r.status.value_or(info.status);
  info.aidType = r.aidType.value_or(info.aidType);
  if (r.name || r.callsign || r.shipType || r.dimensions) {
    info.staticTimeS = r.timeS;
  }
  return info;
}

} // namespace

bool VesselReport::hasData() const
{
  return hasPosition() || speedKn || courseDeg || headingDeg || status ||
         name || callsign || imo || shipType || dimensions || draughtM ||
         destination || eta || aidType || altitudeM;
}

VesselReport vesselReport(const AisMessage& m, double timeS)
{
  VesselReport r;
  r.timeS = timeS;
  r.mmsi = m.mmsi;
  switch (m.type) {
  case 4:
    r.kind = AisStationKind::BaseStation;
    break;
  case 9:
    r.kind = AisStationKind::SarAircraft;
    break;
  case 21:
    r.kind = AisStationKind::AidToNavigation;
    break;
  default:
    r.kind = AisStationKind::Vessel;
    break;
  }
  if (m.type <= 3 || m.type == 5 || m.type == 27) {
    r.classB = false;
  } else if (m.isClassB()) {
    r.classB = true;
  }
  r.latDeg = m.latDeg;
  r.lonDeg = m.lonDeg;
  r.speedKn = m.speedKn;
  r.courseDeg = m.courseDeg;
  if (m.headingDeg) {
    r.headingDeg = static_cast<double>(*m.headingDeg);
  }
  r.turnDegMin = m.turnDegMin;
  if (m.altitudeM) {
    r.altitudeM = static_cast<double>(*m.altitudeM);
  }
  r.status = m.status;
  r.name = m.name;
  r.callsign = m.callsign;
  r.imo = m.imo;
  r.shipType = m.shipType;
  r.dimensions = m.dimensions;
  r.draughtM = m.draughtM;
  r.destination = m.destination;
  r.eta = m.eta;
  r.aidType = m.aidType;
  return r;
}

bool VesselInfo::sameAs(const VesselInfo& other) const
{
  const bool sameEta =
      eta.has_value() == other.eta.has_value() &&
      (!eta ||
       (eta->month == other.eta->month && eta->day == other.eta->day &&
        eta->hour == other.eta->hour && eta->minute == other.eta->minute));
  return kind == other.kind && classB == other.classB && name == other.name &&
         callsign == other.callsign && imo == other.imo &&
         shipType == other.shipType && dimensions == other.dimensions &&
         draughtM == other.draughtM && destination == other.destination &&
         sameEta && status == other.status && aidType == other.aidType;
}

const VesselInfo* VesselTrack::infoAt(double timeS) const
{
  if (info.empty()) {
    return nullptr;
  }
  const std::vector<VesselInfo>::const_iterator after = std::upper_bound(
      info.begin(), info.end(), timeS,
      [](double t, const VesselInfo& i) { return t < i.timeS; });
  return after == info.begin() ? &info.front() : &*(after - 1);
}

QString VesselTrack::lastName() const
{
  for (std::vector<VesselInfo>::const_reverse_iterator i = info.rbegin();
       i != info.rend(); ++i) {
    if (!i->name.isEmpty()) {
      return i->name;
    }
  }
  return QString();
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
double vesselStaleS(const VesselInfo& info, double speedKn, double minimumS)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  if (info.kind == AisStationKind::BaseStation ||
      info.kind == AisStationKind::AidToNavigation) {
    return std::max(3600.0, minimumS);
  }
  const double speed = std::isnan(speedKn) ? 0.0 : speedKn;
  double intervalS = 10.0;
  if (info.classB) {
    intervalS = speed < 2.0 ? 180.0 : 30.0;
  } else if (info.status == AisNavStatus::AtAnchor ||
             info.status == AisNavStatus::Moored || speed < 3.0) {
    intervalS = 180.0;
  } else if (speed > 23.0) {
    intervalS = 2.0;
  } else if (speed > 14.0) {
    intervalS = 6.0;
  }
  return std::max(10.0 * intervalS, minimumS);
}

void VesselTrackBuilder::add(const VesselReport& r)
{
  ++m_reports;
  Station& s = m_stations[r.mmsi];
  s.lastS = std::max(s.lastS, r.timeS);
  m_changed.insert(r.mmsi);
  VesselInfo info = withReport(s.current, r);
  if (s.info.empty() || !info.sameAs(s.current)) {
    info.timeS = r.timeS;
    s.info.push_back(info);
  }
  s.current = info;
  if (!r.hasPosition()) {
    return;
  }
  Point p;
  p.sample.timeS = r.timeS;
  p.sample.latDeg = r.latDeg.value_or(0.0);
  p.sample.lonDeg = r.lonDeg.value_or(0.0);
  if (r.altitudeM) {
    p.sample.heightM = *r.altitudeM;
  }
  if (r.speedKn) {
    p.sample.speedMS = static_cast<float>(*r.speedKn * knotsToMetresPerSecond);
  }
  p.sample.trackDeg = floatOr(r.courseDeg);
  p.motion.headingDeg = floatOr(r.headingDeg);
  p.motion.turnDegMin = floatOr(r.turnDegMin);
  s.points.push_back(p);
}

VesselTrack VesselTrackBuilder::build(uint32_t mmsi, Station station)
{
  std::vector<Point> points = std::move(station.points);
  std::stable_sort(points.begin(), points.end(),
                   [](const Point& a, const Point& b) {
                     return a.sample.timeS < b.sample.timeS;
                   });
  VesselTrack track;
  track.mmsi = mmsi;
  track.track.reserve(points.size());
  track.motion.reserve(points.size());
  for (const Point& p : points) {
    if (!track.track.isEmpty() &&
        std::fabs(p.sample.timeS - track.track.lastTimeS()) < 0.0005) {
      continue;
    }
    track.track.add(p.sample);
    track.motion.push_back(p.motion);
  }
  track.info = std::move(station.info);
  std::stable_sort(track.info.begin(), track.info.end(),
                   [](const VesselInfo& a, const VesselInfo& b) {
                     return a.timeS < b.timeS;
                   });
  return track;
}

std::vector<VesselTrack> VesselTrackBuilder::finish()
{
  std::vector<uint32_t> mmsis;
  mmsis.reserve(m_stations.size());
  for (const std::pair<const uint32_t, Station>& entry : m_stations) {
    mmsis.push_back(entry.first);
  }
  std::sort(mmsis.begin(), mmsis.end());
  std::vector<VesselTrack> tracks;
  tracks.reserve(mmsis.size());
  for (const uint32_t mmsi : mmsis) {
    tracks.push_back(build(mmsi, std::move(m_stations.at(mmsi))));
  }
  m_stations.clear();
  m_changed.clear();
  m_reports = 0;
  return tracks;
}

std::vector<VesselTrack> VesselTrackBuilder::buildChanged()
{
  std::vector<VesselTrack> tracks;
  tracks.reserve(m_changed.size());
  for (const uint32_t mmsi : m_changed) {
    const std::unordered_map<uint32_t, Station>::const_iterator found =
        m_stations.find(mmsi);
    if (found != m_stations.end()) {
      tracks.push_back(build(mmsi, found->second));
    }
  }
  m_changed.clear();
  return tracks;
}

void VesselTrackBuilder::forgetBefore(double timeS)
{
  for (std::unordered_map<uint32_t, Station>::iterator i = m_stations.begin();
       i != m_stations.end();) {
    if (i->second.lastS < timeS) {
      m_changed.erase(i->first);
      i = m_stations.erase(i);
    } else {
      ++i;
    }
  }
}

} // namespace ts
