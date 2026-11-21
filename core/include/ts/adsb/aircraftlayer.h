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
 * Aircraft layers: where their data comes from (a recording, a receiver)
 * and how they are drawn.
 */
#ifndef TS_ADSB_AIRCRAFTLAYER_H
#define TS_ADSB_AIRCRAFTLAYER_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftdraws.h"
#include "ts/adsb/aircrafttracks.h"
#include "ts/tracks/movingtrack.h"

namespace ts
{

/** How a receiver sends its data. */
enum class ReceiverProtocol : uint8_t {
  Sbs,   ///< SBS-1/BaseStation lines (dump1090/readsb port 30003).
  Avr,   ///< Raw messages as AVR text (port 30002).
  Beast, ///< Beast binary (port 30005).
  Json,  ///< aircraft.json over HTTP, polled (dump1090/readsb's web).
};

/** Returns the protocol's identifier ("sbs", "avr", "beast", "json"). */
QString receiverProtocolId(ReceiverProtocol protocol);

/** Returns the protocol with identifier @p id, if it is one. */
std::optional<ReceiverProtocol> receiverProtocolFromId(const QString& id);

/** Returns the usual port of @p protocol (30003, 30002, 30005, 8080). */
uint16_t defaultReceiverPort(ReceiverProtocol protocol);

/**
 * Where an aircraft layer's data comes from: a recording, or a receiver
 * on the network, live.
 *
 * @reentrant
 */
struct AircraftSource {
  /** Where it comes from. */
  enum class Origin : uint8_t {
    File,     ///< A recording.
    Receiver, ///< A receiver, live.
  };
  /** Where it comes from. */
  Origin origin = Origin::File;

  /** The recording (File). */
  QString path;
  /** Its format (Unknown: detected). */
  AdsbFormat format = AdsbFormat::Unknown;
  /**
   * The time of its first message, for formats timed by a receiver's
   * counter (Beast, AVR "@…;"), Unix seconds.
   */
  double startTimeS = 0.0;
  /** True if its SBS times are local time, false if UTC. */
  bool sbsLocalTime = true;
  /** Its columns (CSV). */
  CsvColumns csv;

  /** The receiver's host (Receiver). */
  QString host;
  /** Its port. */
  uint16_t port = 30003;
  /** What it sends. */
  ReceiverProtocol protocol = ReceiverProtocol::Sbs;
  /** The address of its aircraft.json (ReceiverProtocol::Json). */
  QUrl url;
  /** How often aircraft.json is fetched, seconds (at least 1). */
  double pollS = 1.0;
  /** A file the session is recorded to (empty: not recorded). */
  QString recordPath;

  /** Where the receiver is (decodes surface positions; limits range). */
  std::optional<CprPosition> receiverPosition;

  /** The source kind. */
  static QString kind() { return QStringLiteral("aircraft"); }
  /** Returns true if @p source is an aircraft layer's. */
  static bool isAircraft(const QJsonObject& source);
  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form; nothing if it isn't one. */
  static std::optional<AircraftSource> fromJson(const QJsonObject& json);
  /** Returns how a recording is read. */
  AdsbReadOptions readOptions() const;
  /** Returns a short description ("dump1090 at host:port", the file). */
  QString describe() const;
};

/** An aircraft's own settings, overriding its layer's. */
struct AircraftOverride {
  /** Its own trail, or the layer's. */
  std::optional<TrailSettings> trail;
  /** Returns true if it overrides nothing. */
  bool isEmpty() const { return !trail; }
  /** Equality. */
  bool operator==(const AircraftOverride& other) const
  {
    return trail == other.trail;
  }
};

/** A latitude and longitude box (an area filter). */
struct LatLonBox {
  /** Southern edge, degrees. */
  double southDeg = -90.0;
  /** Western edge, degrees. */
  double westDeg = -180.0;
  /** Northern edge, degrees. */
  double northDeg = 90.0;
  /** Eastern edge, degrees (less than west: across the antimeridian). */
  double eastDeg = 180.0;
  /** Returns true if it contains the point. */
  bool contains(double latDeg, double lonDeg) const;
  /** Equality. */
  bool operator==(const LatLonBox& other) const
  {
    return southDeg == other.southDeg && westDeg == other.westDeg &&
           northDeg == other.northDeg && eastDeg == other.eastDeg;
  }
};

/**
 * How an aircraft layer is drawn, and which aircraft it shows.
 *
 * @reentrant
 */
struct AircraftStyle {
  /** Markers' and labels' colour (and the trails' by aircraft). */
  QColor color = QColor(255, 220, 0);
  /** Markers' size (length and span), logical pixels. */
  double markerSizePx = 18.0;
  /** Callsigns (or addresses) beside the markers. */
  bool labels = true;
  /** The altitude in the label. */
  bool labelAltitude = false;
  /** The ground speed in the label. */
  bool labelSpeed = false;
  /** Lines down to the ground (globe). */
  bool dropLines = false;
  /** The trails. */
  TrailSettings trail;
  /** What the trails' colour shows. */
  TrailColouring colouring = TrailColouring::Altitude;
  /** When aircraft go stale and reports have gaps. */
  TrackTiming timing;

  /** Only callsigns containing this (case ignored; empty: all). */
  QString callsignFilter;
  /** Only addresses starting with this (hex; empty: all). */
  QString addressFilter;
  /** Only at or above this altitude, feet. */
  std::optional<double> minimumAltitudeFt;
  /** Only at or below this altitude, feet. */
  std::optional<double> maximumAltitudeFt;
  /** Only within this area. */
  std::optional<LatLonBox> area;

  /** Aircraft hidden by hand (by address). */
  std::set<uint32_t> hidden;
  /** Aircraft's own settings, by address. */
  std::map<uint32_t, AircraftOverride> overrides;

  /** Returns aircraft @p address's trail. */
  TrailSettings trailOf(uint32_t address) const;

  /**
   * Returns true if aircraft @p address, called @p callsign and at
   * @p at (its altitude in @p altitudeFt, NaN if none), passes the
   * filters and isn't hidden.
   */
  bool shows(uint32_t address, const QString& callsign, double latDeg,
             double lonDeg, double altitudeFt) const;

  /** Returns true if any filter is set. */
  bool isFiltered() const;

  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form; missing members keep their defaults. */
  static AircraftStyle fromJson(const QJsonObject& json);
  /** Equality. */
  bool operator==(const AircraftStyle& other) const;
  /** Inequality. */
  bool operator!=(const AircraftStyle& other) const
  {
    return !(*this == other);
  }
};

} // namespace ts

#endif // TS_ADSB_AIRCRAFTLAYER_H
