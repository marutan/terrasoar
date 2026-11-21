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
#include "ts/tracks/movingtrack.h"

#include <algorithm>
#include <cmath>

#include <QJsonValue>

#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

/** The angle from @p a to @p b the shorter way, degrees in (−180, 180]. */
double turn(double a, double b)
{
  double d = std::fmod(b - a, 360.0);
  if (d > 180.0) {
    d -= 360.0;
  } else if (d <= -180.0) {
    d += 360.0;
  }
  return d;
}

float mixFloat(float a, float b, double f)
{
  if (std::isnan(a)) {
    return b;
  }
  if (std::isnan(b)) {
    return a;
  }
  return static_cast<float>(static_cast<double>(a) +
                            (static_cast<double>(b) - static_cast<double>(a)) *
                                f);
}

} // namespace

double TrailSettings::shownS() const
{
  switch (mode) {
  case TrailMode::Off:
    return 0.0;
  case TrailMode::Span:
    return std::max(spanS, 0.0);
  case TrailMode::All:
    break;
  }
  return std::numeric_limits<double>::infinity();
}

QJsonObject TrailSettings::toJson() const
{
  QJsonObject json;
  QString id = QStringLiteral("all");
  if (mode == TrailMode::Off) {
    id = QStringLiteral("off");
  } else if (mode == TrailMode::Span) {
    id = QStringLiteral("span");
  }
  json.insert(QStringLiteral("mode"), id);
  json.insert(QStringLiteral("spanS"), spanS);
  return json;
}

std::optional<TrailSettings> TrailSettings::fromJson(const QJsonObject& json)
{
  const QString mode = json.value(QLatin1String("mode")).toString();
  TrailSettings settings;
  if (mode == QLatin1String("off")) {
    settings.mode = TrailMode::Off;
  } else if (mode == QLatin1String("span")) {
    settings.mode = TrailMode::Span;
  } else if (mode == QLatin1String("all")) {
    settings.mode = TrailMode::All;
  } else {
    return std::nullopt;
  }
  const QJsonValue span = json.value(QLatin1String("spanS"));
  if (span.isDouble() && span.toDouble() >= 0.0) {
    settings.spanS = span.toDouble();
  }
  return settings;
}

TrackSample interpolate(const TrackSample& a, const TrackSample& b, double f)
{
  TrackSample p = a;
  p.timeS = a.timeS + (b.timeS - a.timeS) * f;
  p.latDeg = a.latDeg + (b.latDeg - a.latDeg) * f;
  p.lonDeg = normaliseLonDeg(a.lonDeg + turn(a.lonDeg, b.lonDeg) * f);
  if (std::isnan(a.heightM)) {
    p.heightM = b.heightM;
  } else if (!std::isnan(b.heightM)) {
    p.heightM = a.heightM + (b.heightM - a.heightM) * f;
  }
  p.heightApproximate = a.heightApproximate || b.heightApproximate;
  if (std::isnan(a.trackDeg)) {
    p.trackDeg = b.trackDeg;
  } else if (!std::isnan(b.trackDeg)) {
    double track =
        static_cast<double>(a.trackDeg) +
        turn(static_cast<double>(a.trackDeg), static_cast<double>(b.trackDeg)) *
            f;
    if (track < 0.0) {
      track += 360.0;
    } else if (track >= 360.0) {
      track -= 360.0;
    }
    p.trackDeg = static_cast<float>(track);
  }
  p.speedMS = mixFloat(a.speedMS, b.speedMS, f);
  p.verticalRateMS = mixFloat(a.verticalRateMS, b.verticalRateMS, f);
  return p;
}

void MovingTrack::add(const TrackSample& point)
{
  if (!m_points.empty() && point.timeS < m_points.back().timeS) {
    m_sorted = false;
  }
  m_points.push_back(point);
}

std::size_t MovingTrack::finish()
{
  if (!m_sorted) {
    std::stable_sort(m_points.begin(), m_points.end(),
                     [](const TrackSample& a, const TrackSample& b) {
                       return a.timeS < b.timeS;
                     });
    m_sorted = true;
  }
  const std::size_t before = m_points.size();
  const std::vector<TrackSample>::iterator end =
      std::unique(m_points.begin(), m_points.end(),
                  [](const TrackSample& a, const TrackSample& b) {
                    return std::fabs(a.timeS - b.timeS) < 0.0005;
                  });
  m_points.erase(end, m_points.end());
  return before - m_points.size();
}

