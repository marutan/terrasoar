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
 * Exporting a vessel's track: KML (gx:Track), GPX or CSV.
 */
#ifndef TS_AIS_VESSELEXPORT_H
#define TS_AIS_VESSELEXPORT_H

#include <QByteArray>
#include <QString>

#include "ts/adsb/aircraftexport.h"
#include "ts/ais/vesseltracks.h"

namespace ts
{

/**
 * Returns @p track in @p format (TrackExportFormat), named @p name: KML
 * and GPX at sea level (aircraft at their altitude), CSV with every value
 * of each report.
 */
QByteArray exportVesselTrack(const VesselTrack& track, const QString& name,
                             TrackExportFormat format);

} // namespace ts

#endif // TS_AIS_VESSELEXPORT_H
