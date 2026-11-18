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
 * Fetches tiles from a web server through a disk cache, for web sources
 * of every kind (WMS imagery, web terrain tiles).
 */
#ifndef TS_NETWORK_CACHEDTILEFETCHER_H
#define TS_NETWORK_CACHEDTILEFETCHER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include <QByteArray>
#include <QString>
#include <QUrl>

#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

class NetworkClient;
class TileDiskCache;
struct CachedTile;
struct HttpResponse;

/** Behaviour of a web tile source (its CachedTileFetcher). */
struct WebTileSettings {
  /**
   * Directory of the disk caches (one file per source, see
   * webCacheFileName()); empty for no disk cache.
   */
  QString cacheDirectory;
  /** Size limit of this source's disk cache, bytes. */
  int64_t cacheBytes = int64_t{256} * 1024 * 1024;
  /** Freshness of tiles whose server says nothing about it, seconds. */
  int64_t defaultLifetimeS = int64_t{7} * 24 * 3600;
  /**
   * After this many requests in a row that can't reach the server, it is
   * left alone for offlineRetry: tiles come from the disk cache only.
   */
  int32_t failuresBeforeOffline = 3;
  /** How long the server is left alone, see failuresBeforeOffline. */
  std::chrono::seconds offlineRetry{30};
  /** Loader threads, so several requests are in flight. */
  int32_t loaderThreads = 6;
  /**
   * Seconds since 1970, UTC (for tests); null for the system clock.
   * Called on loader threads.
   */
  std::function<int64_t()> clock;
};

/** What a web tile source has done, for tests and the debug overlay. */
struct WebTileStatistics {
  /** Requests made (each may have been retried). */
  int32_t requests = 0;
  /** Tiles served fresh from the disk cache. */
  int32_t cacheHits = 0;
  /** Stale tiles the server confirmed with 304 Not Modified. */
  int32_t revalidated = 0;
  /** Stale tiles shown because the server couldn't be reached. */
  int32_t servedStale = 0;
  /** Tiles that failed (to be retried later). */
  int32_t failures = 0;
  /** Requests skipped while the server is left alone. */
  int32_t skippedOffline = 0;
};

/** What became of a request for one tile (CachedTileFetcher::fetch()). */
enum class FetchOutcome {
  /** The tile was accepted (from the server or the cache). */
  Data,
  /** The server has no such tile (404 or 204, see Messages): for good. */
  NoData,
  /** Nothing now; worth asking again later. */
  Failed,
  /** Given up because it was cancelled. */
  Cancelled,
};

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
  int64_t bytes = 0;
  /** True if it failed because the server couldn't be reached. */
  bool unreachable = false;
  /** Why it failed (translated), or empty. */
  QString errorString;
};

/**
 * Fetches tiles through a disk cache: a fresh saved tile is used without
 * asking the server, a stale one is revalidated (If-None-Match /
 * If-Modified-Since), and when the server can't be reached (or answers
 * with an error) a saved tile is used however old it is. After
 * failuresBeforeOffline unreachable requests in a row the server is left
 * alone for offlineRetry. The source's status is reported through a
 * callback: Ok, Offline, or Error with a message.
 *
 * What a tile's bytes mean is up to the caller: fetch() hands them to an
 * accept function, which decodes them (keeping the result) and says
 * whether they are a tile at all.
 *
 * @threadsafe
 */
class CachedTileFetcher
{
public:
  /** How the source describes problems to the user (translated). */
  struct Messages {
    /** For a response with an error status. */
    std::function<QString(const HttpResponse&)> statusError;
    /** For a response accept() refused. */
    std::function<QString(const HttpResponse&)> notATile;
    /**
     * True if 404 and 204 mean the source has no tile there (XYZ tile
     * sets); otherwise they are server errors (WMS, which always has a
     * map to give).
     */
    bool notFoundIsNoData = false;
  };

