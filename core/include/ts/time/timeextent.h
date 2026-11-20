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
 * The times a layer has data for, as web map services write them: lists
 * of instants and start/end/period intervals in ISO 8601.
 */
#ifndef TS_TIME_TIMEEXTENT_H
#define TS_TIME_TIMEEXTENT_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace ts
{

/**
 * An ISO 8601 duration (`P1D`, `PT6H`, `P1M`, `P1Y2M10DT2H30M`): calendar
 * years and months, days and seconds kept apart, as months have no fixed
 * length.
 *
 * @reentrant
 */
struct TimePeriod {
  /** Whole years. */
  int32_t years = 0;
  /** Whole months. */
  int32_t months = 0;
  /** Whole days. */
  int32_t days = 0;
  /** Seconds. */
  int64_t seconds = 0;

  /** Returns true if it is zero (no step). */
  bool isZero() const
  {
    return years == 0 && months == 0 && days == 0 && seconds == 0;
  }

  /** Returns @p time moved on by @p count periods. */
  QDateTime after(const QDateTime& time, int64_t count) const;

  /** Returns its length in seconds, months counted as 30 days. */
  double approximateSeconds() const;

  /**
   * Reads a duration; nothing if it isn't one (or has fractions, which no
   * service uses for time).
   */
  static std::optional<TimePeriod> parse(const QString& text);
};

/**
 * Returns @p text (an ISO 8601 date or date and time: `2004`, `2004-07`,
 * `2004-07-01`, `2004-07-01T12:00Z`, `2004-07-01T12:00:00.000Z`, with an
 * offset or none, which is read as UTC) as a UTC time; nothing if it
 * isn't one. "current", "present" and "now" are @p now.
 */
std::optional<QDateTime> parseIsoTime(const QString& text,
                                      const QDateTime& now);

/**
 * The times a dimension has values for: a comma-separated list of
 * instants and `start/end/period` intervals (WMS 1.3.0 Annex C, WMTS
 * Dimension values). Values are written back the way the extent writes
 * them (a date alone stays a date), so the server recognises them.
 *
 * @reentrant
 */
class TimeExtent
{
public:
  /** An empty extent: no times. */
  TimeExtent() = default;

  /**
   * Reads @p text; parts that can't be read are left out. Intervals ending
   * "current" (or "now", "present") end at @p now.
   */
  static TimeExtent
  parse(const QString& text,
        const QDateTime& now = QDateTime::currentDateTimeUtc());

  /** Reads each of @p values as parse() does and joins them. */
  static TimeExtent
  parse(const QStringList& values,
        const QDateTime& now = QDateTime::currentDateTimeUtc());

  /** Returns true if it has no times. */
  bool isEmpty() const { return m_parts.empty(); }

  /** Returns the earliest time, or nothing if empty. */
  std::optional<QDateTime> first() const;

  /** Returns the latest time, or nothing if empty. */
  std::optional<QDateTime> last() const;

  /**
   * Returns the latest time not after @p time; the earliest if all are
   * after it; nothing if empty.
   */
  std::optional<QDateTime> atOrBefore(const QDateTime& time) const;

  /** Returns the earliest time after @p time, or nothing. */
  std::optional<QDateTime> after(const QDateTime& time) const;

  /** Returns the latest time before @p time, or nothing. */
  std::optional<QDateTime> before(const QDateTime& time) const;

  /** Returns the number of times (intervals counted step by step). */
  int64_t count() const;

  /**
   * Returns the shortest step between its times: the smallest period of
   * its intervals, else of the gaps between listed instants; nothing for
   * a single time.
   */
  std::optional<double> shortestStepSeconds() const;

  /**
   * Returns @p time written as the extent writes the value it belongs to
   * (date only, to the minute, second or millisecond, with Z).
   */
  QString format(const QDateTime& time) const;

  /**
   * Returns up to @p limit times, earliest first (for lists in the user
   * interface).
   */
  std::vector<QDateTime> values(int64_t limit) const;

private:
  /** How a value is written. */
  enum class Precision : uint8_t { Year, Month, Day, Minute, Second, Milli };

  /** An instant (end == start, no period) or an interval. */
  struct Part {
    QDateTime start;
    QDateTime end;
    std::optional<TimePeriod> period;
    Precision precision = Precision::Second;
  };

  static Precision precisionOf(const QString& text);
  static QString write(const QDateTime& time, Precision precision);
  /** Index of the last step of @p part not after @p time (−1 if none). */
  static int64_t stepAtOrBefore(const Part& part, const QDateTime& time);
  static int64_t steps(const Part& part);
  const Part* partOf(const QDateTime& time) const;

  /** Sorted by start. */
  std::vector<Part> m_parts;
};

} // namespace ts

#endif // TS_TIME_TIMEEXTENT_H
