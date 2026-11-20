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
#include "ts/orbits/satellitelayer.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QJsonArray>
#include <QJsonValue>

#include "ts/orbits/elementsource.h"

namespace ts
{

namespace
{

QString originName(SatelliteSource::Origin origin)
{
  switch (origin) {
  case SatelliteSource::Origin::Celestrak:
    return QStringLiteral("celestrak");
  case SatelliteSource::Origin::Url:
    return QStringLiteral("url");
  case SatelliteSource::Origin::File:
    return QStringLiteral("file");
  case SatelliteSource::Origin::Text:
    return QStringLiteral("text");
  }
  return QString();
}

std::optional<SatelliteSource::Origin> originOf(const QString& name)
{
  for (const SatelliteSource::Origin o :
       {SatelliteSource::Origin::Celestrak, SatelliteSource::Origin::Url,
        SatelliteSource::Origin::File, SatelliteSource::Origin::Text}) {
    if (name == originName(o)) {
      return o;
    }
  }
  return std::nullopt;
}

template <typename Mode>
struct ModeName {
  Mode mode;
  const char* name;
};

const std::array<ModeName<TrackMode>, 4> trackModes{
    {{TrackMode::None, "none"},
     {TrackMode::Selected, "selected"},
     {TrackMode::Automatic, "automatic"},
     {TrackMode::All, "all"}}};

const std::array<ModeName<FootprintMode>, 3> footprintModes{
    {{FootprintMode::None, "none"},
     {FootprintMode::Selected, "selected"},
     {FootprintMode::All, "all"}}};

template <typename Mode, std::size_t N>
QString modeName(const std::array<ModeName<Mode>, N>& names, Mode mode)
{
  for (const ModeName<Mode>& entry : names) {
    if (entry.mode == mode) {
      return QLatin1String(entry.name);
    }
  }
  return QString();
}

template <typename Mode, std::size_t N>
Mode modeOf(const std::array<ModeName<Mode>, N>& names, const QJsonValue& value,
            Mode fallback)
{
  for (const ModeName<Mode>& entry : names) {
    if (value.toString() == QLatin1String(entry.name)) {
      return entry.mode;
    }
  }
  return fallback;
}

// The value's default, then its range.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double number(const QJsonObject& json, const char* key, double fallback,
              double low, double high)
{
  const QJsonValue value = json.value(QLatin1String(key));
  if (!value.isDouble()) {
    return fallback;
  }
  return std::clamp(value.toDouble(), low, high);
}

} // namespace

bool SatelliteSource::isSatellites(const QJsonObject& source)
{
  return source.value(QLatin1String("kind")).toString() == kind();
}

QJsonObject SatelliteSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), kind());
  json.insert(QStringLiteral("origin"), originName(origin));
  switch (origin) {
  case Origin::Celestrak:
    json.insert(QStringLiteral("group"), group);
    json.insert(QStringLiteral("url"), url.toString());
    break;
  case Origin::Url:
    json.insert(QStringLiteral("url"), url.toString());
    break;
  case Origin::File:
    json.insert(QStringLiteral("path"), path);
    break;
  case Origin::Text:
    json.insert(QStringLiteral("text"), text);
    break;
  }
  if (!attribution.isEmpty()) {
    json.insert(QStringLiteral("attribution"), attribution);
  }
  return json;
}

std::optional<SatelliteSource>
SatelliteSource::fromJson(const QJsonObject& json)
{
  if (!isSatellites(json)) {
    return std::nullopt;
  }
  const std::optional<Origin> origin =
      originOf(json.value(QLatin1String("origin")).toString());
  if (!origin) {
    return std::nullopt;
  }
  SatelliteSource source;
  source.origin = *origin;
  source.group = json.value(QLatin1String("group")).toString();
  source.url = QUrl(json.value(QLatin1String("url")).toString());
  source.path = json.value(QLatin1String("path")).toString();
  source.text = json.value(QLatin1String("text")).toString();
  source.attribution = json.value(QLatin1String("attribution")).toString();
  const bool complete =
      (source.isDownloaded() && source.url.isValid() &&
       !source.url.isEmpty()) ||
      (source.origin == Origin::File && !source.path.isEmpty()) ||
      (source.origin == Origin::Text && !source.text.isEmpty());
  return complete ? std::optional<SatelliteSource>(source) : std::nullopt;
}

SatelliteSource SatelliteSource::celestrak(const QString& group,
                                           const QUrl& base)
{
  SatelliteSource source;
  source.origin = Origin::Celestrak;
  source.group = group;
  source.url = celestrakGroupUrl(group, ElementFormat::OmmJson, base);
  // Not translated: it is stored in projects, and CelesTrak's name.
  source.attribution = QStringLiteral("Orbital elements: CelesTrak");
  return source;
}

void SatelliteStyle::setShown(int32_t catalogueNumber, bool shown)
{
  if (shown == shownByDefault) {
    exceptions.erase(catalogueNumber);
  } else {
    exceptions.insert(catalogueNumber);
  }
}

void SatelliteStyle::setAllShown(bool shown)
{
  shownByDefault = shown;
  exceptions.clear();
}

TimeSpan SatelliteStyle::pastOf(int32_t catalogueNumber) const
{
  const std::map<int32_t, SpanOverride>::const_iterator it =
      overrides.find(catalogueNumber);
  return it != overrides.end() ? it->second.past.value_or(past) : past;
}

