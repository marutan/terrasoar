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

/**
 * @file
 * Tracks of moving objects (aircraft, later ships): reports in time
 * order, where an object is between them, and the trails behind it.
 */
#ifndef TS_TRACKS_MOVINGTRACK_H
#define TS_TRACKS_MOVINGTRACK_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <QJsonObject>

namespace ts
{

/**
 * One reported position of a moving object.
 *
 * @reentrant
 */
struct TrackSample {
  /** When, seconds since 1970-01-01T00:00Z (UTC). */
  double timeS = 0.0;
  /** Latitude, degrees north. */
  double latDeg = 0.0;
  /** Longitude, degrees east. */
  double lonDeg = 0.0;
  /** Height above the WGS 84 ellipsoid, metres; NaN if not known. */
  double heightM = std::numeric_limits<double>::quiet_NaN();
  /** Track over the ground, degrees from true north; NaN if not known. */
  float trackDeg = std::numeric_limits<float>::quiet_NaN();
  /** Ground speed, metres a second; NaN if not known. */
  float speedMS = std::numeric_limits<float>::quiet_NaN();
  /** Vertical rate, metres a second (up positive); NaN if not known. */
  float verticalRateMS = std::numeric_limits<float>::quiet_NaN();
  /** True if the height is approximate (from a barometric altitude). */
  bool heightApproximate = false;
};

/** How long reports stay good: when an object is shown, and gaps. */
struct TrackTiming {
  /**
   * An object with no report for this long is no longer shown (stale),
   * seconds.
   */
  double staleS = 60.0;
  /**
   * Reports farther apart than this are a gap in reception: not joined
   * (no line across, no position between), seconds.
   */
  double gapS = 30.0;
};

/** Which part of a track the trail behind an object shows. */
enum class TrailMode : uint8_t {
  Off,  ///< No trail.
  Span, ///< The reports within a time span before the clock's time.
  All,  ///< Every report up to the clock's time.
};

/** A trail's mode and span. */
struct TrailSettings {
  /** What it shows. */
  TrailMode mode = TrailMode::Span;
  /** The span before the clock's time (TrailMode::Span), seconds. */
  double spanS = 600.0;

  /** Returns the seconds shown before the clock's time (infinite: All). */
  double shownS() const;

  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form; nothing if it isn't one. */
  static std::optional<TrailSettings> fromJson(const QJsonObject& json);
  /** Equality. */
  bool operator==(const TrailSettings& other) const
  {
    return mode == other.mode && spanS == other.spanS;
  }
  /** Inequality. */
  bool operator!=(const TrailSettings& other) const
  {
    return !(*this == other);
  }
};

/** A run of reports without a gap: indices [first, last]. */
struct TrackPiece {
  /** The first report. */
  std::size_t first = 0;
  /** The last report (equal to first for a piece of one). */
  std::size_t last = 0;
};

/**
 * One object's reports in time order.
 *
 * @reentrant
 */
class MovingTrack
{
public:
  /** Returns the reports, in time order (after finish()). */
  const std::vector<TrackSample>& points() const { return m_points; }

  /** Adds a report (in any order; call finish() after the last). */
  void add(const TrackSample& point);

  /** Reserves room for @p count reports. */
  void reserve(std::size_t count) { m_points.reserve(count); }

  /**
   * Sorts the reports by time and removes duplicates: reports at the same
   * time (to a millisecond) as the one before, which keeps the first.
   * Returns how many were removed.
   */
  std::size_t finish();

  /** Returns true if there are no reports. */
  bool isEmpty() const { return m_points.empty(); }

  /** Returns the first report's time (0 if none). */
  double firstTimeS() const;
  /** Returns the last report's time (0 if none). */
  double lastTimeS() const;

  /**
   * Returns where the object is at @p timeS: between two reports,
   * interpolated (linearly, the shorter way round in longitude and in
   * track); after one, at it until @p timing's stale time or the next
   * report. Nothing before the first report, after the last by more than
   * the stale time, or within a gap after the stale time.
   */
  std::optional<TrackSample> at(double timeS, const TrackTiming& timing) const;

  /** Returns the index of the last report at or before @p timeS, if any. */
  std::optional<std::size_t> indexAtOrBefore(double timeS) const;

  /** Returns the runs of reports with no gap of @p timing's gapS. */
  std::vector<TrackPiece> pieces(const TrackTiming& timing) const;

  /**
   * Returns the trail at @p timeS as runs of points: the reports in
   * (timeS − span, timeS] of each piece, starting with the interpolated
   * point at the span's start and ending with the object's interpolated
   * position at @p timeS (if that piece is where it is now). What a
   * shader time filter on pieces() draws, worked out on the CPU.
   */
  std::vector<std::vector<TrackSample>> trail(double timeS,
                                              const TrailSettings& trail,
                                              const TrackTiming& timing) const;

private:
  std::vector<TrackSample> m_points;
  bool m_sorted = true;
};

/**
 * Returns the point a fraction @p f of the way from @p a to @p b (time,
 * position the shorter way round, height, track, speed).
 */
TrackSample interpolate(const TrackSample& a, const TrackSample& b, double f);

} // namespace ts

#endif // TS_TRACKS_MOVINGTRACK_H
