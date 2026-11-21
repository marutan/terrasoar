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
 * Ship layers: where their AIS data comes from (a recording, a receiver,
 * an open online feed) and how they are drawn.
 */
#ifndef TS_AIS_VESSELLAYER_H
#define TS_AIS_VESSELLAYER_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include "ts/adsb/aircraftlayer.h"
#include "ts/ais/aisreaders.h"
#include "ts/ais/vesseldraws.h"
#include "ts/tracks/movingtrack.h"

namespace ts
{

/** The open online AIS feeds Terra Soar knows. */
enum class AisFeedKind : uint8_t {
  Digitraffic, ///< Fintraffic Digitraffic (Finland, the Baltic), REST.
  Kystverket,  ///< The Norwegian Coastal Administration, NMEA over TCP.
};

/** A catalogue entry of an open AIS feed: where, its terms and credit. */
struct AisFeedEntry {
  /** The feed. */
  AisFeedKind feed = AisFeedKind::Digitraffic;
  /** Its identifier ("digitraffic", "kystverket"). */
  QString id;
  /** Its name (translated). */
  QString name;
  /** What it covers (translated). */
  QString coverage;
  /** The licence ("CC BY 4.0", "NLOD"). */
  QString licence;
  /** The credit shown while the layer is. */
  QString attribution;
  /** Its address: the REST base, or tcp://host:port. */
  QUrl url;
  /** How often it is polled (REST), seconds. */
  double pollS = 30.0;
};

/** Returns the open feeds' catalogue entries. */
const std::vector<AisFeedEntry>& aisFeedCatalogue();

/** Returns the catalogue entry of feed @p id, if there is one. */
std::optional<AisFeedEntry> aisFeedEntry(const QString& id);

/**
 * Where a ship layer's data comes from.
 *
 * @reentrant
 */
struct VesselSource {
  /** Where it comes from. */
  enum class Origin : uint8_t {
    File,     ///< A recording.
    Receiver, ///< A receiver sending NMEA (AIS-catcher, rtl-ais, a ship's).
    Feed,     ///< An open online feed.
  };
  /** Where it comes from. */
  Origin origin = Origin::File;

  /** The recording (File). */
  QString path;
  /** Its format (Unknown: detected). */
  AisFormat format = AisFormat::Unknown;
  /** The time of a log without times, Unix seconds. */
  double startTimeS = 0.0;
  /** Its columns (CSV). */
  AisCsvColumns csv;

  /** True to listen for UDP, false to connect over TCP (Receiver). */
  bool udp = false;
  /** The receiver's host (TCP). */
  QString host;
  /** Its port (TCP), or the port listened on (UDP). */
  uint16_t port = 10110;
  /** A file the session is recorded to (empty: not recorded). */
  QString recordPath;

  /** The feed's identifier (Feed). */
  QString feed;
  /** Its address, if not the catalogue's (tests, mirrors). */
  QUrl url;

  /** The source kind. */
  static QString kind() { return QStringLiteral("vessels"); }
  /** Returns true if @p source is a ship layer's. */
  static bool isVessels(const QJsonObject& source);
  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form; nothing if it isn't one. */
  static std::optional<VesselSource> fromJson(const QJsonObject& json);
  /** Returns how a recording is read. */
  AisReadOptions readOptions() const;
  /** Returns the feed's address: url, or the catalogue's. */
  QUrl feedUrl() const;
  /** Returns the credit to show (feeds), or empty. */
  QString attribution() const;
};

/**
 * How a ship layer is drawn, and which stations it shows.
 *
 * @reentrant
 */
struct VesselStyle {
  /** Markers' colour when not by type (and trails' by vessel). */
  QColor color = QColor(240, 240, 240);
  /** Markers coloured by ship type (the legend), else the colour. */
  bool colourByType = true;
  /** Markers' size (length), logical pixels. */
  double markerSizePx = 16.0;
  /** Names (or MMSIs) beside the markers. */
  bool labels = true;
  /** The speed in the label. */
  bool labelSpeed = false;
  /** The destination in the label. */
  bool labelDestination = false;
  /** Course vectors this many minutes ahead (0: none). */
  double courseVectorMinutes = 0.0;
  /** The trails. */
  TrailSettings trail;
  /** What the trails' colour shows. */
  VesselColouring trailColouring = VesselColouring::Type;
  /** Shown at least this long after the last report, seconds. */
  double staleMinimumS = 360.0;
  /** Reports farther apart are a gap in the trail, seconds. */
  double gapS = 600.0;

  /** Only names (or MMSIs) containing this (empty: all). */
  QString nameFilter;
  /** Only these ship type groups (empty: all). */
  std::set<ShipTypeGroup> typeGroups;
  /** Only these navigational statuses (empty: all). */
  std::set<AisNavStatus> statuses;
  /** Only at or above this speed, knots. */
  std::optional<double> minimumSpeedKn;
  /** Only at or below this speed, knots. */
  std::optional<double> maximumSpeedKn;
  /** Only within this area. */
  std::optional<LatLonBox> area;

  /** Stations hidden by hand (by MMSI). */
  std::set<uint32_t> hidden;
  /** Stations' own trails, by MMSI. */
  std::map<uint32_t, TrailSettings> trails;

  /** Returns station @p mmsi's trail. */
  TrailSettings trailOf(uint32_t mmsi) const;

  /**
   * Returns true if station @p mmsi, as @p info says and at @p at, passes
   * the filters and isn't hidden.
   */
  bool shows(uint32_t mmsi, const VesselInfo& info,
             const TrackSample& at) const;

  /** Returns the group a station is coloured and filtered by. */
  static ShipTypeGroup groupOf(const VesselInfo& info);

  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form; missing members keep their defaults. */
  static VesselStyle fromJson(const QJsonObject& json);
  /** Equality. */
  bool operator==(const VesselStyle& other) const;
};

} // namespace ts

#endif // TS_AIS_VESSELLAYER_H
