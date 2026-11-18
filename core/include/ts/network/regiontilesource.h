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
 * What an offline region needs of a web imagery source.
 */
#ifndef TS_NETWORK_REGIONTILESOURCE_H
#define TS_NETWORK_REGIONTILESOURCE_H

#include <atomic>
#include <cstdint>
#include <optional>

#include <QUrl>

#include "ts/network/cachedtilefetcher.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

class TileDiskCache;

/**
 * A web imagery source whose tiles can be kept for an offline region:
 * WMS (WmsImageryProvider) and WMTS (WmtsImageryProvider). Regions count
 * and fetch quadtree tiles; each source fetches and pins whatever it
 * stores for one (a GetMap image, or the WMTS tiles covering it).
 *
 * @threadsafe
 */
class RegionTileSource
{
public:
  RegionTileSource() = default;
  virtual ~RegionTileSource();

  RegionTileSource(const RegionTileSource&) = delete;
  RegionTileSource& operator=(const RegionTileSource&) = delete;
  RegionTileSource(RegionTileSource&&) = delete;
  RegionTileSource& operator=(RegionTileSource&&) = delete;

  /**
   * Returns the service's address, as its catalogue entry has it (for
   * the terms of use, bulkDownloadPolicy()).
   */
  virtual QUrl serviceUrl() const = 0;

  /**
   * Returns where the source has tiles, or nothing for everywhere;
   * quadtree tiles outside aren't counted or fetched.
   */
  virtual std::optional<GeoRect> dataBounds() const = 0;

  /**
   * Returns a guess of what one quadtree tile takes on disk, from the
   * image format and tile size alone (before any samples).
   */
  virtual int64_t typicalTileBytes() const = 0;

  /**
   * Makes sure what quadtree tile @p key needs is in the disk cache and
   * fresh, and pins it for the offline region @p region (unless 0, as for
   * the samples of a size estimate). Doesn't change the source's status.
   * Called on download threads.
   */
  virtual RegionTileResult
  fetchForRegion(TileKey key, int64_t region,
                 const std::atomic<bool>& cancelled) = 0;

  /** Returns the disk cache, or null without one. */
  virtual TileDiskCache* diskCache() const = 0;
};

/**
 * Returns a guess of the size of a @p tileSizePx tile in @p format: about
 * 20 KB for a 256-pixel JPEG photograph and 25 KB for a PNG map, scaled
 * by area.
 */
int64_t typicalImageBytes(const QString& format, int32_t tileSizePx);

} // namespace ts

#endif // TS_NETWORK_REGIONTILESOURCE_H
