// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Loads height tiles from an elevation provider on worker threads.
 */
#ifndef TS_ELEVATION_HEIGHTTILELOADER_H
#define TS_ELEVATION_HEIGHTTILELOADER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ts/providers/tileprovider.h"

namespace ts
{

/** A height tile as loaded: nothing where the source has no data. */
struct LoadedHeights {
  /** The requested key. */
  TileKey key;
  /** The heights, or nothing. */
  std::optional<HeightTile> heights;
};

/**
 * Loads HeightTile objects from an ElevationProvider on worker threads,
 * like VectorTileLoader does features. setRequests() replaces the wanted
 * keys (nearest first) and cancels loads no longer wanted; takeLoaded()
 * hands out what has arrived.
 *
 * @threadsafe
 */
class HeightTileLoader
{
public:
  /** Starts @p threads workers loading from @p provider. */
  HeightTileLoader(std::shared_ptr<ElevationProvider> provider, int threads);
  /** Cancels what is loading and joins the workers. */
  ~HeightTileLoader();

  HeightTileLoader(const HeightTileLoader&) = delete;
  HeightTileLoader& operator=(const HeightTileLoader&) = delete;
  HeightTileLoader(HeightTileLoader&&) = delete;
  HeightTileLoader& operator=(HeightTileLoader&&) = delete;

  /** Returns the provider. */
  const std::shared_ptr<ElevationProvider>& provider() const
  {
    return m_provider;
  }

  /** Sets the keys wanted, most wanted first. */
  void setRequests(const std::vector<TileKey>& keys);

  /** Takes up to @p maximum loaded tiles. */
  std::vector<LoadedHeights> takeLoaded(int maximum);

  /** Returns how many keys are queued, loading or waiting to be taken. */
  int outstanding() const;

  /** Sets the function called (on a worker) each time a tile has loaded. */
  void setTilesReadyCallback(std::function<void()> callback);

private:
  void work();

  std::shared_ptr<ElevationProvider> m_provider;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<TileKey> m_queue;
  std::unordered_map<TileKey, std::shared_ptr<std::atomic<bool>>, TileKeyHash>
      m_inFlight;
  std::deque<LoadedHeights> m_done;
  std::unordered_set<TileKey, TileKeyHash> m_doneKeys;
  std::function<void()> m_callback;
  bool m_stopping = false;
  std::vector<std::thread> m_workers;
};

} // namespace ts

#endif // TS_ELEVATION_HEIGHTTILELOADER_H
