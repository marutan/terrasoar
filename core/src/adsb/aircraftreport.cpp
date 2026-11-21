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
#include "ts/adsb/aircraftreport.h"

namespace ts
{

bool AircraftReport::hasData() const
{
  return position || baroAltitudeFt || geometricAltitudeFt || groundSpeedKt ||
         trackDeg || verticalRateFtMin || callsign || squawk || registration ||
         aircraftType || category || emergency || onGround;
}

QString formatIcaoAddress(uint32_t address)
{
  return QString::number(address & 0xFFFFFFU, 16)
      .toUpper()
      .rightJustified(6, QLatin1Char('0'));
}

std::optional<uint32_t> parseIcaoAddress(const QString& text)
{
  const QString trimmed = text.trimmed();
  if (trimmed.size() != 6) {
    return std::nullopt;
  }
  bool ok = false;
  const uint32_t address = trimmed.toUInt(&ok, 16);
  if (!ok) {
    return std::nullopt;
  }
  return address;
}

} // namespace ts
