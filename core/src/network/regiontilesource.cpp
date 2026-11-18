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
#include "ts/network/regiontilesource.h"

#include <cstdint>

namespace ts
{

RegionTileSource::~RegionTileSource() = default;

int64_t typicalImageBytes(const QString& format, int32_t tileSizePx)
{
  // Rough sizes of 256-pixel map tiles: JPEG photographs about 20 KB,
  // PNG maps about 25 KB; scaled by area for other tile sizes.
  const double perPixel =
      format.contains(QLatin1String("jpeg"), Qt::CaseInsensitive) ||
              format.contains(QLatin1String("jpg"), Qt::CaseInsensitive)
          ? 20000.0 / 65536.0
          : 25000.0 / 65536.0;
  const double pixels = static_cast<double>(tileSizePx) * tileSizePx;
  return static_cast<int64_t>(perPixel * pixels);
}

} // namespace ts
