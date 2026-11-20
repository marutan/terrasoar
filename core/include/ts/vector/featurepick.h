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
 * Whether a vector feature lies under a point, for picking.
 */
#ifndef TS_VECTOR_FEATUREPICK_H
#define TS_VECTOR_FEATUREPICK_H

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * Returns true if @p feature lies under @p point: a polygon containing it
 * (not in a hole), a line or point within @p toleranceDeg (degrees of
 * latitude; longitude scaled by the latitude).
 *
 * @reentrant
 */
bool featureUnder(const Feature& feature, GeoPoint point, double toleranceDeg);

} // namespace ts

#endif // TS_VECTOR_FEATUREPICK_H
