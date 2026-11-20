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
 * A satellite layer's source and style, as stored in its LayerDefinition.
 */
#ifndef TS_ORBITS_SATELLITELAYER_H
#define TS_ORBITS_SATELLITELAYER_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include "ts/orbits/satellitetrack.h"

namespace ts
{

/**
 * Where a satellite layer's element sets come from: the layer's "source"
 * object, kind "satellites".
 *
 * ```json
 * { "kind": "satellites", "origin": "celestrak", "group": "stations",
 *   "url":
 * "https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=json",
 *   "attribution": "Orbital elements: CelesTrak" }
 * { "kind": "satellites", "origin": "file", "path": "/data/amateur.tle" }
 * { "kind": "satellites", "origin": "text", "text": "ISS (ZARYA)\n1 25544U …" }
 * ```
 *
 * @reentrant
 */
struct SatelliteSource {
  /** Where it comes from. */
  enum class Origin : uint8_t {
    Celestrak, ///< A CelesTrak group (or satellites by number), downloaded.
    Url,       ///< Any address with TLEs or OMM JSON, downloaded.
    File,      ///< A TLE or OMM file.
    Text,      ///< Text pasted in, kept in the layer.
  };
  /** Where it comes from. */
  Origin origin = Origin::Celestrak;
  /** The CelesTrak group (Celestrak). */
  QString group;
  /** The address (Celestrak, Url). */
  QUrl url;
  /** The file (File). */
  QString path;
  /** The element sets (Text). */
  QString text;
  /** The credit shown while the layer is (empty: none). */
  QString attribution;

  /** The source kind. */
  static QString kind() { return QStringLiteral("satellites"); }

  /** Returns true if @p source is a satellite layer's. */
  static bool isSatellites(const QJsonObject& source);

  /** Returns true if it is downloaded (Celestrak, Url). */
  bool isDownloaded() const
  {
    return origin == Origin::Celestrak || origin == Origin::Url;
  }

  /** Returns the JSON form. */
  QJsonObject toJson() const;

  /** Reads the JSON form; nothing if it isn't one. */
  static std::optional<SatelliteSource> fromJson(const QJsonObject& json);

  /** Returns a CelesTrak group's source at @p base (null: CelesTrak's). */
  static SatelliteSource celestrak(const QString& group,
                                   const QUrl& base = QUrl());
};

/** Which shown satellites draw their tracks. */
enum class TrackMode : uint8_t {
  /** None. */
  None,
  /** The selected satellite only. */
  Selected,
  /** All shown, or only the selected one when more than 20 are shown. */
  Automatic,
  /** All shown. */
  All,
};

/** Which shown satellites draw their footprints. */
enum class FootprintMode : uint8_t {
  /** None. */
  None,
  /** The selected satellite only. */
  Selected,
  /** All shown. */
  All,
};

/** A satellite's own spans, overriding the layer's. */
struct SpanOverride {
  /** Its past span, or the layer's. */
  std::optional<TimeSpan> past;
  /** Its future span, or the layer's. */
  std::optional<TimeSpan> future;

  /** Returns true if it overrides nothing. */
  bool isEmpty() const { return !past && !future; }

  /** Equality. */
  bool operator==(const SpanOverride& other) const
  {
    return past == other.past && future == other.future;
  }
};

/**
 * How a satellite layer is drawn and which of its satellites are shown:
 * the layer's "style" object.
 *
 * ```json
 * { "color": "#ffcc00ff", "markerSizePx": 7, "labels": true,
 *   "tracks": "automatic", "past": {"amount": 1, "unit": "orbits"},
 *   "future": {"amount": 1, "unit": "orbits"}, "groundTrack": false,
 *   "footprints": "selected", "footprintElevationDeg": 10,
 *   "maximumAgeDays": 14, "shown": "all", "except": [25544],
 *   "overrides": {"25544": {"future": {"amount": 90, "unit": "minutes"}}} }
 * ```
 *
 * @reentrant
 */
struct SatelliteStyle {
  /** Markers', labels' and tracks' colour (and opacity). */
  QColor color = QColor(255, 204, 0);
  /** Markers' size (diameter), logical pixels. */
  double markerSizePx = 7.0;
  /** Names beside the markers. */
  bool labels = true;
  /** Which satellites draw tracks. */
  TrackMode tracks = TrackMode::Automatic;
  /** The past span of tracks. */
  TimeSpan past{1.0, SpanUnit::Orbits};
  /** The future span of tracks. */
  TimeSpan future{1.0, SpanUnit::Orbits};
  /** Ground tracks under the tracks on the globe. */
  bool groundTrack = false;
  /** Which satellites draw footprints. */
  FootprintMode footprints = FootprintMode::None;
  /** The elevation footprints are seen above, degrees. */
  double footprintElevationDeg = 10.0;
  /** Elements older than this (from the clock's time) are marked, days. */
  double maximumAgeDays = 14.0;
  /** True if satellites are shown unless excepted, false if hidden. */
  bool shownByDefault = true;
  /** The catalogue numbers that are the other way. */
  std::set<int32_t> exceptions;
  /** Satellites' own spans, by catalogue number. */
  std::map<int32_t, SpanOverride> overrides;

  /** Returns true if satellite @p catalogueNumber is shown. */
  bool isShown(int32_t catalogueNumber) const
  {
    return (exceptions.count(catalogueNumber) == 0) == shownByDefault;
  }

  /** Shows or hides satellite @p catalogueNumber. */
  void setShown(int32_t catalogueNumber, bool shown);

  /** Shows (or hides) every satellite. */
  void setAllShown(bool shown);

  /** Returns satellite @p catalogueNumber's past span. */
  TimeSpan pastOf(int32_t catalogueNumber) const;

  /** Returns satellite @p catalogueNumber's future span. */
  TimeSpan futureOf(int32_t catalogueNumber) const;

  /**
   * Returns true if a satellite draws its track when @p shownCount are
   * shown and it is (or isn't) @p selected.
   */
  bool drawsTrack(bool selected, int64_t shownCount) const;

  /** Returns true if a satellite draws its footprint. */
  bool drawsFootprint(bool selected) const;

  /** Most satellites shown for which TrackMode::Automatic draws all. */
  static constexpr int64_t automaticTrackLimit = 20;

  /** Returns the JSON form. */
  QJsonObject toJson() const;

  /** Reads the JSON form; missing members keep their defaults. */
  static SatelliteStyle fromJson(const QJsonObject& json);

  /** Equality. */
  bool operator==(const SatelliteStyle& other) const;
};

} // namespace ts

#endif // TS_ORBITS_SATELLITELAYER_H
