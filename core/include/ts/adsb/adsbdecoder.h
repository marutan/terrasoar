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
 * Turns Mode S messages from one receiver into aircraft reports, pairing
 * and decoding their positions.
 */
#ifndef TS_ADSB_ADSBDECODER_H
#define TS_ADSB_ADSBDECODER_H

#include <cstdint>
#include <optional>
#include <unordered_map>

#include <QByteArray>

#include "ts/adsb/aircraftreport.h"
#include "ts/adsb/cpr.h"
#include "ts/adsb/modesmessage.h"

namespace ts
{

/** How an AdsbDecoder decodes. */
struct AdsbDecoderSettings {
  /**
   * Where the receiver is, if known: it resolves surface positions, and
   * airborne positions decoded farther than maximumRangeNm from it are
   * refused.
   */
  std::optional<CprPosition> receiver;
  /** Farthest an aircraft is believed from the receiver, nautical miles. */
  double maximumRangeNm = 450.0;
  /** Longest between an airborne even and odd frame for a pair, seconds. */
  double airbornePairS = 10.0;
  /** Longest between a surface even and odd frame for a pair, seconds. */
  double surfacePairS = 25.0;
  /** Longest a position is used to decode the next one locally, seconds. */
  double localReferenceS = 300.0;
  /**
   * How long an address heard with a checked parity (DF 11, 17, 18) is
   * believed in messages whose parity is the address, seconds.
   */
  double knownAddressS = 60.0;
  /** Fastest an aircraft is believed to move, knots. */
  double maximumSpeedKt = 2000.0;
};

/** Counts of what an AdsbDecoder was given. */
struct AdsbDecoderCounts {
  /** Messages given. */
  int64_t messages = 0;
  /** Messages refused: wrong length, failed parity, not read. */
  int64_t refused = 0;
  /** Messages from addresses not known (their parity can't be checked). */
  int64_t unknownAddress = 0;
  /** Positions decoded. */
  int64_t positions = 0;
  /** Positions refused as impossible (too far, too fast). */
  int64_t positionsRefused = 0;
};

/**
 * Decodes Mode S messages from one receiver, in time order, into aircraft
 * reports. It keeps each aircraft's last even and odd position frames,
 * its last position (to decode the next locally), and the difference
 * between its GNSS and barometric altitudes.
 *
 * A position is decoded locally from the aircraft's last position, if it
 * is recent and the result is within reach of it; otherwise globally,
 * from an even and an odd frame close enough in time.
 *
 * @reentrant
 */
class AdsbDecoder
{
public:
  /** Creates a decoder with @p settings. */
  explicit AdsbDecoder(const AdsbDecoderSettings& settings = {});

  /** Returns the settings. */
  const AdsbDecoderSettings& settings() const { return m_settings; }

  /**
   * Decodes @p message (bytes) received at @p timeS (Unix seconds).
   * Returns the report, or nothing if it said nothing usable.
   */
  std::optional<AircraftReport> decode(const QByteArray& message, double timeS);

  /** The same, for a message already decoded. */
  std::optional<AircraftReport> decode(const ModeSMessage& message,
                                       double timeS);

  /** Returns the counts so far. */
  const AdsbDecoderCounts& counts() const { return m_counts; }

  /** Forgets every aircraft (as when a connection is made again). */
  void clear();

private:
  struct Frame {
    CprFrame cpr;
    double timeS = 0.0;
  };
  struct Aircraft {
    std::optional<Frame> even;
    std::optional<Frame> odd;
    std::optional<CprPosition> position;
    double positionTimeS = 0.0;
    double heardS = -1e300;
    std::optional<int32_t> gnssMinusBaroFt;
    double gnssMinusBaroTimeS = 0.0;
  };

  std::optional<CprPosition> decodePosition(Aircraft& aircraft, CprFrame frame,
                                            double timeS);
  bool withinRange(CprPosition position) const;

  AdsbDecoderSettings m_settings;
  std::unordered_map<uint32_t, Aircraft> m_aircraft;
  AdsbDecoderCounts m_counts;
};

/** Returns the distance between two positions, nautical miles. */
double distanceNm(CprPosition a, CprPosition b);

} // namespace ts

#endif // TS_ADSB_ADSBDECODER_H
