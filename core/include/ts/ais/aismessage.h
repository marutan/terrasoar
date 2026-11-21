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
 * AIS (ITU-R M.1371) messages from NMEA 0183 sentences: checksums, tag
 * blocks, multi-sentence messages, 6-bit payloads and the message types
 * Terra Soar reads.
 */
#ifndef TS_AIS_AISMESSAGE_H
#define TS_AIS_AISMESSAGE_H

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

namespace ts
{

/** One NMEA AIS sentence (!AIVDM, !AIVDO, or another talker's). */
struct AisSentence {
  /** Fragments in the message (1 to 9). */
  int32_t count = 1;
  /** This fragment's number (1 to count). */
  int32_t number = 1;
  /** The sequential message id joining fragments (−1: none). */
  int32_t sequence = -1;
  /** The radio channel ('A', 'B', '1', '2', or empty). */
  QByteArray channel;
  /** The armoured payload. */
  QByteArray payload;
  /** Fill bits at the end of the payload (0 to 5). */
  int32_t fillBits = 0;
  /** True for VDO (the own vessel), false for VDM. */
  bool own = false;
  /** The tag block's time (c:), Unix seconds, if it had one. */
  std::optional<double> tagTimeS;
  /** The tag block's source (s:), if it had one. */
  QByteArray tagSource;
};

/** Why a line isn't a sentence. */
enum class AisSentenceError : uint8_t {
  None,     ///< It is one.
  NotAis,   ///< Not an AIS sentence (another NMEA sentence, or text).
  Checksum, ///< Its checksum is wrong.
  Broken,   ///< Its fields can't be read.
};

/**
 * Reads one line: an optional NMEA 4.0 tag block (\\c:…,s:…*hh\\), then
 * "!xxVDM" or "!xxVDO" with its checksum. @p error says why if not.
 */
std::optional<AisSentence> parseAisSentence(const QByteArray& line,
                                            AisSentenceError* error = nullptr);

/** Returns the NMEA checksum (exclusive or) of @p text. */
uint8_t nmeaChecksum(const QByteArray& text);

/**
 * Joins the fragments of multi-sentence messages, in any order and
 * interleaved, by sequence id and channel. Fragments of a message not
 * completed within maximumPending later sentences are dropped.
 *
 * @reentrant
 */
class AisAssembler
{
public:
  /** A whole message's payload. */
  struct Payload {
    /** The armoured payload, fragments joined. */
    QByteArray payload;
    /** The last fragment's fill bits. */
    int32_t fillBits = 0;
    /** The first fragment's tag time, if any. */
    std::optional<double> tagTimeS;
    /** True for VDO. */
    bool own = false;
  };

  /**
   * Adds @p sentence; returns the message it completes (at once for one
   * of one).
   */
  std::optional<Payload> add(const AisSentence& sentence);

  /** Returns how many incomplete messages were dropped. */
  int64_t dropped() const { return m_dropped; }

