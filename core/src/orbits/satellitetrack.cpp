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
#include "ts/orbits/satellitetrack.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QJsonValue>

#include "ts/maths/angles.h"
#include "ts/orbits/earthframes.h"

namespace ts
{

namespace
{

/** The Earth's mean radius, metres (for footprints). */
constexpr double meanRadiusM = 6371008.8;

} // namespace

SatelliteState satelliteState(const Sgp4& sgp4, JulianDate time)
{
  SatelliteState state;
  const Sgp4Result result = sgp4.propagate(time);
  state.error = result.error;
  if (!result.ok()) {
    return state;
  }
  const EcefState ecef = temeToEcef(result.teme, greenwichSiderealRad(time));
  state.positionEcefM = ecef.positionM;
  state.velocityEcefMS = ecef.velocityMS;
  state.geodetic = ecefToGeodetic(ecef.positionM);
  state.speedKmS = length(result.teme.velocityKmS);
  return state;
}

double TimeSpan::minutes(double periodMinutes) const
{
  const double value = std::max(amount, 0.0);
  return unit == SpanUnit::Orbits ? value * periodMinutes : value;
}

QJsonObject TimeSpan::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("amount"), amount);
  json.insert(QStringLiteral("unit"), unit == SpanUnit::Orbits
                                          ? QStringLiteral("orbits")
                                          : QStringLiteral("minutes"));
  return json;
}

std::optional<TimeSpan> TimeSpan::fromJson(const QJsonObject& json)
{
  const QJsonValue amount = json.value(QLatin1String("amount"));
  const QString unit = json.value(QLatin1String("unit")).toString();
  if (!amount.isDouble() || amount.toDouble() < 0.0 ||
      (unit != QLatin1String("orbits") && unit != QLatin1String("minutes"))) {
    return std::nullopt;
  }
  TimeSpan span;
  span.amount = amount.toDouble();
  span.unit =
      unit == QLatin1String("orbits") ? SpanUnit::Orbits : SpanUnit::Minutes;
  return span;
}

double trackStepMinutes(double periodMinutes)
{
  return std::clamp(periodMinutes / 120.0, 0.25, 30.0);
}

TrackSampler::TrackSampler(std::shared_ptr<const Sgp4> sgp4)
    : m_sgp4(std::move(sgp4)),
      m_stepMinutes(trackStepMinutes(m_sgp4->periodMinutes()))
{
}

std::optional<TrackPoint> TrackSampler::pointAt(double minutesSinceEpoch) const
{
  const Sgp4Result result = m_sgp4->propagate(minutesSinceEpoch);
  if (!result.ok()) {
    return std::nullopt;
  }
  JulianDate time = m_sgp4->elements().epoch;
  time.fraction += minutesSinceEpoch / 1440.0;
  const EcefState ecef = temeToEcef(result.teme, greenwichSiderealRad(time));
  TrackPoint point;
  point.minutes = minutesSinceEpoch;
  point.positionEcefM = ecef.positionM;
  point.geodetic = ecefToGeodetic(ecef.positionM);
  return point;
}

