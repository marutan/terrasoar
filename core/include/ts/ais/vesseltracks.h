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
 * Vessels from AIS: reports whatever their source, tracks built from
 * them, and how long a vessel stays shown without one.
 */
#ifndef TS_AIS_VESSELTRACKS_H
#define TS_AIS_VESSELTRACKS_H

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include <QString>

#include "ts/ais/aismessage.h"
#include "ts/tracks/movingtrack.h"

namespace ts
{

/** What an AIS station is. */
enum class AisStationKind : uint8_t {
  Vessel,          ///< A vessel (Class A or B).
  BaseStation,     ///< A base station (type 4).
  AidToNavigation, ///< An aid to navigation (type 21).
  SarAircraft,     ///< A search-and-rescue aircraft (type 9).
};

/**
 * One report of an AIS station: its MMSI, the time, and what was heard
 * then. Fields not heard are empty.
 *
 * @reentrant
 */
struct VesselReport {
  /** When, Unix seconds (UTC). */
  double timeS = 0.0;
  /** The MMSI. */
  uint32_t mmsi = 0;
  /** What it is. */
  AisStationKind kind = AisStationKind::Vessel;
  /** True for Class B (types 18, 19, 24). */
  std::optional<bool> classB;
  /** Latitude, degrees. */
  std::optional<double> latDeg;
  /** Longitude, degrees. */
  std::optional<double> lonDeg;
  /** Speed over ground, knots. */
  std::optional<double> speedKn;
  /** Course over ground, degrees. */
  std::optional<double> courseDeg;
  /** True heading, degrees. */
  std::optional<double> headingDeg;
  /** Rate of turn, degrees a minute. */
  std::optional<double> turnDegMin;
  /** Altitude, metres (search-and-rescue aircraft). */
  std::optional<double> altitudeM;
  /** Navigational status. */
  std::optional<AisNavStatus> status;
  /** Name. */
  std::optional<QString> name;
  /** Callsign. */
  std::optional<QString> callsign;
  /** IMO number. */
  std::optional<uint32_t> imo;
  /** Ship and cargo type. */
  std::optional<int32_t> shipType;
  /** Dimensions and the antenna's place. */
  std::optional<AisDimensions> dimensions;
  /** Draught, metres. */
  std::optional<double> draughtM;
  /** Destination. */
  std::optional<QString> destination;
  /** Estimated time of arrival. */
  std::optional<AisEta> eta;
  /** Aid to navigation type. */
  std::optional<int32_t> aidType;

  /** Returns true if it has a position. */
  bool hasPosition() const { return latDeg && lonDeg; }
  /** Returns true if it says anything beyond the MMSI and time. */
  bool hasData() const;
};

/** Returns the report of @p message heard at @p timeS. */
VesselReport vesselReport(const AisMessage& message, double timeS);

/** What a vessel was called and what it is, from a time on. */
struct VesselInfo {
  /** From when, Unix seconds. */
  double timeS = 0.0;
  /** What it is. */
  AisStationKind kind = AisStationKind::Vessel;
  /** True for Class B. */
  bool classB = false;
  /** Name. */
  QString name;
  /** Callsign. */
  QString callsign;
  /** IMO number (0: none). */
  uint32_t imo = 0;
  /** Ship type (0: not known). */
  int32_t shipType = 0;
  /** Dimensions (all zero: not known). */
  AisDimensions dimensions;
  /** Draught, metres (0: not known). */
  double draughtM = 0.0;
  /** Destination. */
  QString destination;
  /** ETA, if given. */
  std::optional<AisEta> eta;
  /** Navigational status. */
  AisNavStatus status = AisNavStatus::NotDefined;
  /** Aid to navigation type (−1: not one). */
  int32_t aidType = -1;
  /** The time of the last static report (type 5, 19, 24), 0 if none. */
  double staticTimeS = 0.0;

  /** Returns true if it says the same as @p other (ignoring times). */
  bool sameAs(const VesselInfo& other) const;
};

/** A vessel's heading and rate of turn with each position. */
struct VesselMotion {
  /** True heading, degrees; NaN if not given. */
  float headingDeg = std::numeric_limits<float>::quiet_NaN();
  /** Rate of turn, degrees a minute; NaN if not given. */
  float turnDegMin = std::numeric_limits<float>::quiet_NaN();
};

/**
 * One station's track: its positions (course and speed in each sample,
 * heights only for aircraft), the heading and rate of turn with each, and
 * what it was called when.
 *
 * @reentrant
 */
struct VesselTrack {
  /** The MMSI. */
  uint32_t mmsi = 0;
  /** The positions, in time order. */
  MovingTrack track;
  /** The headings and rates of turn, one for each of track's points. */
  std::vector<VesselMotion> motion;
  /** What it was called, in time order (changes only). */
  std::vector<VesselInfo> info;

  /** Returns the info at @p timeS (the first if before it; null if none). */
  const VesselInfo* infoAt(double timeS) const;
  /** Returns its last name, or an empty string. */
  QString lastName() const;
};

/**
 * Returns how long a station is shown after its last report, seconds:
 * ten of its reporting intervals (ITU-R M.1371: Class A every 2–10 s
 * under way by speed, 3 minutes at anchor or moored; Class B every 30 s,
 * 3 minutes below 2 knots), at least @p minimumS. Base stations and aids
 * to navigation report every few minutes: an hour.
 */
double vesselStaleS(const VesselInfo& info, double speedKn,
                    double minimumS = 360.0);

/**
 * Builds vessel tracks from reports (roughly in time order per station,
 * as AircraftTrackBuilder). Positions take the speed, course, heading and
 * rate of turn of their own report.
 *
 * @reentrant
 */
class VesselTrackBuilder
{
public:
  /** Adds @p report. */
  void add(const VesselReport& report);

  /** Returns the number of reports added. */
  int64_t reportCount() const { return m_reports; }

  /** Returns the tracks, sorted, by MMSI; the builder is left empty. */
  std::vector<VesselTrack> finish();

  /** Returns the tracks of the stations with reports since the last call. */
  std::vector<VesselTrack> buildChanged();

  /** Forgets stations with no report since @p timeS. */
  void forgetBefore(double timeS);

private:
  struct Point {
    TrackSample sample;
    VesselMotion motion;
  };
  struct Station {
    std::vector<Point> points;
    std::vector<VesselInfo> info;
    VesselInfo current;
    double lastS = 0.0;
  };

  static VesselTrack build(uint32_t mmsi, Station station);

  std::unordered_map<uint32_t, Station> m_stations;
  std::set<uint32_t> m_changed;
  int64_t m_reports = 0;
};

} // namespace ts

#endif // TS_AIS_VESSELTRACKS_H
