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
 * Horizontal datum shift grids (such as OSTN15), and the grids installed
 * packs provide.
 */
#ifndef TS_PROJECTION_DATUMSHIFT_H
#define TS_PROJECTION_DATUMSHIFT_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QString>

namespace ts
{

/**
 * A grid of latitude and longitude offsets from one datum to another, in
 * PROJ's GeoTIFF format (`TYPE=HORIZONTAL_OFFSET`, as PROJ's `hgridshift`
 * reads): offsets in arc-seconds, bilinearly interpolated between the
 * nodes, longitude offsets positive east. Read whole into memory, so it
 * holds no file open.
 *
 * @threadsafe (immutable once loaded)
 */
class HorizontalShiftGrid
{
public:
  /**
   * Reads the grid at @p path. Returns null and sets @p errorString
   * (English) if it can't be read or isn't a horizontal offset grid.
   */
  static std::shared_ptr<const HorizontalShiftGrid> load(const QString& path,
                                                         QString* errorString);

  /** Returns the file it was read from. */
  QString path() const { return m_path; }

  /**
   * Returns the offsets to add to latitude @p latRad and longitude
   * @p lonRad of the source datum to get the target's, {latitude,
   * longitude}, radians; nothing outside the grid.
   */
  std::optional<std::array<double, 2>> offsetRad(double latRad,
                                                 double lonRad) const;

  /**
   * Returns offsetRad() at the nearest point of the grid: at the point
   * inside it, at the nearest edge outside.
   */
  std::array<double, 2> clampedOffsetRad(double latRad, double lonRad) const;

  /**
   * Returns how far inside the grid latitude @p latDeg and longitude
   * @p lonDeg are, degrees (the distance to the nearest edge, in degrees
   * of latitude or longitude); negative outside.
   */
  double insideDeg(double latDeg, double lonDeg) const;

private:
  HorizontalShiftGrid() = default;

  QString m_path;
  /** Nodes along a row (west to east) and a column (south to north). */
  int32_t m_columns = 0;
  int32_t m_rows = 0;
  /** The south-west node and the spacing, degrees. */
  double m_westDeg = 0.0;
  double m_southDeg = 0.0;
  double m_lonStepDeg = 0.0;
  double m_latStepDeg = 0.0;
  /** Offsets in arc-seconds, rows from the south. */
  std::vector<float> m_latOffsets;
  std::vector<float> m_lonOffsets;
};

/**
 * Makes the grids in @p pathsById (an id such as "ostn15" → a file) the
 * ones datumShiftGrid() gives, reading those not already read; ids left
 * out have none. Grids that can't be read are left out with a warning.
 * Projections made afterwards use them (ts::projectionWith()).
 *
 * @threadsafe
 */
void setDatumShiftGrids(const std::map<QString, QString>& pathsById);

/**
 * Returns the grid with @p id ("ostn15") an installed pack provides, or
 * null.
 *
 * @threadsafe
 */
std::shared_ptr<const HorizontalShiftGrid> datumShiftGrid(const QString& id);

} // namespace ts

#endif // TS_PROJECTION_DATUMSHIFT_H
