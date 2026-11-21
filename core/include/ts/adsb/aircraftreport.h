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
 * What is known of an aircraft at one time, whatever the source: decoded
 * messages, SBS lines, JSON snapshots or CSV rows.
 */
#ifndef TS_ADSB_AIRCRAFTREPORT_H
#define TS_ADSB_AIRCRAFTREPORT_H

#include <cstdint>
#include <optional>

#include <QString>

#include "ts/adsb/cpr.h"
#include "ts/adsb/modesmessage.h"

namespace ts
{

/** Feet in a metre's units: metres = feet × feetToMetres. */
constexpr double feetToMetres = 0.3048;

/** Metres a second in a knot. */
constexpr double knotsToMetresPerSecond = 1852.0 / 3600.0;

/**
 * One report of an aircraft: its address, the time, and what was heard
 * then. Fields not heard are empty.
 *
 * @reentrant
 */
struct AircraftReport {
  /** When, seconds since 1970-01-01T00:00Z (UTC). */
  double timeS = 0.0;
  /** The 24-bit ICAO address. */
  uint32_t address = 0;
  /** Where it was. */
  std::optional<CprPosition> position;
  /** Barometric (pressure) altitude, feet. */
  std::optional<double> baroAltitudeFt;
  /** Geometric altitude above the WGS 84 ellipsoid, feet. */
  std::optional<double> geometricAltitudeFt;
  /** Ground speed, knots. */
  std::optional<double> groundSpeedKt;
  /** Track over the ground, degrees from true north. */
  std::optional<double> trackDeg;
  /** Vertical rate, feet a minute (up positive). */
  std::optional<double> verticalRateFtMin;
  /** Callsign. */
  std::optional<QString> callsign;
  /** Mode A code (squawk). */
  std::optional<QString> squawk;
  /** Registration, from sources that know it. */
  std::optional<QString> registration;
  /** ICAO aircraft type designator, from sources that know it. */
  std::optional<QString> aircraftType;
  /** Emitter category ("A3"). */
  std::optional<QString> category;
  /** Emergency or priority status. */
  std::optional<AdsbEmergency> emergency;
  /** True on the ground. */
  std::optional<bool> onGround;

  /** Returns true if it says anything beyond the address and time. */
  bool hasData() const;
};

/** Returns @p address as six upper-case hex digits ("4840D6"). */
QString formatIcaoAddress(uint32_t address);

/** Reads six hex digits (a leading '~' marks a non-ICAO address: none). */
std::optional<uint32_t> parseIcaoAddress(const QString& text);

} // namespace ts

#endif // TS_ADSB_AIRCRAFTREPORT_H