  /** Sentences after which an incomplete message is dropped. */
  static constexpr int64_t maximumPending = 64;

private:
  struct Partial {
    std::vector<QByteArray> fragments;
    int32_t fillBits = 0;
    std::optional<double> tagTimeS;
    bool own = false;
    int64_t started = 0;
  };
  std::map<QByteArray, Partial> m_partial;
  int64_t m_count = 0;
  int64_t m_dropped = 0;
};

/** The navigational status of types 1–3 and 27. */
enum class AisNavStatus : uint8_t {
  UnderWayEngine = 0,       ///< Under way using engine.
  AtAnchor = 1,             ///< At anchor.
  NotUnderCommand = 2,      ///< Not under command.
  RestrictedManoeuvre = 3,  ///< Restricted manoeuvrability.
  ConstrainedByDraught = 4, ///< Constrained by her draught.
  Moored = 5,               ///< Moored.
  Aground = 6,              ///< Aground.
  Fishing = 7,              ///< Engaged in fishing.
  UnderWaySailing = 8,      ///< Under way sailing.
  AisSart = 14,             ///< AIS-SART (search and rescue transmitter).
  NotDefined = 15,          ///< Not defined (the default).
};

/** The size and the antenna's place: distances from it, metres. */
struct AisDimensions {
  /** To the bow. */
  int32_t toBow = 0;
  /** To the stern. */
  int32_t toStern = 0;
  /** To port. */
  int32_t toPort = 0;
  /** To starboard. */
  int32_t toStarboard = 0;
  /** Returns true if any is known (not all zero). */
  bool isKnown() const
  {
    return toBow + toStern > 0 && toPort + toStarboard > 0;
  }
  /** Equality. */
  bool operator==(AisDimensions other) const
  {
    return toBow == other.toBow && toStern == other.toStern &&
           toPort == other.toPort && toStarboard == other.toStarboard;
  }
};

/** An estimated time of arrival, as type 5 gives it (UTC, no year). */
struct AisEta {
  /** Month, 1–12. */
  int32_t month = 0;
  /** Day, 1–31. */
  int32_t day = 0;
  /** Hour, 0–23. */
  int32_t hour = 0;
  /** Minute, 0–59. */
  int32_t minute = 0;
};

/**
 * What one AIS message says. Values the message doesn't carry, or marks
 * "not available" (heading 511, speed 1023, longitude 181, latitude 91,
 * course 3600, rate of turn −128, …), are empty.
 *
 * @reentrant
 */
struct AisMessage {
  /** The message type, 1 to 27. */
  int32_t type = 0;
  /** The MMSI. */
  uint32_t mmsi = 0;
  /** Latitude, degrees. */
  std::optional<double> latDeg;
  /** Longitude, degrees. */
  std::optional<double> lonDeg;
  /** Speed over ground, knots. */
  std::optional<double> speedKn;
  /** Course over ground, degrees from true north. */
  std::optional<double> courseDeg;
  /** True heading, degrees. */
  std::optional<int32_t> headingDeg;
  /** Rate of turn, degrees a minute (right positive). */
  std::optional<double> turnDegMin;
  /** Navigational status. */
  std::optional<AisNavStatus> status;
  /** True if the position is accurate to 10 m (DGNSS). */
  bool accurate = false;
  /** The UTC second of the report (0–59), if given. */
  std::optional<int32_t> second;
  /** Altitude, metres (type 9, search-and-rescue aircraft). */
  std::optional<int32_t> altitudeM;
  /** Name (vessel, or aid to navigation with its extension). */
  std::optional<QString> name;
  /** Callsign. */
  std::optional<QString> callsign;
  /** IMO number. */
  std::optional<uint32_t> imo;
  /** Ship and cargo type (0–99). */
  std::optional<int32_t> shipType;
  /** Dimensions and the antenna's place. */
  std::optional<AisDimensions> dimensions;
  /** Draught, metres. */
  std::optional<double> draughtM;
  /** Destination. */
  std::optional<QString> destination;
  /** Estimated time of arrival. */
  std::optional<AisEta> eta;
  /** Aid to navigation type (type 21), 0–31. */
  std::optional<int32_t> aidType;
  /** Type 24's part (0: A, name; 1: B, type, callsign, size). */
  std::optional<int32_t> part;
  /** Type 4's UTC date and time, as written (y, m, d, h, min, s). */
  std::optional<std::vector<int32_t>> utc;

  /** Returns true for Class B messages (18, 19, 24). */
  bool isClassB() const { return type == 18 || type == 19 || type == 24; }
};

/**
 * Returns the bits of an armoured payload (6 per character), less
 * @p fillBits at the end; empty if a character isn't in the armour.
 */
std::vector<bool> aisPayloadBits(const QByteArray& payload, int32_t fillBits);

/**
 * Decodes a whole message's payload. Returns nothing if it is too short
 * for its type's fixed fields, or a type Terra Soar doesn't read (@p type
 * is set to the type, if not null, so others can be counted).
 */
std::optional<AisMessage> decodeAis(const QByteArray& payload, int32_t fillBits,
                                    int32_t* type = nullptr);

} // namespace ts

#endif // TS_AIS_AISMESSAGE_H
