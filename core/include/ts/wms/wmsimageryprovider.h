// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Imagery from a WMS server: one GetMap per tile, cached on disk.
 */
#ifndef TS_WMS_WMSIMAGERYPROVIDER_H
#define TS_WMS_WMSIMAGERYPROVIDER_H

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <QString>

#include "ts/network/cachedtilefetcher.h"
#include "ts/providers/tileprovider.h"
#include "ts/wms/wmssource.h"

namespace ts
{

class NetworkClient;
class TileDiskCache;

/** Behaviour of a WmsImageryProvider (see WebTileSettings). */
using WmsProviderSettings = WebTileSettings;

/** What a WmsImageryProvider has done (see WebTileStatistics). */
using WmsStatistics = WebTileStatistics;

/** What became of one tile of an offline region (fetchForRegion()). */
struct RegionTileResult {
  /** What happened. */
  enum class Outcome {
    Downloaded,    ///< Fetched and stored.
    Revalidated,   ///< Stored but stale; the server confirmed it (304).
    AlreadyCached, ///< Stored and fresh; nothing fetched.
    NoData,        ///< The source has nothing there.
    Failed,        ///< Not stored (see errorString).
    Cancelled,     ///< Cancelled while fetching.
  };
  /** What happened. */
  Outcome outcome = Outcome::Failed;
  /** The tile's size on disk, when stored. */
  long long bytes = 0;
  /** True if it failed because the server couldn't be reached. */
  bool unreachable = false;
  /** Why it failed (translated), or empty. */
  QString errorString;
};

/**
 * Imagery from one or more layers of a WMS server.
 *
 * Each quadtree tile is one GetMap request in the source's CRS (see
 * WmsSource and getMapUrl()), made through a shared NetworkClient on a
 * loader thread. Responses are kept in a TileDiskCache as they came, with
 * their HTTP freshness: a fresh tile is served from disk without asking,
 * a stale one is revalidated (If-None-Match / If-Modified-Since), and
 * when the server can't be reached stale tiles are shown as they are.
 *
 * Failures are told apart (loadTileImage()): a tile outside the source's
 * bounds has no data; a network error or a server error is a failure the
 * loader retries later, unless a saved copy of the tile (stale or not) can
 * be shown meanwhile. status() says how the source is doing: Ok;
 * Offline when the server can't be reached (saved tiles shown where there
 * are some); Error with the server's message when it answers with an
 * error or with something that isn't an image.
 *
 * Images in Web Mercator are delivered as they are and reprojected when
 * drawn (imageProjection()). See docs/design/wms.md.
 *
 * @threadsafe
 */
class WmsImageryProvider : public ImageryProvider
{
public:
  /** Requests @p source's tiles through @p network. */
  WmsImageryProvider(WmsSource source, std::shared_ptr<NetworkClient> network,
                     WmsProviderSettings settings = {});

  /** Closes the disk cache. */
  ~WmsImageryProvider() override;

  WmsImageryProvider(const WmsImageryProvider&) = delete;
  WmsImageryProvider& operator=(const WmsImageryProvider&) = delete;
  WmsImageryProvider(WmsImageryProvider&&) = delete;
  WmsImageryProvider& operator=(WmsImageryProvider&&) = delete;

  /** Returns the source's id (WmsSource::id()). */
  QString id() const override;
  /** Returns the source's title, or its layers' names. */
  QString name() const override;
  /** Returns the source's deepest level. */
  int maximumLevel() const override;
  /** Returns the source's tile size. */
  int tileSizePx() const override;
  /** Returns the loader threads from the settings. */
  int preferredLoaderThreads() const override;
  /** Returns Web Mercator for Web Mercator sources. */
  TileImageProjection imageProjection() const override;
  /** Returns the image, or a null image whatever went wrong. */
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;
  /** Loads tile @p key (see the class description). */
  ImageLoadResult loadTileImage(TileKey key,
                                const std::atomic<bool>& cancelled) override;

  /** Returns the source. */
  const WmsSource& source() const { return m_source; }

  /**
   * Returns where the source has tiles (its bounds, and for Web Mercator
   * the latitudes the projection reaches), or nothing for everywhere.
   */
  std::optional<GeoRect> dataBounds() const;

  /**
   * Makes sure tile @p key is in the disk cache and fresh, and pins it
   * for the offline region @p region (unless 0, as for the samples of a
   * size estimate). A fresh stored tile isn't fetched again; a stale one
   * is revalidated. The tile isn't decoded for the view, only checked to
   * be an image. Doesn't change status(): a failing download is reported
   * to its caller. Fails without a disk cache, and for responses the
   * server forbids storing (no-store). Called on download threads.
   */
  RegionTileResult fetchForRegion(TileKey key, long long region,
                                  const std::atomic<bool>& cancelled);

  /** Returns the disk cache, or null without one. */
  TileDiskCache* diskCache() const { return m_fetcher->diskCache(); }

  /** Returns what it has done so far. */
  WmsStatistics statistics() const;

private:
  const WmsSource m_source;
  const std::unique_ptr<CachedTileFetcher> m_fetcher;
};

/**
 * Returns the file name of the disk cache of the source with id
 * @p sourceId: a hash of the id, so equal sources share a cache.
 */
QString wmsCacheFileName(const QString& sourceId);

} // namespace ts

#endif // TS_WMS_WMSIMAGERYPROVIDER_H
