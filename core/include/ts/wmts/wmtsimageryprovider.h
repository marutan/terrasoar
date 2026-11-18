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
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QImage>
#include <QString>

#include "ts/network/cachedtilefetcher.h"
#include "ts/network/regiontilesource.h"
#include "ts/providers/tileprovider.h"
#include "ts/wmts/wmtssource.h"

namespace ts
{

class NetworkClient;
class TileDiskCache;
class WmtsCrsTransform;

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
 * Tile matrix sets in other CRSs (projected ones such as British National
 * Grid, see isProjectedWmtsCrs()) are reprojected on the CPU: a grid of the
 * quadtree tile's points (inside the source's bounds) goes through PROJ
 * (WmtsCrsTransform), its extent picks the block of tiles, and each pixel
 * of the geographic image is placed in the CRS by interpolating the grid
 * and coloured from the block, bilinear.
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
class WmtsImageryProvider : public ImageryProvider, public RegionTileSource
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
   * Returns the service's address (WmtsSource::service), else the KVP
   * address, else the template's host.
   */
  QUrl serviceUrl() const override;

  /**
   * Returns where the source has tiles (its bounds, and for Web Mercator
   * the latitudes the projection reaches), or nothing for everywhere.
   */
  std::optional<GeoRect> dataBounds() const override;

  /**
   * Returns typicalImageBytes() for the format and the size of a WMTS
   * tile of the finest matrix.
   */
  int64_t typicalTileBytes() const override;

  /**
   * Makes sure the WMTS tiles that quadtree tile @p key is made from are
   * in the disk cache and fresh, and pins them for @p region (see
   * CachedTileFetcher::fetchForRegion()). The result adds up theirs: no
   * data if none of them exist or all are missing, failed or cancelled
   * if any was, else downloaded, revalidated or already stored, with
   * their bytes. A WMTS tile shared by neighbouring quadtree tiles is
   * fetched once and counted again (already stored) for each.
   */
  RegionTileResult fetchForRegion(TileKey key, int64_t region,
                                  const std::atomic<bool>& cancelled) override;

  /** Returns the disk cache, or null without one. */
  TileDiskCache* diskCache() const override { return m_fetcher->diskCache(); }

  /** Returns what it has done so far (counted per WMTS tile). */
  WebTileStatistics statistics() const;

private:
  /**
   * A window's WMTS tiles side by side, and what became of them. Drawn
   * smaller (by scale) when the block would be over 2048 pixels across.
   */
  struct Mosaic {
    QImage image;
    ImageLoadResult::Outcome outcome = ImageLoadResult::Outcome::NoData;
    /** The mosaic's pixels per WMTS tile pixel (1, or less). */
    double scale = 1.0;
  };

  /**
   * For a projected CRS: the part of a quadtree tile inside the source's
   * bounds, a grid of its points in the CRS, and their extent there.
   */
  struct ProjectedGrid {
    /** Cells along each side of the grid. */
    static constexpr int32_t cells = 16;
    GeoRect clip;
    /** (cells + 1)², row by row from the north-west; NaN where PROJ fails. */
    std::vector<std::pair<double, double>> points;
    GeoRect extent;
  };

  Mosaic fetchMosaic(const WmtsTileWindow& window,
                     const std::atomic<bool>& cancelled) const;
  std::optional<ProjectedGrid> projectedGrid(TileKey key) const;
  ImageLoadResult loadReprojected(TileKey key,
                                  const std::atomic<bool>& cancelled);
  QImage resample(TileKey key, const ProjectedGrid& grid,
                  const WmtsTileWindow& window, const Mosaic& mosaic) const;
  std::unique_ptr<WmtsCrsTransform> takeTransform() const;
  void returnTransform(std::unique_ptr<WmtsCrsTransform> transform) const;

  const WmtsSource m_source;
  const std::unique_ptr<CachedTileFetcher> m_fetcher;
  /** PROJ transformations not in use (each used by one thread at a time). */
  mutable std::mutex m_transformMutex;
  mutable std::vector<std::unique_ptr<WmtsCrsTransform>> m_transforms;
};

} // namespace ts

#endif // TS_WMTS_WMTSIMAGERYPROVIDER_H
