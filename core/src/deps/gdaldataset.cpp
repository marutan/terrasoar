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

#include "ts/deps/gdaldataset.h"

#include <cstdint>
#include <utility>

#include <QByteArray>
#include <QFile>

#include <cpl_error.h>
#include <gdal.h>

#include "ts/deps/dependencies.h"

namespace ts
{

GdalDataset::GdalDataset(const QString& path)
{
  initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  // GDAL keeps the last error per thread, so reset it first.
  CPLErrorReset();
  m_handle = GDALOpenEx(encoded.constData(),
                        GDAL_OF_READONLY | GDAL_OF_RASTER | GDAL_OF_VECTOR |
                            GDAL_OF_VERBOSE_ERROR,
                        nullptr, nullptr, nullptr);
  if (m_handle == nullptr) {
    m_error = QString::fromUtf8(CPLGetLastErrorMsg());
  }
}

GdalDataset::~GdalDataset()
{
  close();
}

GdalDataset::GdalDataset(GdalDataset&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)),
      m_error(std::move(other.m_error))
{
}

GdalDataset& GdalDataset::operator=(GdalDataset&& other) noexcept
{
  if (this != &other) {
    close();
    m_handle = std::exchange(other.m_handle, nullptr);
    m_error = std::move(other.m_error);
  }
  return *this;
}

void GdalDataset::close()
{
  if (m_handle != nullptr) {
    GDALClose(static_cast<GDALDatasetH>(m_handle));
    m_handle = nullptr;
  }
}

QString GdalDataset::driverShortName() const
{
  if (m_handle == nullptr) {
    return QString();
  }
  GDALDriverH driver =
      GDALGetDatasetDriver(static_cast<GDALDatasetH>(m_handle));
  return driver ? QString::fromUtf8(GDALGetDriverShortName(driver)) : QString();
}

int32_t GdalDataset::layerCount() const
{
  return m_handle
             ? GDALDatasetGetLayerCount(static_cast<GDALDatasetH>(m_handle))
             : 0;
}

int32_t GdalDataset::rasterBandCount() const
{
  return m_handle ? GDALGetRasterCount(static_cast<GDALDatasetH>(m_handle)) : 0;
}

int64_t GdalDataset::featureCount(int32_t layerIndex) const
{
  if (m_handle == nullptr) {
    return -1;
  }
  OGRLayerH layer =
      GDALDatasetGetLayer(static_cast<GDALDatasetH>(m_handle), layerIndex);
  if (layer == nullptr) {
    return -1;
  }
  return static_cast<int64_t>(OGR_L_GetFeatureCount(layer, TRUE));
}

} // namespace ts
