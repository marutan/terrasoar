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
#include "ts/projection/datumshift.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <utility>

#include <QByteArray>

#include <gdal.h>

#include "ts/deps/dependencies.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

/** Radians in an arc-second. */
constexpr double arcSecondRad = pi / (180.0 * 3600.0);

bool fail(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return false;
}

/** Reads band @p band of @p dataset whole, rows from the south. */
// The band, then the size as GDAL takes it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool readBand(GDALDatasetH dataset, int32_t band, int32_t columns, int32_t rows,
              std::vector<float>* values)
{
  std::vector<float> northFirst(static_cast<std::size_t>(columns) *
                                static_cast<std::size_t>(rows));
  if (GDALRasterIO(GDALGetRasterBand(dataset, band), GF_Read, 0, 0, columns,
                   rows, northFirst.data(), columns, rows, GDT_Float32, 0,
                   0) != CE_None) {
    return false;
  }
  values->resize(northFirst.size());
  const std::size_t width = static_cast<std::size_t>(columns);
  for (std::size_t row = 0; row < static_cast<std::size_t>(rows); ++row) {
    std::copy_n(northFirst.begin() +
                    static_cast<std::ptrdiff_t>(
                        (static_cast<std::size_t>(rows) - 1 - row) * width),
                width,
                values->begin() + static_cast<std::ptrdiff_t>(row * width));
  }
  return std::all_of(values->begin(), values->end(),
                     [](float value) { return std::isfinite(value); });
}

/** Returns the band of @p dataset whose description is @p name, or 0. */
int32_t bandNamed(GDALDatasetH dataset, const char* name)
{
  for (int32_t band = 1; band <= GDALGetRasterCount(dataset); ++band) {
    if (QByteArray(GDALGetDescription(GDALGetRasterBand(dataset, band))) ==
        name) {
      return band;
    }
  }
  return 0;
}

} // namespace

std::shared_ptr<const HorizontalShiftGrid>
HorizontalShiftGrid::load(const QString& path, QString* errorString)
{
  initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr);
  if (dataset == nullptr) {
    fail(errorString, QStringLiteral("Cannot open the grid %1").arg(path));
    return nullptr;
  }
  // The constructor is private, out of std::make_shared()'s reach.
  // NOLINTNEXTLINE(modernize-make-shared)
  std::shared_ptr<HorizontalShiftGrid> grid(new HorizontalShiftGrid());
  grid->m_path = path;
  grid->m_columns = GDALGetRasterXSize(dataset);
  grid->m_rows = GDALGetRasterYSize(dataset);
  std::array<double, 6> transform{};
  const int32_t latBand = bandNamed(dataset, "latitude_offset");
  const int32_t lonBand = bandNamed(dataset, "longitude_offset");
  const char* type = GDALGetMetadataItem(dataset, "TYPE", nullptr);
  const char* positive =
      lonBand == 0 ? nullptr
                   : GDALGetMetadataItem(GDALGetRasterBand(dataset, lonBand),
                                         "positive_value", nullptr);
  // NTv2's convention, kept by some converted grids.
  const bool positiveWest =
      positive != nullptr && QByteArray(positive) == "west";
  bool ok = GDALGetGeoTransform(dataset, transform.data()) == CE_None &&
            transform[2] == 0.0 && transform[4] == 0.0 && transform[1] > 0.0 &&
            transform[5] < 0.0 && grid->m_columns >= 2 && grid->m_rows >= 2 &&
            latBand != 0 && lonBand != 0 && type != nullptr &&
            QByteArray(type) == "HORIZONTAL_OFFSET";
  // GDAL gives the corners of pixels about the nodes.
  grid->m_lonStepDeg = transform[1];
  grid->m_latStepDeg = -transform[5];
  grid->m_westDeg = transform[0] + 0.5 * transform[1];
  grid->m_southDeg =
      transform[3] + (static_cast<double>(grid->m_rows) - 0.5) * transform[5];
  ok = ok &&
       readBand(dataset, latBand, grid->m_columns, grid->m_rows,
                &grid->m_latOffsets) &&
       readBand(dataset, lonBand, grid->m_columns, grid->m_rows,
                &grid->m_lonOffsets);
  GDALClose(dataset);
  if (!ok) {
    fail(errorString,
         QStringLiteral("%1 is not a horizontal offset grid").arg(path));
    return nullptr;
  }
  if (positiveWest) {
    for (float& offset : grid->m_lonOffsets) {
      offset = -offset;
    }
  }
  return grid;
}

// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double HorizontalShiftGrid::insideDeg(double latDeg, double lonDeg) const
{
  const double east =
      m_westDeg + static_cast<double>(m_columns - 1) * m_lonStepDeg;
  const double north =
      m_southDeg + static_cast<double>(m_rows - 1) * m_latStepDeg;
  return std::min(
      {lonDeg - m_westDeg, east - lonDeg, latDeg - m_southDeg, north - latDeg});
}

// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<std::array<double, 2>>
HorizontalShiftGrid::offsetRad(double latRad, double lonRad) const
{
  const double x = (radToDeg(lonRad) - m_westDeg) / m_lonStepDeg;
  const double y = (radToDeg(latRad) - m_southDeg) / m_latStepDeg;
  const double lastColumn = static_cast<double>(m_columns - 1);
  const double lastRow = static_cast<double>(m_rows - 1);
  if (!(x >= 0.0 && x <= lastColumn && y >= 0.0 && y <= lastRow)) {
    return std::nullopt;
  }
  const double column = std::min(std::floor(x), lastColumn - 1.0);
  const double row = std::min(std::floor(y), lastRow - 1.0);
  const double fx = x - column;
  const double fy = y - row;
  const std::size_t width = static_cast<std::size_t>(m_columns);
  const std::size_t sw =
      static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column);
  const std::size_t nw = sw + width;
  const std::array<double, 4> weights = {
      (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy};
  const std::array<std::size_t, 4> nodes = {sw, sw + 1, nw, nw + 1};
  double lat = 0.0;
  double lon = 0.0;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    lat += weights.at(i) * static_cast<double>(m_latOffsets.at(nodes.at(i)));
    lon += weights.at(i) * static_cast<double>(m_lonOffsets.at(nodes.at(i)));
  }
  return std::array<double, 2>{lat * arcSecondRad, lon * arcSecondRad};
}

// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<double, 2> HorizontalShiftGrid::clampedOffsetRad(double latRad,
                                                            double lonRad) const
{
  const double east =
      m_westDeg + static_cast<double>(m_columns - 1) * m_lonStepDeg;
  const double north =
      m_southDeg + static_cast<double>(m_rows - 1) * m_latStepDeg;
  const double lat = std::clamp(radToDeg(latRad), m_southDeg, north);
  const double lon = std::clamp(radToDeg(lonRad), m_westDeg, east);
  // Inside by construction (both ends are nodes).
  return offsetRad(degToRad(lat), degToRad(lon))
      .value_or(std::array<double, 2>{0.0, 0.0});
}

namespace
{

struct InstalledGrids {
  std::mutex mutex;
  std::map<QString, std::shared_ptr<const HorizontalShiftGrid>> grids;
};

InstalledGrids& installedGrids()
{
  static InstalledGrids installed;
  return installed;
}

} // namespace

void setDatumShiftGrids(const std::map<QString, QString>& pathsById)
{
  InstalledGrids& installed = installedGrids();
  std::map<QString, std::shared_ptr<const HorizontalShiftGrid>> previous;
  {
    const std::lock_guard<std::mutex> lock(installed.mutex);
    previous = installed.grids;
  }
  // Read outside the lock: a grid is a few megabytes.
  std::map<QString, std::shared_ptr<const HorizontalShiftGrid>> grids;
  for (const std::pair<const QString, QString>& entry : pathsById) {
    const std::map<QString,
                   std::shared_ptr<const HorizontalShiftGrid>>::const_iterator
        known = previous.find(entry.first);
    if (known != previous.end() && known->second->path() == entry.second) {
      grids[entry.first] = known->second;
      continue;
    }
    QString error;
    std::shared_ptr<const HorizontalShiftGrid> grid =
        HorizontalShiftGrid::load(entry.second, &error);
    if (grid) {
      grids[entry.first] = std::move(grid);
    } else {
      qWarning("Datum shift grid %s: %s", qUtf8Printable(entry.first),
               qUtf8Printable(error));
    }
  }
  const std::lock_guard<std::mutex> lock(installed.mutex);
  installed.grids = std::move(grids);
}

std::shared_ptr<const HorizontalShiftGrid> datumShiftGrid(const QString& id)
{
  InstalledGrids& installed = installedGrids();
  const std::lock_guard<std::mutex> lock(installed.mutex);
  const std::map<QString,
                 std::shared_ptr<const HorizontalShiftGrid>>::const_iterator
      found = installed.grids.find(id);
  return found == installed.grids.end() ? nullptr : found->second;
}

} // namespace ts
