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
 * Imagery from a local raster file GDAL reads: GeoTIFFs, JPEG 2000, and
 * the Raster Product Format's charts and imagery (CADRG, CIB) through
 * their A.TOC.
 */
#ifndef TS_PROVIDERS_GDALIMAGERYPROVIDER_H
#define TS_PROVIDERS_GDALIMAGERYPROVIDER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <QString>

#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

/**
 * Imagery from a local raster file (layer source `{"kind": "file",
 * "path": …}`), each tile warped from it by GDAL into the tile's
 * latitude/longitude rectangle (bilinear). A file with subdatasets (an
 * RPF A.TOC: one per product scale and ARC zone) is all of them, drawn in
 * the order the file lists them. Paletted rasters (CADRG) are expanded to
 * RGBA; rasters without an alpha band are opaque where they have data.
 * The deepest level is the one whose texels are about the finest part's
 * pixels.
 *
 * Charts are resampled into the quadtree's tiles, not drawn pixel for
 * pixel in their ARC zone (docs/design/rpf.md).
 *
 * @threadsafe (a pool of GDAL handles per part, one per loader thread)
 */
class GdalImageryProvider : public ImageryProvider
{
public:
  /** Opens the raster at @p path; see isValid(). */
  explicit GdalImageryProvider(const QString& path);
  ~GdalImageryProvider() override;

  GdalImageryProvider(const GdalImageryProvider&) = delete;
  GdalImageryProvider& operator=(const GdalImageryProvider&) = delete;
  GdalImageryProvider(GdalImageryProvider&&) = delete;
  GdalImageryProvider& operator=(GdalImageryProvider&&) = delete;

  /** Returns true if the file could be opened as georeferenced imagery. */
  bool isValid() const;

  /** Returns why it couldn't (English), if not valid. */
  QString errorString() const;

  QString id() const override;
  QString name() const override;
  int32_t maximumLevel() const override { return m_maximumLevel; }
  int32_t tileSizePx() const override { return tileSizePxFixed; }
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

  /** What one part of the file covers. */
  struct Coverage {
    /** GDAL's name for it (the file, or a subdataset). */
    QString name;
    /** Its latitudes and longitudes, degrees. */
    GeoRect extent;
    /** Its pixels' size, degrees (along a meridian, for a projected
     *  raster its metres over 111 km). */
    double pixelSizeDeg = 0.0;
  };

  /** Returns the parts of the file, in drawing order. */
  std::vector<Coverage> coverage() const;

private:
  struct Part;

  /** The tiles' size, pixels (also used while constructing). */
  static constexpr int32_t tileSizePxFixed = 256;

  QString m_path;
  QString m_error;
  int32_t m_maximumLevel = 0;
  std::vector<std::unique_ptr<Part>> m_parts;
};

} // namespace ts

#endif // TS_PROVIDERS_GDALIMAGERYPROVIDER_H
