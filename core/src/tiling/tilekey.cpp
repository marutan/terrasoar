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

#include "ts/tiling/tilekey.h"

#include <algorithm>
#include <cmath>

namespace ts
{

TileKey TileKey::containing(int level, const Geodetic& position)
{
  const double latDeg = position.latDeg;
  const double lonDeg = position.lonDeg;
  const int clampedLevel = std::clamp(level, 0, maximumLevel);
  const double size = sizeDeg(clampedLevel);
  double lon = normaliseLonDeg(lonDeg);
  // normaliseLonDeg maps 180° E to −180°; keep it in the last column.
  if (lonDeg >= 180.0 && lon == -180.0) {
    lon = 180.0;
  }
  const double lat = std::clamp(latDeg, -90.0, 90.0);
  const std::int64_t column = std::clamp<std::int64_t>(
      static_cast<std::int64_t>(std::floor((lon + 180.0) / size)), 0,
      columns(clampedLevel) - 1);
  const std::int64_t row = std::clamp<std::int64_t>(
      static_cast<std::int64_t>(std::floor((90.0 - lat) / size)), 0,
      rows(clampedLevel) - 1);
  return TileKey{clampedLevel, static_cast<int>(column), static_cast<int>(row)};
}

GeoRect TileKey::bounds() const
{
  const double size = sizeDeg(level);
  const double west = -180.0 + size * x;
  const double north = 90.0 - size * y;
  return GeoRect{west, north - size, west + size, north};
}

TileKey TileKey::parent() const
{
  if (level == 0) {
    return *this;
  }
  return TileKey{level - 1, x / 2, y / 2};
}

TileKey TileKey::child(int index) const
{
  return TileKey{level + 1, 2 * x + (index & 1), 2 * y + ((index >> 1) & 1)};
}

bool TileKey::contains(TileKey other) const
{
  if (other.level < level) {
    return false;
  }
  const int shift = other.level - level;
  return (other.x >> shift) == x && (other.y >> shift) == y;
}

Vec3 TileKey::centreEcef() const
{
  const GeoRect rect = bounds();
  return geodeticToEcef(
      Geodetic{rect.centreLatDeg(), rect.centreLonDeg(), 0.0});
}

QString TileKey::toString() const
{
  return QStringLiteral("%1/%2/%3").arg(level).arg(x).arg(y);
}

} // namespace ts
