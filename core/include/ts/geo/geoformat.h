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
 * Coordinates and distances as text for the user.
 */
#ifndef TS_GEO_GEOFORMAT_H
#define TS_GEO_GEOFORMAT_H

#include <QLocale>
#include <QString>

namespace ts
{

/**
 * Returns @p latDeg as text, e.g. "51.47690° N": five decimals (about a
 * metre), the hemisphere letter translated.
 *
 * @reentrant
 */
QString formatLatitude(double latDeg, const QLocale& locale);

/**
 * Returns @p latDeg rounded to whole degrees as text, e.g. "51° N", for
 * where a rough latitude is enough (the scale bar).
 *
 * @reentrant
 */
QString formatWholeLatitude(double latDeg, const QLocale& locale);

/**
 * Returns @p lonDeg (any value; normalised to [−180, 180)) as text, e.g.
 * "0.00050° W".
 *
 * @reentrant
 */
QString formatLongitude(double lonDeg, const QLocale& locale);

/**
 * Returns @p distanceM as text with a unit that suits its size: "8.5 m",
 * "850 m", "12.3 km", "12,345 km".
 *
 * @reentrant
 */
QString formatDistance(double distanceM, const QLocale& locale);

/**
 * Returns a round distance as text without decimals, in the largest unit
 * that keeps it a whole number: "20 cm", "500 m", "2,000 km". For scale
 * bars; @p distanceM is rounded to whole centimetres.
 *
 * @reentrant
 */
QString formatRoundDistance(double distanceM, const QLocale& locale);

} // namespace ts

#endif // TS_GEO_GEOFORMAT_H
