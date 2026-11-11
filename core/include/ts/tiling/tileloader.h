// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Loads tiles on worker threads.
 */
#ifndef TS_TILING_TILELOADER_H
#define TS_TILING_TILELOADER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QImage>

#include "ts/tiling/lrucache.h"
#include "ts/tiling/tilekey.h"
#include "ts/tiling/tilemesh.h"
#include "ts/tiling/tileselector.h"

namespace ts
{

class ImageryProvider;

/** A tile loaded on a worker, ready to go to the GPU. */
struct LoadedTile {
  /** The tile. */
  TileKey key;
  /** Its imagery; null if the provider has none there. */
  QImage image;
  /** Its mesh. */
  TileMesh mesh;
};

/** Settings of a TileLoader. */
struct TileLoaderSettings {
  /**
   * Worker threads (the reference PC has 4 hardware threads). A provider
   * that asks for more (ImageryProvider::preferredLoaderThreads(), a
   * network source waiting on its server) gets what it asks for.
   */
  int threads = 2;
  /**
   * After a tile failed to load (ImageLoadResult::Outcome::Failed), it is
   * not asked for again for this long, so an unreachable server isn't
   * asked every frame and the view can go idle.
   */
  std::chrono::milliseconds failureRetryDelay{30000};
  /** Budget of the decoded-image cache in bytes (common use: 48 MB). */
  std::size_t cacheBytes = std::size_t{48} * 1024 * 1024;
  /** Grid segments of the tile meshes. */
  int meshSegments = TileMesh::defaultSegments;
  /** False to load images only (the renderer builds meshes itself). */
  bool buildMeshes = true;
};

/**
 * Loads imagery tiles and builds their meshes on a pool of worker threads.
 *
 * The render thread says each frame which tiles it wants, in order of
 * priority (setRequests()); work for tiles no longer wanted is dropped,
 * and tiles being loaded that are no longer wanted are told to cancel.
 * Finished tiles wait until the render thread takes them (takeLoaded()),
 * a few per frame. Decoded images are kept in an LRU cache, so a tile
 * evicted from the GPU comes back without asking the provider again.
 *
 * A tile that failed to load (a network error, not "no data") is not
 * delivered; it is left out of the requests for failureRetryDelay and
 * then asked for again when it is still wanted. It doesn't count as
 * outstanding meanwhile.
 *
 * The tiles-ready callback is called on a worker thread after each tile
 * finishes; the application uses it to schedule a frame on the GUI
 * thread.
 *
 * @threadsafe
 */
class TileLoader
{
public:
  /** Starts the workers for @p provider, which must not be null. */
  TileLoader(std::shared_ptr<ImageryProvider> provider,
             const TileLoaderSettings& settings = {});

  /** Cancels outstanding work and joins the workers. */
  ~TileLoader();

  TileLoader(const TileLoader&) = delete;
  TileLoader& operator=(const TileLoader&) = delete;
  TileLoader(TileLoader&&) = delete;
  TileLoader& operator=(TileLoader&&) = delete;

  /** Returns the provider. */
  const std::shared_ptr<ImageryProvider>& provider() const
  {
    return m_provider;
  }

  /**
   * Replaces the wanted tiles with @p requests, most urgent first. Tiles
   * already loading or loaded keep going; tiles loading but no longer
   * wanted are cancelled.
   */
  void setRequests(const std::vector<TileLoadRequest>& requests);

  /** Returns up to @p maximum loaded tiles, oldest first. */
  std::vector<LoadedTile> takeLoaded(int maximum);

  /** Returns how many tiles are queued, loading or waiting to be taken. */
  int outstanding() const;

  /**
   * Sets the function called (on a worker thread) each time a tile has
   * finished loading. Set it before requesting tiles.
   */
  void setTilesReadyCallback(std::function<void()> callback);

  /** Returns the decoded-image cache. */
  LruCache<TileKey, QImage, TileKeyHash>& cache() { return m_cache; }

  /** Returns the number of tiles loaded so far (from cache or provider). */
  int loadedCount() const { return m_loadedCount.load(); }

  /** Returns the number of loads that were cancelled. */
  int cancelledCount() const { return m_cancelledCount.load(); }

  /** Returns how many loads failed (and wait to be retried). */
  int failedCount() const { return m_failedCount.load(); }

  /** Returns the number of worker threads. */
  int threadCount() const { return static_cast<int>(m_workers.size()); }

  /** Forgets every failure, so failed tiles are asked for again at once. */
  void retryFailed();

private:
  void work();
  /** The tile, or nothing if it failed or was cancelled. */
  std::optional<LoadedTile> load(TileKey key,
                                 const std::atomic<bool>& cancelled);

  std::shared_ptr<ImageryProvider> m_provider;
  TileLoaderSettings m_settings;
  LruCache<TileKey, QImage, TileKeyHash> m_cache;

  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<TileKey> m_queue;
  std::unordered_map<TileKey, std::shared_ptr<std::atomic<bool>>, TileKeyHash>
      m_inFlight;
  std::deque<LoadedTile> m_done;
  std::unordered_set<TileKey, TileKeyHash> m_doneKeys;
  /** Failed tiles and when they may be asked for again. */
  std::unordered_map<TileKey, std::chrono::steady_clock::time_point,
                     TileKeyHash>
      m_failed;
  std::function<void()> m_callback;
  bool m_stopping = false;
  std::atomic<int> m_loadedCount{0};
  std::atomic<int> m_cancelledCount{0};
  std::atomic<int> m_failedCount{0};
  std::vector<std::thread> m_workers;
};

} // namespace ts

#endif // TS_TILING_TILELOADER_H
