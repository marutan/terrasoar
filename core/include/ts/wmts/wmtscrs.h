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
 * What PROJ knows of the coordinate system of a WMTS tile matrix set, and
 * turning longitudes and latitudes into it.
 */
#ifndef TS_WMTS_WMTSCRS_H
#define TS_WMTS_WMTSCRS_H

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QString>

#include "ts/tiling/tilekey.h"

namespace ts
{

/**
 * A coordinate system of a WMTS tile matrix set as PROJ describes it.
 *
 * @reentrant
 */
struct WmtsCrsInfo {
  /** The code, "EPSG:n". */
  QString code;
  /** PROJ's name for it. */
  QString name;
  /** True for a geographic CRS (degrees), false for a projected one. */
  bool geographic = false;
  /** True if its first axis points north or south (EPSG:4326, 3035, …). */
  bool northingFirst = false;
  /**
   * Metres per unit of its axes: 1 for metres, 0.3048 for feet, and for a
   * geographic CRS WMTS's degree (wmtsMetresPerDegree).
   */
  double metresPerUnit = 1.0;
  /** Where it is meant to be used, longitude and latitude; if PROJ says. */
  std::optional<GeoRect> areaOfUse;
};

/**
 * Returns what PROJ knows of @p code ("EPSG:n"), or nothing if PROJ
 * doesn't know it, or it isn't a 2D geographic or projected CRS. Asked of
 * PROJ once per code and kept.
 *
 * @threadsafe
 */
std::optional<WmtsCrsInfo> wmtsCrsInfo(const QString& code);

/**
 * Turns longitudes and latitudes (WGS 84) into the coordinates of a CRS,
 * easting (or longitude) first whatever the CRS's own axis order, and
 * back. Wraps a PROJ transformation with its own context: use each from
 * one thread at a time.
 *
 * @reentrant
 */
class WmtsCrsTransform
{
public:
  /** Prepares the transformation to @p code; check isValid(). */
  explicit WmtsCrsTransform(const QString& code);

  /** Frees the transformation. */
  ~WmtsCrsTransform();

  WmtsCrsTransform(const WmtsCrsTransform&) = delete;
  WmtsCrsTransform& operator=(const WmtsCrsTransform&) = delete;
  WmtsCrsTransform(WmtsCrsTransform&&) = delete;
  WmtsCrsTransform& operator=(WmtsCrsTransform&&) = delete;

  /** Returns true if PROJ could make the transformation. */
  bool isValid() const;

  /** Returns the CRS's code. */
  const QString& code() const { return m_code; }

  /**
   * Turns each point of @p points from (longitude, latitude) in degrees
   * into (easting, northing) in the CRS's units, in place. Points PROJ
   * can't transform become NaN.
   */
  void forward(std::vector<std::pair<double, double>>& points) const;

  /**
   * Turns each point of @p points from (easting, northing) back into
   * (longitude, latitude) in degrees, in place; NaN where PROJ can't.
   */
  void inverse(std::vector<std::pair<double, double>>& points) const;

private:
  struct Private;
  QString m_code;
  std::unique_ptr<Private> d;
};

} // namespace ts

#endif // TS_WMTS_WMTSCRS_H