TimeSpan SatelliteStyle::futureOf(int32_t catalogueNumber) const
{
  const std::map<int32_t, SpanOverride>::const_iterator it =
      overrides.find(catalogueNumber);
  return it != overrides.end() ? it->second.future.value_or(future) : future;
}

bool SatelliteStyle::drawsTrack(bool selected, int64_t shownCount) const
{
  switch (tracks) {
  case TrackMode::None:
    return false;
  case TrackMode::Selected:
    return selected;
  case TrackMode::Automatic:
    return selected || shownCount <= automaticTrackLimit;
  case TrackMode::All:
    return true;
  }
  return false;
}

bool SatelliteStyle::drawsFootprint(bool selected) const
{
  return footprints == FootprintMode::All ||
         (footprints == FootprintMode::Selected && selected);
}

QJsonObject SatelliteStyle::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
  json.insert(QStringLiteral("markerSizePx"), markerSizePx);
  json.insert(QStringLiteral("labels"), labels);
  json.insert(QStringLiteral("tracks"), modeName(trackModes, tracks));
  json.insert(QStringLiteral("past"), past.toJson());
  json.insert(QStringLiteral("future"), future.toJson());
  json.insert(QStringLiteral("groundTrack"), groundTrack);
  json.insert(QStringLiteral("footprints"),
              modeName(footprintModes, footprints));
  json.insert(QStringLiteral("footprintElevationDeg"), footprintElevationDeg);
  json.insert(QStringLiteral("maximumAgeDays"), maximumAgeDays);
  json.insert(QStringLiteral("shown"),
              shownByDefault ? QStringLiteral("all") : QStringLiteral("none"));
  QJsonArray except;
  for (const int32_t n : exceptions) {
    except.append(n);
  }
  json.insert(QStringLiteral("except"), except);
  QJsonObject own;
  for (const std::pair<const int32_t, SpanOverride>& entry : overrides) {
    if (entry.second.isEmpty()) {
      continue;
    }
    QJsonObject spans;
    if (entry.second.past) {
      spans.insert(QStringLiteral("past"), entry.second.past->toJson());
    }
    if (entry.second.future) {
      spans.insert(QStringLiteral("future"), entry.second.future->toJson());
    }
    own.insert(QString::number(entry.first), spans);
  }
  if (!own.isEmpty()) {
    json.insert(QStringLiteral("overrides"), own);
  }
  return json;
}

SatelliteStyle SatelliteStyle::fromJson(const QJsonObject& json)
{
  SatelliteStyle style;
  // A colour that isn't one keeps the default.
  const QColor color(json.value(QLatin1String("color")).toString());
  if (color.isValid()) {
    style.color = color;
  }
  style.markerSizePx =
      number(json, "markerSizePx", style.markerSizePx, 1.0, 64.0);
  style.labels = json.value(QLatin1String("labels")).toBool(style.labels);
  style.tracks =
      modeOf(trackModes, json.value(QLatin1String("tracks")), style.tracks);
  style.past = TimeSpan::fromJson(json.value(QLatin1String("past")).toObject())
                   .value_or(style.past);
  style.future =
      TimeSpan::fromJson(json.value(QLatin1String("future")).toObject())
          .value_or(style.future);
  style.groundTrack =
      json.value(QLatin1String("groundTrack")).toBool(style.groundTrack);
  style.footprints =
      modeOf(footprintModes, json.value(QLatin1String("footprints")),
             style.footprints);
  style.footprintElevationDeg = number(json, "footprintElevationDeg",
                                       style.footprintElevationDeg, 0.0, 89.0);
  style.maximumAgeDays =
      number(json, "maximumAgeDays", style.maximumAgeDays, 0.0, 100000.0);
  style.shownByDefault =
      json.value(QLatin1String("shown")).toString() != QLatin1String("none");
  const QJsonArray except = json.value(QLatin1String("except")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert): the same on Qt 5 and Qt 6
  for (int32_t i = 0; i < static_cast<int32_t>(except.size()); ++i) {
    if (except.at(i).isDouble()) {
      style.exceptions.insert(except.at(i).toInt());
    }
  }
  const QJsonObject own = json.value(QLatin1String("overrides")).toObject();
  for (QJsonObject::const_iterator it = own.begin(); it != own.end(); ++it) {
    bool ok = false;
    const int32_t n = it.key().toInt(&ok);
    const QJsonObject spans = it.value().toObject();
    SpanOverride entry;
    entry.past =
        TimeSpan::fromJson(spans.value(QLatin1String("past")).toObject());
    entry.future =
        TimeSpan::fromJson(spans.value(QLatin1String("future")).toObject());
    if (ok && !entry.isEmpty()) {
      style.overrides[n] = entry;
    }
  }
  return style;
}

bool SatelliteStyle::operator==(const SatelliteStyle& other) const
{
  return color == other.color && markerSizePx == other.markerSizePx &&
         labels == other.labels && tracks == other.tracks &&
         past == other.past && future == other.future &&
         groundTrack == other.groundTrack && footprints == other.footprints &&
         footprintElevationDeg == other.footprintElevationDeg &&
         maximumAgeDays == other.maximumAgeDays &&
         shownByDefault == other.shownByDefault &&
         exceptions == other.exceptions && overrides == other.overrides;
}

} // namespace ts
