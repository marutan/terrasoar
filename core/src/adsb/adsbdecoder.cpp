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
#include "ts/adsb/adsbdecoder.h"

#include <algorithm>
#include <cmath>

#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** The Earth's mean radius in nautical miles. */
constexpr double earthRadiusNm = 6371008.8 / 1852.0;

} // namespace

double distanceNm(CprPosition a, CprPosition b)
{
  const double lat1 = degToRad(a.latDeg);
  const double lat2 = degToRad(b.latDeg);
  const double dLat = lat2 - lat1;
  const double dLon = degToRad(b.lonDeg - a.lonDeg);
  const double h = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
                   std::cos(lat1) * std::cos(lat2) * std::sin(dLon / 2.0) *
                       std::sin(dLon / 2.0);
  return 2.0 * earthRadiusNm * std::asin(std::sqrt(std::min(h, 1.0)));
}

AdsbDecoder::AdsbDecoder(const AdsbDecoderSettings& settings)
    : m_settings(settings)
{
}

void AdsbDecoder::clear()
{
  m_aircraft.clear();
}

std::optional<AircraftReport> AdsbDecoder::decode(const QByteArray& message,
                                                  double timeS)
{
  const std::optional<ModeSMessage> decoded = decodeModeS(message);
  if (!decoded) {
    ++m_counts.messages;
    ++m_counts.refused;
    return std::nullopt;
  }
  return decode(*decoded, timeS);
}

bool AdsbDecoder::withinRange(CprPosition position) const
{
  return !m_settings.receiver || distanceNm(position, *m_settings.receiver) <=
                                     m_settings.maximumRangeNm;
}

std::optional<CprPosition>
AdsbDecoder::decodePosition(Aircraft& aircraft, CprFrame frame, double timeS)
{
  // Locally from where it last was, if that is recent.
  if (aircraft.position &&
      timeS - aircraft.positionTimeS <= m_settings.localReferenceS) {
    const CprPosition local = cprLocal(frame, *aircraft.position);
    const double reachNm = m_settings.maximumSpeedKt *
                               std::fabs(timeS - aircraft.positionTimeS) /
                               3600.0 +
                           2.0;
    if (distanceNm(local, *aircraft.position) <= reachNm) {
      return local;
    }
    // Too far: the last position (or this frame) was wrong. Start again.
    aircraft.position.reset();
  }
  const std::optional<Frame>& other = frame.odd ? aircraft.even : aircraft.odd;
  if (!other || other->cpr.surface != frame.surface) {
    return std::nullopt;
  }
  const double window =
      frame.surface ? m_settings.surfacePairS : m_settings.airbornePairS;
  if (std::fabs(timeS - other->timeS) > window) {
    return std::nullopt;
  }
  const CprFrame& even = frame.odd ? other->cpr : frame;
  const CprFrame& odd = frame.odd ? frame : other->cpr;
  if (frame.surface) {
    const std::optional<CprPosition> reference =
        aircraft.position ? aircraft.position : m_settings.receiver;
    if (!reference) {
      return std::nullopt;
    }
    return cprGlobalSurface(even, odd, frame.odd, *reference);
  }
  return cprGlobalAirborne(even, odd, frame.odd);
}

std::optional<AircraftReport> AdsbDecoder::decode(const ModeSMessage& message,
                                                  double timeS)
{
  ++m_counts.messages;
  if (message.addressFromParity) {
    const std::unordered_map<uint32_t, Aircraft>::const_iterator known =
        m_aircraft.find(message.address);
    if (known == m_aircraft.end() ||
        timeS - known->second.heardS > m_settings.knownAddressS) {
      ++m_counts.unknownAddress;
      return std::nullopt;
    }
  }
  Aircraft& aircraft = m_aircraft[message.address];
  if (!message.addressFromParity) {
    aircraft.heardS = timeS;
  }
  AircraftReport report;
  report.timeS = timeS;
  report.address = message.address;
  if (message.altitudeFt) {
    report.baroAltitudeFt = *message.altitudeFt;
  }
  if (message.gnssHeightM) {
    report.geometricAltitudeFt = *message.gnssHeightM / feetToMetres;
  }
  if (message.gnssMinusBaroFt) {
    aircraft.gnssMinusBaroFt = message.gnssMinusBaroFt;
    aircraft.gnssMinusBaroTimeS = timeS;
  }
  if (message.cpr) {
    const CprFrame& frame = *message.cpr;
    std::optional<CprPosition> position =
        decodePosition(aircraft, frame, timeS);
    (frame.odd ? aircraft.odd : aircraft.even) = Frame{frame, timeS};
    if (position && !withinRange(*position)) {
      position.reset();
      ++m_counts.positionsRefused;
    }
    if (position) {
      aircraft.position = position;
      aircraft.positionTimeS = timeS;
      report.position = position;
      ++m_counts.positions;
    }
    // Geometric from barometric with a recent difference.
    if (report.baroAltitudeFt && aircraft.gnssMinusBaroFt &&
        timeS - aircraft.gnssMinusBaroTimeS <= 60.0) {
      report.geometricAltitudeFt =
          *report.baroAltitudeFt + *aircraft.gnssMinusBaroFt;
    }
  }
  report.groundSpeedKt = message.groundSpeedKt;
  report.trackDeg = message.trackDeg;
  if (message.verticalRateFtMin) {
    report.verticalRateFtMin = *message.verticalRateFtMin;
  }
  report.callsign = message.callsign;
  report.squawk = message.squawk;
  report.category = message.category;
  report.emergency = message.emergency;
  // DF 11 and 17's capability say less than the positions; keep it only
  // with something else.
  if (report.hasData()) {
    report.onGround = message.onGround;
    return report;
  }
  return std::nullopt;
}

} // namespace ts
