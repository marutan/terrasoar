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
 * A satellite seen from a place on the ground: its direction and
 * distance, and its next pass overhead.
 */
#ifndef TS_ORBITS_SATELLITEPASSES_H
#define TS_ORBITS_SATELLITEPASSES_H

#include <optional>

#include "ts/geo/geodetic.h"
#include "ts/maths/vec3.h"
#include "ts/orbits/orbitalelements.h"
#include "ts/orbits/sgp4.h"

namespace ts
{

/** Where something in the sky is, seen from a place. */
struct LookAngles {
  /** Azimuth: degrees clockwise from north, [0, 360). */
  double azimuthDeg = 0.0;
  /** Elevation above the horizon (the ellipsoid's tangent plane), degrees. */
  double elevationDeg = 0.0;
  /** Distance, kilometres. */
  double rangeKm = 0.0;
};

/** Returns where @p targetEcefM (ECEF metres) is seen from @p observer. */
LookAngles lookAngles(const Geodetic& observer, const Vec3& targetEcefM);

/** One pass of a satellite over a place: from rising to setting. */
struct SatellitePass {
  /** When it rises above the minimum elevation. */
  JulianDate rise;
  /** When it is highest. */
  JulianDate culmination;
  /** When it sets below the minimum elevation. */
  JulianDate set;
  /** Its highest elevation, degrees. */
  double maxElevationDeg = 0.0;
  /** Azimuth at rising, degrees. */
  double riseAzimuthDeg = 0.0;
  /** Azimuth at setting, degrees. */
  double setAzimuthDeg = 0.0;
  /** True if it was already up at the time searched from (rise before). */
  bool inProgress = false;
  /**
   * True if it never sets within the search (geostationary satellites):
   * rise and set are then the ends of the search.
   */
  bool alwaysUp = false;
};

/**
 * Returns the next pass of @p sgp4 over @p observer above
 * @p minElevationDeg, searching @p searchDays from @p from; the pass under
 * way if it is up at @p from. Times are found to about a second. Nothing
 * if it doesn't rise within the search.
 */
std::optional<SatellitePass> nextPass(const Sgp4& sgp4,
                                      const Geodetic& observer, JulianDate from,
                                      double minElevationDeg = 0.0,
                                      double searchDays = 3.0);

} // namespace ts

#endif // TS_ORBITS_SATELLITEPASSES_H
