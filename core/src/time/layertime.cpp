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
#include "ts/time/layertime.h"

#include <QJsonArray>

#include "ts/layers/layertreemodel.h"

namespace ts
{

namespace
{

QString modeName(LayerTime::Mode mode)
{
  switch (mode) {
  case LayerTime::Mode::Fixed:
    return QStringLiteral("fixed");
  case LayerTime::Mode::MonthOfYear:
    return QStringLiteral("month");
  case LayerTime::Mode::Clock:
    break;
  }
  return QStringLiteral("clock");
}

} // namespace

QString LayerTime::valueAt(const QDateTime& clock) const
{
  if (mode == Mode::Fixed) {
    return value.isEmpty() ? defaultValue : value;
  }
  const TimeExtent extent = times();
  if (extent.isEmpty()) {
    return defaultValue;
  }
  const QDateTime utc = clock.toUTC();
  if (mode == Mode::MonthOfYear) {
    for (const QDateTime& candidate : extent.values(400)) {
      if (candidate.date().month() == utc.date().month()) {
        return extent.format(candidate);
      }
    }
  }
  const std::optional<QDateTime> chosen = extent.atOrBefore(utc);
  return chosen ? extent.format(*chosen) : defaultValue;
}

bool LayerTime::followsClock() const
{
  return mode != Mode::Fixed && !times().isEmpty();
}

QJsonObject LayerTime::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("dimension"), dimension);
  json.insert(QStringLiteral("extent"), extent);
  if (!defaultValue.isEmpty()) {
    json.insert(QStringLiteral("default"), defaultValue);
  }
  json.insert(QStringLiteral("mode"), modeName(mode));
  if (mode == Mode::Fixed) {
    json.insert(QStringLiteral("value"), value);
  }
  return json;
}

std::optional<LayerTime> LayerTime::fromSource(const QJsonObject& source)
{
  const QJsonValue value = source.value(QStringLiteral("time"));
  if (!value.isObject()) {
    return std::nullopt;
  }
  const QJsonObject json = value.toObject();
  LayerTime time;
  time.dimension = json.value(QStringLiteral("dimension")).toString().trimmed();
  if (time.dimension.isEmpty()) {
    return std::nullopt;
  }
  time.extent = json.value(QStringLiteral("extent")).toString();
  time.defaultValue = json.value(QStringLiteral("default")).toString();
  const QString mode = json.value(QStringLiteral("mode")).toString();
  if (mode == QLatin1String("fixed")) {
    time.mode = Mode::Fixed;
  } else if (mode == QLatin1String("month")) {
    time.mode = Mode::MonthOfYear;
  }
  time.value = json.value(QStringLiteral("value")).toString();
  return time;
}

bool LayerTime::isMonthly(const TimeExtent& extent)
{
  const std::vector<QDateTime> values = extent.values(13);
  if (values.size() != 12) {
    return false;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    const QDate date = values.at(i).toUTC().date();
    if (date.year() != values.front().toUTC().date().year() ||
        date.month() != static_cast<int32_t>(i) + 1) {
      return false;
    }
  }
  return true;
}

QJsonObject sourceAtTime(const QJsonObject& source, const QDateTime& clock)
{
  const std::optional<LayerTime> time = LayerTime::fromSource(source);
  if (!time) {
    return source;
  }
  const QString value = time->valueAt(clock);
  QJsonArray dimensions = source.value(QStringLiteral("dimensions")).toArray();
  bool replaced = false;
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < dimensions.size(); ++i) {
    const QJsonArray pair = dimensions.at(i).toArray();
    if (pair.at(0).toString().compare(time->dimension, Qt::CaseInsensitive) ==
        0) {
      dimensions.replace(i, QJsonArray{pair.at(0).toString(), value});
      replaced = true;
    }
  }
  if (!replaced && !value.isEmpty()) {
    dimensions.append(QJsonArray{time->dimension, value});
  }
  QJsonObject drawn = source;
  if (!dimensions.isEmpty()) {
    drawn.insert(QStringLiteral("dimensions"), dimensions);
  }
  return drawn;
}

LayerStack stackAtTime(const LayerStack& stack, const QDateTime& clock)
{
  LayerStack drawn = stack;
  for (ImageryStackEntry& entry : drawn.imagery) {
    entry.definition.source = sourceAtTime(entry.definition.source, clock);
  }
  return drawn;
}

} // namespace ts
