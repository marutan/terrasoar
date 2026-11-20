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
#include "ts/time/timeextent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

#include <QDate>
#include <QRegularExpression>
#include <QTime>

namespace ts
{

namespace
{

constexpr int64_t msPerSecond = 1000;
constexpr int64_t secondsPerDay = 86400;
/** Most values listed for the user. */
constexpr int64_t largestCount = std::numeric_limits<int32_t>::max();

/** Returns the UTC time @p ms after 1970. */
QDateTime utcFromMs(int64_t ms)
{
  return QDateTime::fromMSecsSinceEpoch(ms).toUTC();
}

/** Returns @p date at @p time, UTC, @p offsetS east of UTC. */
QDateTime utcAt(const QDate& date, const QTime& time, int64_t offsetS)
{
  const int64_t days = QDate(1970, 1, 1).daysTo(date);
  return utcFromMs(days * secondsPerDay * msPerSecond +
                   time.msecsSinceStartOfDay() - offsetS * msPerSecond);
}

bool isNow(const QString& text)
{
  const QString lower = text.trimmed().toLower();
  return lower == QLatin1String("current") ||
         lower == QLatin1String("present") || lower == QLatin1String("now");
}

} // namespace

QDateTime TimePeriod::after(const QDateTime& time, int64_t count) const
{
  QDateTime moved = time;
  if (years != 0 || months != 0) {
    moved = moved.addMonths(
        static_cast<int32_t>(count * (int64_t{years} * 12 + months)));
  }
  if (days != 0) {
    moved = moved.addDays(count * days);
  }
  if (seconds != 0) {
    moved = moved.addMSecs(count * seconds * msPerSecond);
  }
  return moved;
}

double TimePeriod::approximateSeconds() const
{
  return (static_cast<double>(years) * 365.25 +
          static_cast<double>(months) * 30.0 + static_cast<double>(days)) *
             static_cast<double>(secondsPerDay) +
         static_cast<double>(seconds);
}

std::optional<TimePeriod> TimePeriod::parse(const QString& text)
{
  static const QRegularExpression pattern(
      QStringLiteral("^P(?:(\\d+)Y)?(?:(\\d+)M)?(?:(\\d+)W)?(?:(\\d+)D)?"
                     "(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$"));
  const QString trimmed = text.trimmed().toUpper();
  const QRegularExpressionMatch match = pattern.match(trimmed);
  if (!match.hasMatch() || trimmed == QLatin1String("P") ||
      trimmed.endsWith(QLatin1Char('T'))) {
    return std::nullopt;
  }
  TimePeriod period;
  period.years = match.captured(1).toInt();
  period.months = match.captured(2).toInt();
  period.days = match.captured(3).toInt() * 7 + match.captured(4).toInt();
  period.seconds = match.captured(5).toLongLong() * 3600 +
                   match.captured(6).toLongLong() * 60 +
                   match.captured(7).toLongLong();
  if (period.isZero()) {
    return std::nullopt;
  }
  return period;
}

std::optional<QDateTime> parseIsoTime(const QString& text, const QDateTime& now)
{
  if (isNow(text)) {
    return now.toUTC();
  }
  static const QRegularExpression pattern(QStringLiteral(
      "^(\\d{4})(?:-(\\d{2})(?:-(\\d{2})(?:[T ](\\d{2}):(\\d{2})"
      "(?::(\\d{2})(?:\\.(\\d{1,9}))?)?)?)?)?(Z|[+-]\\d{2}(?::?\\d{2})?)?$"));
  const QRegularExpressionMatch match = pattern.match(text.trimmed());
  if (!match.hasMatch()) {
    return std::nullopt;
  }
  const int32_t year = match.captured(1).toInt();
  const int32_t month =
      match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
  const int32_t day =
      match.captured(3).isEmpty() ? 1 : match.captured(3).toInt();
  const int32_t hour = match.captured(4).toInt();
  const int32_t minute = match.captured(5).toInt();
  const int32_t second = match.captured(6).toInt();
  const QString fraction =
      match.captured(7).left(3).leftJustified(3, QLatin1Char('0'));
  const int32_t ms = match.captured(7).isEmpty() ? 0 : fraction.toInt();
  const QDate date(year, month, day);
  const QTime time(hour, minute, second, ms);
  if (!date.isValid() || !time.isValid()) {
    return std::nullopt;
  }
  int64_t offsetS = 0;
  const QString zone = match.captured(8);
  if (zone.size() > 1) {
    const QString digits = zone.mid(1).remove(QLatin1Char(':'));
    offsetS = digits.left(2).toLongLong() * 3600 +
              (digits.size() >= 4 ? digits.mid(2, 2).toLongLong() * 60 : 0);
    if (zone.startsWith(QLatin1Char('-'))) {
      offsetS = -offsetS;
    }
  }
  return utcAt(date, time, offsetS);
}

TimeExtent::Precision TimeExtent::precisionOf(const QString& text)
{
  const QString trimmed = text.trimmed();
  if (isNow(trimmed)) {
    return Precision::Second;
  }
  const int32_t t = static_cast<int32_t>(trimmed.indexOf(QLatin1Char('T')));
  if (t < 0) {
    switch (trimmed.size()) {
    case 4:
      return Precision::Year;
    case 7:
      return Precision::Month;
    default:
      return Precision::Day;
    }
  }
  const QString clock = trimmed.mid(t + 1);
  if (clock.contains(QLatin1Char('.'))) {
    return Precision::Milli;
  }
  return clock.count(QLatin1Char(':')) >= 2 ||
                 (clock.size() >= 8 && clock.at(5) == QLatin1Char(':'))
             ? Precision::Second
             : Precision::Minute;
}

QString TimeExtent::write(const QDateTime& time, Precision precision)
{
  const QDateTime utc = time.toUTC();
  switch (precision) {
  case Precision::Year:
    return utc.toString(QStringLiteral("yyyy"));
  case Precision::Month:
    return utc.toString(QStringLiteral("yyyy-MM"));
  case Precision::Day:
    return utc.toString(QStringLiteral("yyyy-MM-dd"));
  case Precision::Minute:
    return utc.toString(QStringLiteral("yyyy-MM-ddTHH:mm")) + QLatin1Char('Z');
  case Precision::Second:
    return utc.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) +
           QLatin1Char('Z');
  case Precision::Milli:
    return utc.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) +
           QLatin1Char('Z');
  }
  return utc.toString(Qt::ISODate);
}

