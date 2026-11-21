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
 * Exporting an aircraft's track: KML (gx:Track), GPX or CSV.
 */
#ifndef TS_ADSB_AIRCRAFTEXPORT_H
#define TS_ADSB_AIRCRAFTEXPORT_H

#include <cstdint>

#include <QByteArray>
#include <QString>

#include "ts/adsb/aircrafttracks.h"

namespace ts
{

/** The formats a track is exported in. */
enum class TrackExportFormat : uint8_t {
  Kml, ///< KML 2.2 with a gx:Track (times, positions, heights).
  Gpx, ///< GPX 1.1, a track with times and elevations.
  Csv, ///< CSV, one row per report with every value.
};

/** Returns the format a file's suffix asks for (KML by default). */
TrackExportFormat trackExportFormatFor(const QString& path);

/**
 * Returns @p track in @p format, named @p name. Heights are written above
 * mean sea level where the formats mean that (KML's absolute altitudes,
 * GPX's elevations): the height above the ellipsoid less @p undulation's
 * geoid (none: the ellipsoid). The CSV has both, and the altitudes heard.
 */
QByteArray exportAircraftTrack(const AircraftTrack& track, const QString& name,
                               TrackExportFormat format,
                               const UndulationFunction& undulation = {});

} // namespace ts

#endif // TS_ADSB_AIRCRAFTEXPORT_H