double MovingTrack::firstTimeS() const
{
  return m_points.empty() ? 0.0 : m_points.front().timeS;
}

double MovingTrack::lastTimeS() const
{
  return m_points.empty() ? 0.0 : m_points.back().timeS;
}

std::optional<std::size_t> MovingTrack::indexAtOrBefore(double timeS) const
{
  const std::vector<TrackSample>::const_iterator after = std::upper_bound(
      m_points.begin(), m_points.end(), timeS,
      [](double t, const TrackSample& p) { return t < p.timeS; });
  if (after == m_points.begin()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(after - m_points.begin()) - 1;
}

std::optional<TrackSample> MovingTrack::at(double timeS,
                                           TrackTiming timing) const
{
  const std::optional<std::size_t> index = indexAtOrBefore(timeS);
  if (!index) {
    return std::nullopt;
  }
  const TrackSample& a = m_points[*index];
  if (*index + 1 < m_points.size()) {
    const TrackSample& b = m_points[*index + 1];
    const double dt = b.timeS - a.timeS;
    if (dt <= timing.gapS && dt > 0.0) {
      return interpolate(a, b, (timeS - a.timeS) / dt);
    }
  }
  if (timeS - a.timeS > timing.staleS) {
    return std::nullopt;
  }
  TrackSample point = a;
  point.timeS = timeS;
  return point;
}

std::vector<TrackPiece> MovingTrack::pieces(TrackTiming timing) const
{
  std::vector<TrackPiece> result;
  if (m_points.empty()) {
    return result;
  }
  TrackPiece piece;
  for (std::size_t i = 1; i < m_points.size(); ++i) {
    if (m_points[i].timeS - m_points[i - 1].timeS > timing.gapS) {
      piece.last = i - 1;
      result.push_back(piece);
      piece.first = i;
    }
  }
  piece.last = m_points.size() - 1;
  result.push_back(piece);
  return result;
}

namespace
{

/**
 * Returns the part of @p piece of @p points within [@p from, @p timeS]:
 * its reports there, with the points interpolated at the ends.
 */
// Two times, as named.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::vector<TrackSample> trailRun(const std::vector<TrackSample>& points,
                                  TrackPiece piece, double from, double timeS)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  std::vector<TrackSample> run;
  for (std::size_t i = piece.first; i <= piece.last; ++i) {
    const TrackSample& p = points.at(i);
    if (p.timeS < from) {
      continue;
    }
    const bool after = p.timeS > timeS;
    if (i > piece.first && (after || (run.empty() && p.timeS > from))) {
      // Between the report before and this one: where the span starts
      // (when nothing is in the run yet), and the head at the time.
      const TrackSample& a = points.at(i - 1);
      const double dt = p.timeS - a.timeS;
      if (run.empty()) {
        run.push_back(interpolate(a, p, (from - a.timeS) / dt));
      }
      if (after) {
        run.push_back(interpolate(a, p, (timeS - a.timeS) / dt));
      }
    }
    if (after) {
      break;
    }
    run.push_back(p);
  }
  return run;
}

} // namespace

std::vector<std::vector<TrackSample>>
MovingTrack::trail(double timeS, TrailSettings trail, TrackTiming timing) const
{
  std::vector<std::vector<TrackSample>> runs;
  if (trail.mode == TrailMode::Off) {
    return runs;
  }
  const double from = timeS - trail.shownS();
  for (const TrackPiece& piece : pieces(timing)) {
    const TrackSample& first = m_points.at(piece.first);
    const TrackSample& last = m_points.at(piece.last);
    if (first.timeS > timeS || last.timeS < from || piece.first == piece.last) {
      continue;
    }
    std::vector<TrackSample> run = trailRun(m_points, piece, from, timeS);
    if (run.size() >= 2) {
      runs.push_back(std::move(run));
    }
  }
  return runs;
}

} // namespace ts
