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
 * How a layer's time dimension follows the program's clock, and the
 * source it draws at a given time.
 */
#ifndef TS_TIME_LAYERTIME_H
#define TS_TIME_LAYERTIME_H

#include <optional>

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include "ts/time/timeextent.h"

namespace ts
{

struct LayerStack;

/**
 * The time settings of a layer whose source has a time dimension (a WMS
 * TIME, a WMTS Time), kept in the source object as `"time": {…}`
 * (toJson()). The source's own `dimensions` hold the value requested;
 * sourceAtTime() puts in the value for the clock's time.
 *
 * @reentrant
 */
struct LayerTime {
  /** How the value is chosen. */
  enum class Mode : uint8_t {
    /** The latest of the layer's times not after the clock's. */
    Clock,
    /** Always value. */
    Fixed,
    /**
     * The layer's time in the clock's month of the year (Blue Marble's
     * monthly pictures, any other climatology): seasons follow the clock
     * whatever its year.
     */
    MonthOfYear,
  };

  /** The dimension's name as the source keeps it ("time", "Time"). */
  QString dimension = QStringLiteral("time");
  /** The times the layer has, as the service writes them (TimeExtent). */
  QString extent;
  /** The service's default value. */
  QString defaultValue;
  /** How the value is chosen. */
  Mode mode = Mode::Clock;
  /** The value for Mode::Fixed. */
  QString value;

  /** Returns the parsed extent. */
  TimeExtent times() const { return TimeExtent::parse(extent); }

  /**
   * Returns the value to request at @p clock: see Mode. The default (or
   * the fixed value) if the extent has no times.
   */
  QString valueAt(const QDateTime& clock) const;

  /**
   * Returns true if the layer changes with the clock (Clock or
   * MonthOfYear, with times to choose from).
   */
  bool followsClock() const;

  /** Returns the object kept in the source as "time". */
  QJsonObject toJson() const;

  /**
   * Reads @p source's "time" object; nothing if it has none or it names
   * no dimension.
   */
  static std::optional<LayerTime> fromSource(const QJsonObject& source);

  /**
   * Returns true if @p extent's times are the months of one year (twelve
   * monthly values, or a year from start to end by P1M): Mode::MonthOfYear
   * makes sense for it.
   */
  static bool isMonthly(const TimeExtent& extent);
};

/**
 * Returns @p source as drawn at @p clock: if it has a LayerTime, the value
 * for that time in its `dimensions` (replacing the dimension's value, or
 * added); else @p source unchanged.
 */
QJsonObject sourceAtTime(const QJsonObject& source, const QDateTime& clock);

/** Returns @p stack with each imagery source as drawn at @p clock. */
LayerStack stackAtTime(const LayerStack& stack, const QDateTime& clock);

} // namespace ts

#endif // TS_TIME_LAYERTIME_H