  /**
   * Fetches through @p network into the disk cache of @p sourceId (see
   * WebTileSettings::cacheDirectory). @p host names the server in
   * messages; @p report receives status changes (on loader threads).
   */
  // The source, then its server's name.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  CachedTileFetcher(const QString& sourceId, QString host,
                    std::shared_ptr<NetworkClient> network,
                    WebTileSettings settings, Messages messages,
                    std::function<void(const ProviderStatus&)> report);
  ~CachedTileFetcher();

  CachedTileFetcher(const CachedTileFetcher&) = delete;
  CachedTileFetcher& operator=(const CachedTileFetcher&) = delete;
  CachedTileFetcher(CachedTileFetcher&&) = delete;
  CachedTileFetcher& operator=(CachedTileFetcher&&) = delete;

  /**
   * Gets tile @p key from @p url (or the cache), calling @p accept with
   * its bytes; @p accept returns false for bytes that aren't a tile (they
   * are then neither kept nor used). Data means accept() took the bytes.
   */
  FetchOutcome fetch(TileKey key, const QUrl& url,
                     const std::function<bool(const QByteArray&)>& accept,
                     const std::atomic<bool>& cancelled);

  /** Returns the settings. */
  const WebTileSettings& settings() const { return m_settings; }

  /** Returns the network client. */
  const std::shared_ptr<NetworkClient>& network() const { return m_network; }

  /** Returns the disk cache, or null without one. */
  TileDiskCache* diskCache() const { return m_cache.get(); }

  /** Returns the server's host name, for messages. */
  const QString& host() const { return m_host; }

  /** Returns what it has done so far. */
  WebTileStatistics statistics() const;

  /**
   * Makes sure tile @p key is in the disk cache and fresh, fetching it
   * from @p url if not, and pins it for the offline region @p region
   * (unless 0, as for the samples of a size estimate). A fresh stored tile
   * isn't fetched again; a stale one is revalidated. The bytes are only
   * checked with @p isTile, not kept. Doesn't change the source's status:
   * a failing download is reported to its caller. Fails without a disk
   * cache, and for responses the server forbids storing (no-store).
   * Called on download threads.
   */
  RegionTileResult
  fetchForRegion(TileKey key, const QUrl& url, int64_t region,
                 const std::function<bool(const QByteArray&)>& isTile,
                 const std::atomic<bool>& cancelled);

  /** Counts a request made outside fetch() (region downloads). */
  void countRequest();

  /** Returns the time now, seconds since 1970 (WebTileSettings::clock). */
  int64_t now() const;

private:
  FetchOutcome fromCache(TileKey key, bool stale,
                         const std::function<bool(const QByteArray&)>& accept);
  FetchOutcome
  staleOrFailed(TileKey key, bool haveCached,
                const std::function<bool(const QByteArray&)>& accept);
  bool leftAlone();
  FetchOutcome
  revalidated(TileKey key, const CachedTile& cached,
              const HttpResponse& response, int64_t nowS,
              const std::function<bool(const QByteArray&)>& accept);
  void store(TileKey key, const HttpResponse& response, int64_t nowS);
  void noteSuccess();
  void noteUnreachable(const QString& detail);
  void noteServerError(const QString& message);

  const QString m_host;
  const std::shared_ptr<NetworkClient> m_network;
  const WebTileSettings m_settings;
  const Messages m_messages;
  const std::function<void(const ProviderStatus&)> m_report;
  const std::unique_ptr<TileDiskCache> m_cache;
  mutable std::mutex m_mutex;
  int32_t m_unreachableInARow = 0;
  std::chrono::steady_clock::time_point m_leftAloneUntil;
  WebTileStatistics m_statistics;
};

/**
 * Returns the file name of the disk cache of the source with id
 * @p sourceId: a hash of the id, so equal sources share a cache.
 */
QString webCacheFileName(const QString& sourceId);

} // namespace ts

#endif // TS_NETWORK_CACHEDTILEFETCHER_H