SatelliteTrack TrackSampler::track(JulianDate time, double pastMinutes,
                                   double futureMinutes)
{
  const double now = time.minutesSince(m_sgp4->elements().epoch);
  const double start = now - std::max(pastMinutes, 0.0);
  const double end = now + std::max(futureMinutes, 0.0);
  // The grid points strictly inside the spans.
  const int64_t first =
      static_cast<int64_t>(std::floor(start / m_stepMinutes)) + 1;
  const int64_t last = static_cast<int64_t>(std::ceil(end / m_stepMinutes)) - 1;
  m_grid.erase(m_grid.begin(), m_grid.lower_bound(first));
  m_grid.erase(m_grid.upper_bound(last), m_grid.end());
  for (int64_t k = first; k <= last; ++k) {
    const std::map<int64_t, std::optional<TrackPoint>>::iterator at =
        m_grid.lower_bound(k);
    if (at == m_grid.end() || at->first != k) {
      m_grid.emplace_hint(at, k,
                          pointAt(static_cast<double>(k) * m_stepMinutes));
      ++m_computed;
    }
  }
  SatelliteTrack track;
  // The time's point, if SGP4 gives one there.
  const std::optional<TrackPoint> atNow = pointAt(now);
  const bool hasCurrent = atNow.has_value();
  const TrackPoint current = atNow.value_or(TrackPoint());
  std::vector<TrackPoint>* part = &track.past;
  const std::optional<TrackPoint> head = pointAt(start);
  if (head && start < now) {
    track.past.push_back(*head);
  }
  for (const std::pair<const int64_t, std::optional<TrackPoint>>& entry :
       m_grid) {
    if (!entry.second) {
      continue;
    }
    if (part == &track.past && entry.second->minutes >= now) {
      if (hasCurrent) {
        track.past.push_back(current);
        track.future.push_back(current);
      }
      part = &track.future;
    }
    if (entry.second->minutes != now) {
      part->push_back(*entry.second);
    }
  }
  if (part == &track.past && hasCurrent) {
    track.past.push_back(current);
    track.future.push_back(current);
  }
  const std::optional<TrackPoint> tail = pointAt(end);
  if (tail && end > now) {
    track.future.push_back(*tail);
  }
  // Times relative to the time asked.
  for (std::vector<TrackPoint>* points : {&track.past, &track.future}) {
    for (TrackPoint& point : *points) {
      point.minutes -= now;
    }
  }
  return track;
}

std::vector<std::vector<LonLat>>
splitAtAntimeridian(const std::vector<LonLat>& points)
{
  std::vector<std::vector<LonLat>> pieces;
  if (points.empty()) {
    return pieces;
  }
  pieces.emplace_back();
  pieces.back().push_back(points.front());
  for (std::size_t i = 1; i < points.size(); ++i) {
    const LonLat& a = points[i - 1];
    const LonLat& b = points[i];
    const double step = b.lonDeg - a.lonDeg;
    if (std::fabs(step) > 180.0) {
      // Crossing eastwards (b far west) or westwards.
      const double edge = step < 0.0 ? 180.0 : -180.0;
      const double unwrapped = b.lonDeg + (step < 0.0 ? 360.0 : -360.0);
      const double f = (edge - a.lonDeg) / (unwrapped - a.lonDeg);
      const double latDeg = a.latDeg + f * (b.latDeg - a.latDeg);
      pieces.back().push_back(LonLat{edge, latDeg});
      pieces.emplace_back();
      pieces.back().push_back(LonLat{-edge, latDeg});
    }
    pieces.back().push_back(b);
  }
  return pieces;
}

// A height, then an angle.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double footprintRadiusRad(double heightM, double minElevationDeg)
{
  const double elevation = degToRad(std::clamp(minElevationDeg, 0.0, 89.9));
  const double ratio = meanRadiusM / (meanRadiusM + std::max(heightM, 0.0));
  return std::acos(std::clamp(ratio * std::cos(elevation), -1.0, 1.0)) -
         elevation;
}

// An angle, then a count.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::vector<LonLat> footprint(const Geodetic& subSatellite,
                              double minElevationDeg, int32_t points)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  std::vector<LonLat> ring;
  const double radius =
      footprintRadiusRad(subSatellite.heightM, minElevationDeg);
  if (radius <= 0.0 || points < 3) {
    return ring;
  }
  const double lat = degToRad(subSatellite.latDeg);
  const double lon = degToRad(subSatellite.lonDeg);
  ring.reserve(static_cast<std::size_t>(points));
  for (int32_t i = 0; i < points; ++i) {
    const double bearing = 2.0 * pi * i / points;
    const double lat2 =
        std::asin(std::sin(lat) * std::cos(radius) +
                  std::cos(lat) * std::sin(radius) * std::cos(bearing));
    const double lon2 =
        lon + std::atan2(std::sin(bearing) * std::sin(radius) * std::cos(lat),
                         std::cos(radius) - std::sin(lat) * std::sin(lat2));
    ring.push_back(LonLat{normaliseLonDeg(radToDeg(lon2)), radToDeg(lat2)});
  }
  return ring;
}

} // namespace ts