TimeExtent TimeExtent::parse(const QString& text, const QDateTime& now)
{
  return parse(text.split(QLatin1Char(','), Qt::SkipEmptyParts), now);
}

TimeExtent TimeExtent::parse(const QStringList& values, const QDateTime& now)
{
  TimeExtent extent;
  for (const QString& value : values) {
    // A list item may itself be a list (WMTS values written "a,b").
    const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& item : items) {
      const QStringList pieces = item.trimmed().split(QLatin1Char('/'));
      Part part;
      part.precision = precisionOf(pieces.first());
      const std::optional<QDateTime> start = parseIsoTime(pieces.first(), now);
      if (!start) {
        continue;
      }
      part.start = *start;
      part.end = *start;
      if (pieces.size() >= 2) {
        const std::optional<QDateTime> end = parseIsoTime(pieces.at(1), now);
        if (!end || *end < *start) {
          continue;
        }
        part.end = *end;
        if (pieces.size() >= 3) {
          part.period = TimePeriod::parse(pieces.at(2));
        }
        if (!part.period) {
          // A continuous interval (no resolution): any time in it, written
          // to the second.
          part.precision = std::max(part.precision, Precision::Second);
        }
      }
      extent.m_parts.push_back(part);
    }
  }
  std::stable_sort(
      extent.m_parts.begin(), extent.m_parts.end(),
      [](const Part& a, const Part& b) { return a.start < b.start; });
  return extent;
}

int64_t TimeExtent::steps(const Part& part)
{
  if (!part.period) {
    return 0;
  }
  return stepAtOrBefore(part, part.end);
}

int64_t TimeExtent::stepAtOrBefore(const Part& part, const QDateTime& time)
{
  if (time < part.start) {
    return -1;
  }
  if (!part.period) {
    return 0;
  }
  const TimePeriod& period = *part.period;
  // An estimate from the approximate length, then corrected.
  const double estimate =
      static_cast<double>(part.start.msecsTo(time)) /
      (period.approximateSeconds() * static_cast<double>(msPerSecond));
  int64_t n = static_cast<int64_t>(
      std::floor(std::min(estimate, static_cast<double>(largestCount))));
  n = std::max(n, int64_t{0});
  while (n > 0 && period.after(part.start, n) > time) {
    --n;
  }
  while (n < largestCount && period.after(part.start, n + 1) <= time) {
    ++n;
  }
  return n;
}

