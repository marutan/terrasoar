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
 * Compact Position Reporting: decoding the positions of ADS-B messages.
 */
#ifndef TS_ADSB_CPR_H
#define TS_ADSB_CPR_H

#include <cstdint>
#include <optional>

#include "ts/adsb/modesmessage.h"

namespace ts
{

/** A decoded position. */
struct CprPosition {
  /** Latitude, degrees north. */
  double latDeg = 0.0;
  /** Longitude, degrees east, in [−180, 180). */
  double lonDeg = 0.0;
};

/**
 * Returns NL, the number of longitude zones at @p latDeg (59 at the
 * equator down to 1 beyond 87°), from the transition latitudes of
 * DO-260B.
 */
int32_t cprLongitudeZones(double latDeg);

/**
 * Decodes an airborne position from an even and an odd frame
 * (globally unambiguous), using the latitude of the newer (@p oddNewer).
 * Returns nothing if the two lie in different longitude zones (the
 * aircraft crossed a zone boundary between them) or the latitude is
 * impossible. The frames must be close in time (10 s).
 */
std::optional<CprPosition>
cprGlobalAirborne(const CprFrame& even, const CprFrame& odd, bool oddNewer);

/**
 * Decodes a surface position from an even and an odd frame. Surface
 * frames are ambiguous to a quarter of the globe: of the solutions, the
 * one nearest @p reference (the receiver, or where the aircraft was) is
 * returned.
 */
std::optional<CprPosition> cprGlobalSurface(const CprFrame& even,
                                            const CprFrame& odd, bool oddNewer,
                                            const CprPosition& reference);

/**
 * Decodes one frame near @p reference (locally unambiguous): correct if
 * the aircraft is within half a zone of it, 180 NM airborne or 45 NM on
 * the surface.
 */
CprPosition cprLocal(const CprFrame& frame, const CprPosition& reference);

} // namespace ts

#endif // TS_ADSB_CPR_H
