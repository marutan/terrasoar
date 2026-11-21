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
 * Mode S and ADS-B (1090 MHz Extended Squitter) messages: the parity
 * check and the fields Terra Soar uses.
 */
#ifndef TS_ADSB_MODESMESSAGE_H
#define TS_ADSB_MODESMESSAGE_H

#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QString>

namespace ts
{

/** An aircraft's emergency or priority status (ADS-B type code 28). */
enum class AdsbEmergency : uint8_t {
  None = 0,                 ///< No emergency.
  General = 1,              ///< General emergency.
  Medical = 2,              ///< Lifeguard or medical.
  MinimumFuel = 3,          ///< Minimum fuel.
  NoCommunications = 4,     ///< No communications.
  UnlawfulInterference = 5, ///< Unlawful interference.
  Downed = 6,               ///< Downed aircraft.
  Reserved = 7,             ///< Reserved.
};

/**
 * One CPR-encoded position (Compact Position Reporting): 17 bits of
 * latitude and of longitude within a zone, an even or odd frame, airborne
 * or on the surface.
 *
 * @reentrant
 */
struct CprFrame {
  /** Latitude within its zone, 0 to 2¹⁷ − 1. */
  uint32_t lat = 0;
  /** Longitude within its zone, 0 to 2¹⁷ − 1. */
  uint32_t lon = 0;
  /** True for an odd frame, false for an even one. */
  bool odd = false;
  /** True for a surface position (zones a quarter the size). */
  bool surface = false;
};

/**
 * What one Mode S message says. Fields the message doesn't carry are
 * empty.
 *
 * @reentrant
 */
struct ModeSMessage {
  /** The downlink format (DF), 0 to 24. */
  int32_t downlinkFormat = 0;
  /** The ADS-B type code (DF 17/18), or 0. */
  int32_t typeCode = 0;
  /** The aircraft's 24-bit ICAO address. */
  uint32_t address = 0;
  /**
   * True if the address is the parity's remainder (DF 0, 4, 5, 16, 20,
   * 21), which can't be checked on its own: a corrupt message gives a
   * wrong address. Use it only for an aircraft already known.
   */
  bool addressFromParity = false;
  /** Barometric (pressure) altitude, feet. */
  std::optional<int32_t> altitudeFt;
  /** GNSS height above the WGS 84 ellipsoid (type codes 20–22), metres. */
  std::optional<int32_t> gnssHeightM;
  /** The callsign, without trailing spaces. */
  std::optional<QString> callsign;
  /** The emitter category ("A3": set A, category 3), if given. */
  std::optional<QString> category;
  /** The position, CPR-encoded. */
  std::optional<CprFrame> cpr;
  /** Ground speed, knots. */
  std::optional<double> groundSpeedKt;
  /** Track over the ground, degrees from true north. */
  std::optional<double> trackDeg;
  /** Heading, degrees from magnetic north (airspeed velocities). */
  std::optional<double> headingDeg;
  /** Airspeed, knots. */
  std::optional<double> airspeedKt;
  /** True for true airspeed, false for indicated. */
  bool trueAirspeed = false;
  /** Vertical rate, feet a minute (up positive). */
  std::optional<int32_t> verticalRateFtMin;
  /** True if the vertical rate is barometric, false if GNSS. */
  bool verticalRateBaro = false;
  /** GNSS altitude less the barometric, feet. */
  std::optional<int32_t> gnssMinusBaroFt;
  /** The Mode A code (squawk), four octal digits. */
  std::optional<QString> squawk;
  /** Emergency or priority status. */
  std::optional<AdsbEmergency> emergency;
  /** True on the ground, false airborne, if the message says. */
  std::optional<bool> onGround;
};

/**
 * Returns the parity remainder of @p message (7 or 14 bytes): the CRC
 * (generator 0x1FFF409) of all but its last 24 bits, exclusive-ored with
 * them. It is 0 for a correct DF 11 (or the interrogator's code), 17 or
 * 18; for DF 0, 4, 5, 16, 20 and 21 it is the address.
 */
uint32_t modeSParity(const QByteArray& message);

/**
 * Returns the bytes of a message written in hex (14 or 28 digits), or
 * an empty array if it isn't one.
 */
QByteArray modeSFromHex(const QByteArray& hex);

/**
 * Decodes @p bytes (7 or 14). Returns nothing if it is neither
 * length for its downlink format, or fails its parity (DF 11, 17, 18),
 * or is a format or ADS-B type Terra Soar doesn't read.
 */
std::optional<ModeSMessage> decodeModeS(const QByteArray& bytes);

/**
 * Returns the altitude in feet from a 13-bit altitude code (with the M
 * and Q bits, as in DF 0, 4, 16 and 20), or nothing if not available.
 */
std::optional<int32_t> decodeAltitudeCode(uint32_t code13);

/** Returns the Mode A code (four octal digits) from a 13-bit identity. */
QString decodeIdentity(uint32_t code13);

/**
 * Returns the ground speed in knots from a surface position's movement
 * field (1–124), or nothing (0, and the reserved 125–127).
 */
std::optional<double> decodeSurfaceMovementKt(uint32_t movement);

} // namespace ts

#endif // TS_ADSB_MODESMESSAGE_H
