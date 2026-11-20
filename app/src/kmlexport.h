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
 * Saving KML from the windows that make it: asking where, writing, and
 * saying what went wrong.
 */
#ifndef TS_APP_KMLEXPORT_H
#define TS_APP_KMLEXPORT_H

#include <vector>

#include <QString>

#include "ts/kml/kmldocument.h"
#include "ts/providers/tileprovider.h"
#include "ts/regions/regiongeometry.h"

class QWidget;

namespace ts
{

/**
 * Asks where to save @p document (KML or KMZ, starting from
 * @p suggestedName) and saves it, warning in a message box titled
 * @p title if it can't be written or files it uses are missing. Returns
 * the path saved to, or empty if nothing was.
 */
QString saveKmlAs(QWidget* parent, const KmlDocument& document,
                  const QString& title, const QString& suggestedName);

/**
 * Writes @p document to @p path, warning in a message box over @p parent
 * titled @p title if it can't be written or files it uses are missing;
 * returns false if it wasn't written.
 */
bool saveKmlTo(QWidget* parent, const KmlDocument& document,
               const QString& path, const QString& title);

/** Returns a line along the ground through @p points. */
KmlGeometry kmlLineThrough(const std::vector<GeoPoint>& points);

/**
 * Returns @p region as a polygon on the ground: its first ring the
 * outside, the others holes; longitudes within ±180°.
 */
KmlGeometry kmlAreaOf(const RegionGeometry& region);

} // namespace ts

#endif // TS_APP_KMLEXPORT_H
