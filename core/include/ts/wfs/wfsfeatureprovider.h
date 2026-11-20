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
 * Vector features from a WFS feature type, loaded by tile, paged, cached
 * on disk and kept for offline regions.
 */
#ifndef TS_WFS_WFSFEATUREPROVIDER_H
#define TS_WFS_WFSFEATUREPROVIDER_H

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <QUrl>

#include "ts/network/cachedtilefetcher.h"
#include "ts/network/regiontilesource.h"
#include "ts/providers/tileprovider.h"
#include "ts/vector/stylerules.h"
#include "ts/wfs/wfssource.h"

namespace ts
{

class NetworkClient;
class TileDiskCache;

/**
 * The features of one WFS feature type, tile by tile: each stored tile
 * (WfsSource::storedLevel()) is one GetFeature request for its bounding
 * box, followed by more pages while a page comes back full, up to
 * WfsSource::maximumPages. Each page is kept in the disk cache (its key
 * the stored tile, the page number in the row's high bits), revalidated
 * and used offline as WMS tiles are (CachedTileFetcher). The source's
 * style rules give each feature its style as it loads.
 *
 * The last 64 stored tiles' features are kept for picking (featuresAt()).
 * A tile whose pages run out before its features do says so in the status
 * ("zoom in to see them all").
 *
 * See docs/design/wfs.md.
 *
 * @threadsafe
 */
class WfsFeatureProvider : public FeatureProvider, public RegionTileSource
{
public:
  /** Requests @p source's features through @p network. */
  WfsFeatureProvider(WfsSource source, std::shared_ptr<NetworkClient> network,
                     WebTileSettings settings = {});
  ~WfsFeatureProvider() override;

  WfsFeatureProvider(const WfsFeatureProvider&) = delete;
  WfsFeatureProvider& operator=(const WfsFeatureProvider&) = delete;
  WfsFeatureProvider(WfsFeatureProvider&&) = delete;
  WfsFeatureProvider& operator=(WfsFeatureProvider&&) = delete;

  /** Returns the source's id. */
  QString id() const override;
  /** Returns the source's title or type name. */
  QString name() const override;
  /** Returns the deepest level that gains detail (TileKey's deepest). */
  int32_t maximumLevel() const override;
  /** Returns the loader threads from the settings. */
  int32_t preferredLoaderThreads() const override;

  /** Loads the features of @p key's stored tile (see the class). */
  std::optional<FeatureTile>
  loadFeatures(TileKey key, const std::atomic<bool>& cancelled) override;

  /** Returns @p key's ancestor at WfsSource::storedLevel(). */
  TileKey storedKey(TileKey key) const override;

  /** Returns the kept features under @p point (see FeatureProvider). */
  std::vector<Feature> featuresAt(const GeoPoint& point,
                                  double toleranceDeg) const override;

  /** Returns the service's address (WfsSource::service, else url). */
  QUrl serviceUrl() const override;
  /** Returns where the feature type has features. */
  std::optional<GeoRect> dataBounds() const override;
  /** Returns a guess of a tile's pages on disk. */
  int64_t typicalTileBytes() const override;
  /**
   * Makes sure every page of @p key's stored tile is in the disk cache
   * and fresh, pinned for @p region; their results added up.
   */
  RegionTileResult fetchForRegion(TileKey key, int64_t region,
                                  const std::atomic<bool>& cancelled) override;
  /** Returns the disk cache, or null without one. */
  TileDiskCache* diskCache() const override { return m_fetcher->diskCache(); }

  /** Returns the source. */
  const WfsSource& source() const { return m_source; }

  /** Returns what it has done so far (counted per page). */
  WebTileStatistics statistics() const;

private:
  /** The cache key of page @p page of stored tile @p key. */
  static TileKey pageKey(TileKey key, int32_t page);
  void keep(const FeatureTile& tile);

  const WfsSource m_source;
  const std::vector<StyleRule> m_rules;
  const std::unique_ptr<CachedTileFetcher> m_fetcher;
  mutable std::mutex m_keptMutex;
  /** Recently loaded tiles, most recent first, for picking. */
  std::list<FeatureTile> m_kept;
};

} // namespace ts

#endif // TS_WFS_WFSFEATUREPROVIDER_H
