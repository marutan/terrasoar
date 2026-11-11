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

#include <QString>

#include "ts/providers/tileprovider.h"
#include "ts/wms/wmssource.h"

namespace ts
{

class NetworkClient;
class TileDiskCache;

/** Behaviour of a WmsImageryProvider. */
struct WmsProviderSettings {
  /**
   * Directory of the disk caches (one file per source, see
   * wmsCacheFileName()); empty for no disk cache.
   */
  QString cacheDirectory;
  /** Size limit of this source's disk cache, bytes. */
  long long cacheBytes = 256LL * 1024 * 1024;
  /** Freshness of tiles whose server says nothing about it, seconds. */
  long long defaultLifetimeS = 7LL * 24 * 3600;
  /**
   * After this many requests in a row that can't reach the server, it is
   * left alone for offlineRetry: tiles come from the disk cache only.
   */
  int failuresBeforeOffline = 3;
  /** How long the server is left alone, see failuresBeforeOffline. */
  std::chrono::seconds offlineRetry{30};
  /** Loader threads, so several requests are in flight. */
  int loaderThreads = 6;
  /**
   * Seconds since 1970, UTC (for tests); null for the system clock.
   * Called on loader threads.
   */
  std::function<long long()> clock;
};

/** What a WmsImageryProvider has done, for tests and the debug overlay. */
struct WmsStatistics {
  /** GetMap requests made (each may have been retried). */
  int requests = 0;
  /** Tiles served fresh from the disk cache. */
  int cacheHits = 0;
  /** Stale tiles the server confirmed with 304 Not Modified. */
  int revalidated = 0;
  /** Stale tiles shown because the server couldn't be reached. */
  int servedStale = 0;
  /** Tiles that failed (to be retried later). */
  int failures = 0;
  /** Requests skipped while the server is left alone. */
  int skippedOffline = 0;
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
                     const WmsProviderSettings& settings = {});

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

  /** Returns the disk cache, or null without one. */
  TileDiskCache* diskCache() const { return m_cache.get(); }

  /** Returns what it has done so far. */
  WmsStatistics statistics() const;

private:
  long long now() const;
  ImageLoadResult fromCache(TileKey key, bool stale);
  void noteSuccess();
  void noteUnreachable(const QString& detail);
  void noteServerError(const QString& message);

  const WmsSource m_source;
  const std::shared_ptr<NetworkClient> m_network;
  const WmsProviderSettings m_settings;
  const std::unique_ptr<TileDiskCache> m_cache;
  mutable std::mutex m_mutex;
  int m_unreachableInARow = 0;
  std::chrono::steady_clock::time_point m_leftAloneUntil;
  WmsStatistics m_statistics;
};

/**
 * Returns the file name of the disk cache of the source with id
 * @p sourceId: a hash of the id, so equal sources share a cache.
 */
QString wmsCacheFileName(const QString& sourceId);

} // namespace ts

#endif // TS_WMS_WMSIMAGERYPROVIDER_H
