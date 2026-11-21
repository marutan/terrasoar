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
 * Aircraft tracks built from reports, and heights from altitudes.
 */
#ifndef TS_ADSB_AIRCRAFTTRACKS_H
#define TS_ADSB_AIRCRAFTTRACKS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QString>

#include "ts/adsb/aircraftreport.h"
#include "ts/tracks/movingtrack.h"

namespace ts
{

/**
 * Returns the geometric height above mean sea level, metres, of a
 * pressure altitude in the International Standard Atmosphere: the
 * pressure altitude is the ISA's geopotential height H, and the geometric
 * height is r H / (r − H) (r = 6 356 766 m). Exact only on a standard
 * day; real temperatures and pressures move it by up to a few hundred
 * metres, so heights from it are marked approximate.
 */
double pressureAltitudeToHeightM(double pressureAltitudeFt);

/**
 * Gives the geoid's height above the ellipsoid (mean sea level) at a
 * latitude and longitude, metres; NaN where not known (taken as 0).
 */
using UndulationFunction = std::function<double(double, double)>;

/** What an aircraft was called and squawked from a time on. */
struct AircraftInfo {
  /** From when, Unix seconds. */
  double timeS = 0.0;
  /** Callsign. */
  QString callsign;
  /** Mode A code. */
  QString squawk;
  /** Registration. */
  QString registration;
  /** ICAO type designator. */
  QString aircraftType;
  /** Emitter category. */
  QString category;
  /** Emergency status. */
  AdsbEmergency emergency = AdsbEmergency::None;
  /** True on the ground (if known). */
  std::optional<bool> onGround;

  /** Returns true if it says the same as @p other (ignoring the time). */
  bool sameAs(const AircraftInfo& other) const;
};

/** The altitudes heard with one report. */
struct AircraftAltitudes {
  /** Barometric altitude, feet; NaN if none. */
  float baroFt = std::numeric_limits<float>::quiet_NaN();
  /** Geometric altitude (above the ellipsoid), feet; NaN if none. */
  float geometricFt = std::numeric_limits<float>::quiet_NaN();
};

/**
 * One aircraft's track: its positions (with heights above the ellipsoid),
 * the altitudes heard with each, and what it was called when.
 *
 * @reentrant
 */
struct AircraftTrack {
  /** The ICAO address. */
  uint32_t address = 0;
  /** The positions, in time order. */
  MovingTrack track;
  /** The altitudes, one for each of track's points. */
  std::vector<AircraftAltitudes> altitudes;
  /** What it was called, in time order (changes only). */
  std::vector<AircraftInfo> info;

  /** Returns the info at @p timeS (the first if before it; null if none). */
  const AircraftInfo* infoAt(double timeS) const;
  /** Returns the last callsign it had, or an empty string. */
  QString lastCallsign() const;
};

/** How AircraftTrackBuilder makes heights. */
struct AircraftHeightSettings {
  /**
   * The geoid, for barometric altitudes (heights above sea level); null
   * or NaN: sea level is taken as the ellipsoid.
   */
  UndulationFunction undulation;
  /** Longest an altitude is carried to later positions, seconds. */
  double altitudeKeptS = 30.0;
};

/**
 * Builds aircraft tracks from reports. Reports may come in any order;
 * each position takes the altitudes, speed, track and vertical rate heard
 * last before it (within AircraftHeightSettings::altitudeKeptS), so
 * reports of one aircraft should come roughly in time order.
 *
 * Heights: a geometric altitude (above the ellipsoid) is exact; a
 * barometric one is turned into a height above sea level through the
 * standard atmosphere, then above the ellipsoid through the geoid, and
 * marked approximate. An aircraft on the ground with no altitude has no
 * height (it is drawn on the ground).
 *
 * @reentrant
 */
class AircraftTrackBuilder
{
public:
  /** Creates a builder making heights with @p settings. */
  explicit AircraftTrackBuilder(AircraftHeightSettings settings = {});

  /** Adds @p report. */
  void add(const AircraftReport& report);

  /** Returns the number of reports added. */
  int64_t reportCount() const { return m_reports; }

  /** Returns the number of positions so far. */
  int64_t positionCount() const { return m_positions; }

  /**
   * Returns the tracks, sorted and without duplicates, by address; the
   * builder is left empty.
   */
  std::vector<AircraftTrack> finish();

private:
  struct Point {
    TrackSample point;
    AircraftAltitudes altitudes;
  };
  struct Aircraft {
    std::vector<Point> points;
    std::vector<AircraftInfo> info;
    AircraftInfo current;
    std::optional<double> baroFt;
    double baroTimeS = 0.0;
    std::optional<double> geometricFt;
    double geometricTimeS = 0.0;
    std::optional<double> speedKt;
    std::optional<double> trackDeg;
    std::optional<double> verticalRateFtMin;
    double motionTimeS = 0.0;
  };

  static AircraftTrack build(uint32_t address, Aircraft&& aircraft);

  AircraftHeightSettings m_settings;
  std::unordered_map<uint32_t, Aircraft> m_aircraft;
  int64_t m_reports = 0;
  int64_t m_positions = 0;
};

} // namespace ts

#endif // TS_ADSB_AIRCRAFTTRACKS_H