std::optional<QDateTime> TimeExtent::first() const
{
  if (m_parts.empty()) {
    return std::nullopt;
  }
  return m_parts.front().start;
}

std::optional<QDateTime> TimeExtent::last() const
{
  std::optional<QDateTime> latest;
  for (const Part& part : m_parts) {
    const QDateTime end =
        part.period ? part.period->after(part.start, steps(part)) : part.end;
    if (!latest || end > *latest) {
      latest = end;
    }
  }
  return latest;
}

std::optional<QDateTime> TimeExtent::atOrBefore(const QDateTime& time) const
{
  std::optional<QDateTime> best;
  for (const Part& part : m_parts) {
    std::optional<QDateTime> candidate;
    if (time >= part.start) {
      if (!part.period) {
        // An instant, or any time within a continuous interval.
        candidate = time <= part.end ? time : part.end;
      } else {
        const int64_t n = std::min(stepAtOrBefore(part, time), steps(part));
        candidate = part.period->after(part.start, n);
      }
    }
    if (candidate && (!best || *candidate > *best)) {
      best = candidate;
    }
  }
  return best ? best : first();
}

std::optional<QDateTime> TimeExtent::after(const QDateTime& time) const
{
  std::optional<QDateTime> best;
  for (const Part& part : m_parts) {
    std::optional<QDateTime> candidate;
    if (part.start > time) {
      candidate = part.start;
    } else if (part.period) {
      const int64_t n = stepAtOrBefore(part, time) + 1;
      if (n <= steps(part)) {
        candidate = part.period->after(part.start, n);
      }
    }
    if (candidate && (!best || *candidate < *best)) {
      best = candidate;
    }
  }
  return best;
}

std::optional<QDateTime> TimeExtent::before(const QDateTime& time) const
{
  std::optional<QDateTime> best;
  for (const Part& part : m_parts) {
    std::optional<QDateTime> candidate;
    if (part.start < time) {
      if (!part.period) {
        candidate = part.end < time ? part.end : part.start;
      } else {
        int64_t n = std::min(stepAtOrBefore(part, time), steps(part));
        if (part.period->after(part.start, n) >= time) {
          --n;
        }
        if (n >= 0) {
          candidate = part.period->after(part.start, n);
        }
      }
    }
    if (candidate && (!best || *candidate > *best)) {
      best = candidate;
    }
  }
  return best;
}

int64_t TimeExtent::count() const
{
  int64_t total = 0;
  for (const Part& part : m_parts) {
    total += steps(part) + 1;
  }
  return total;
}

std::optional<double> TimeExtent::shortestStepSeconds() const
{
  std::optional<double> shortest;
  const std::function<void(double)> consider = [&shortest](double seconds) {
    if (seconds > 0.0 && (!shortest || seconds < *shortest)) {
      shortest = seconds;
    }
  };
  for (std::size_t i = 0; i < m_parts.size(); ++i) {
    const Part& part = m_parts.at(i);
    if (part.period && steps(part) > 0) {
      consider(part.period->approximateSeconds());
    }
    if (i > 0) {
      consider(
          static_cast<double>(m_parts.at(i - 1).start.msecsTo(part.start)) /
          static_cast<double>(msPerSecond));
    }
  }
  return shortest;
}

const TimeExtent::Part* TimeExtent::partOf(const QDateTime& time) const
{
  const Part* found = nullptr;
  for (const Part& part : m_parts) {
    if (part.start <= time) {
      found = &part;
    }
  }
  return found != nullptr ? found
                          : (m_parts.empty() ? nullptr : &m_parts.front());
}

QString TimeExtent::format(const QDateTime& time) const
{
  const Part* part = partOf(time);
  return write(time, part != nullptr ? part->precision : Precision::Second);
}

std::vector<QDateTime> TimeExtent::values(int64_t limit) const
{
  std::vector<QDateTime> list;
  for (const Part& part : m_parts) {
    const int64_t n = steps(part);
    for (int64_t i = 0; i <= n; ++i) {
      if (static_cast<int64_t>(list.size()) >= limit) {
        return list;
      }
      list.push_back(part.period ? part.period->after(part.start, i)
                                 : part.start);
    }
  }
  return list;
}

} // namespace ts
