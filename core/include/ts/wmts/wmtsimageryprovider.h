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
 * Imagery from a WMTS server: its tiles fitted to the quadtree, cached on
 * disk.
 */
#ifndef TS_WMTS_WMTSIMAGERYPROVIDER_H
#define TS_WMTS_WMTSIMAGERYPROVIDER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include <QString>

#include "ts/network/cachedtilefetcher.h"
#include "ts/providers/tileprovider.h"
#include "ts/wmts/wmtssource.h"

namespace ts
{

class NetworkClient;
class TileDiskCache;

/**
 * Imagery from one layer of a WMTS server.
 *
 * For each quadtree tile the provider picks a tile matrix
 * (wmtsMatrixFor()), fetches the block of its tiles that covers the
 * quadtree tile (wmtsTileWindow()), and cuts the quadtree tile's image out
 * of them: one scaled copy, which is exact when the matrix lines up with
 * the quadtree (WorldCRS84Quad, GoogleMapsCompatible). Tiles of matrix
 * sets in Web Mercator make Web Mercator images, reprojected when drawn,
 * as for WMS.
 *
 * Each WMTS tile is fetched through a CachedTileFetcher and kept in the
 * disk cache under its own key (matrix, column, row), so neighbouring
 * quadtree tiles that share a WMTS tile fetch it once. Caching,
 * revalidation, the offline state and status reports are as for WMS
 * (docs/design/wms.md); unlike WMS, 404 means the service has no tile
 * there (no data), as WMTS servers answer for empty areas.
 *
 * A quadtree tile is no data when none of its WMTS tiles exist or all of
 * them are missing; it failed (to be asked for again) if any of them
 * failed. See docs/design/wmts.md.
 *
 * @threadsafe
 */
class WmtsImageryProvider : public ImageryProvider
{
public:
  /** Requests @p source's tiles through @p network. */
  WmtsImageryProvider(WmtsSource source, std::shared_ptr<NetworkClient> network,
                      WebTileSettings settings = {});

  /** Closes the disk cache. */
  ~WmtsImageryProvider() override;

  WmtsImageryProvider(const WmtsImageryProvider&) = delete;
  WmtsImageryProvider& operator=(const WmtsImageryProvider&) = delete;
  WmtsImageryProvider(WmtsImageryProvider&&) = delete;
  WmtsImageryProvider& operator=(WmtsImageryProvider&&) = delete;

  /** Returns the source's id (WmtsSource::id()). */
  QString id() const override;
  /** Returns the source's title, or its layer's identifier. */
  QString name() const override;
  /** Returns the source's deepest level. */
  int32_t maximumLevel() const override;
  /** Returns the size of the images made for quadtree tiles. */
  int32_t tileSizePx() const override;
  /** Returns the loader threads from the settings. */
  int32_t preferredLoaderThreads() const override;
  /** Returns Web Mercator for Web Mercator tile matrix sets. */
  TileImageProjection imageProjection() const override;
  /** Returns the image, or a null image whatever went wrong. */
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;
  /** Loads quadtree tile @p key (see the class description). */
  ImageLoadResult loadTileImage(TileKey key,
                                const std::atomic<bool>& cancelled) override;

  /** Returns the source. */
  const WmtsSource& source() const { return m_source; }

  /**
   * Returns where the source has tiles (its bounds, and for Web Mercator
   * the latitudes the projection reaches), or nothing for everywhere.
   */
  std::optional<GeoRect> dataBounds() const;

  /** Returns the disk cache, or null without one. */
  TileDiskCache* diskCache() const { return m_fetcher->diskCache(); }

  /** Returns what it has done so far (counted per WMTS tile). */
  WebTileStatistics statistics() const;

private:
  const WmtsSource m_source;
  const std::unique_ptr<CachedTileFetcher> m_fetcher;
};

} // namespace ts

#endif // TS_WMTS_WMTSIMAGERYPROVIDER_H
