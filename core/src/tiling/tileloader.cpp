// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/tiling/tileloader.h"

#include <algorithm>
#include <utility>

#include "ts/providers/tileprovider.h"

namespace ts
{

TileLoader::TileLoader(std::shared_ptr<ImageryProvider> provider,
                       const TileLoaderSettings& settings)
    : m_provider(std::move(provider)), m_settings(settings),
      m_cache(settings.cacheBytes)
{
  const int threads = std::clamp(
      std::max(m_settings.threads,
               m_provider ? m_provider->preferredLoaderThreads() : 0),
      1, 16);
  m_workers.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    m_workers.emplace_back(&TileLoader::work, this);
  }
}

TileLoader::~TileLoader()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_stopping = true;
    m_queue.clear();
    for (const std::pair<const TileKey, std::shared_ptr<std::atomic<bool>>>&
             job : m_inFlight) {
      job.second->store(true);
    }
  }
  m_wake.notify_all();
  for (std::thread& worker : m_workers) {
    worker.join();
  }
}

void TileLoader::setTilesReadyCallback(std::function<void()> callback)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_callback = std::move(callback);
}

void TileLoader::setRequests(const std::vector<TileLoadRequest>& requests)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    std::unordered_set<TileKey, TileKeyHash> wanted;
    m_queue.clear();
    for (const TileLoadRequest& request : requests) {
      if (!wanted.insert(request.key).second) {
        continue;
      }
      const std::unordered_map<TileKey, std::chrono::steady_clock::time_point,
                               TileKeyHash>::iterator failed =
          m_failed.find(request.key);
      if (failed != m_failed.end()) {
        if (now < failed->second) {
          continue; // not yet
        }
        m_failed.erase(failed);
      }
      if (m_inFlight.count(request.key) == 0 &&
          m_doneKeys.count(request.key) == 0) {
        m_queue.push_back(request.key);
      }
    }
    for (const std::pair<const TileKey, std::shared_ptr<std::atomic<bool>>>&
             job : m_inFlight) {
      if (wanted.count(job.first) == 0) {
        job.second->store(true);
      }
    }
  }
  m_wake.notify_all();
}

std::vector<LoadedTile> TileLoader::takeLoaded(int maximum)
{
  std::vector<LoadedTile> tiles;
  const std::lock_guard<std::mutex> lock(m_mutex);
  while (!m_done.empty() && static_cast<int>(tiles.size()) < maximum) {
    m_doneKeys.erase(m_done.front().key);
    tiles.push_back(std::move(m_done.front()));
    m_done.pop_front();
  }
  return tiles;
}

int TileLoader::outstanding() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_queue.size() + m_inFlight.size() + m_done.size());
}

void TileLoader::retryFailed()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_failed.clear();
}

void TileLoader::work()
{
  for (;;) {
    TileKey key;
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_wake.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
      if (m_stopping) {
        return;
      }
      key = m_queue.front();
      m_queue.pop_front();
      cancelled = std::make_shared<std::atomic<bool>>(false);
      m_inFlight.emplace(key, cancelled);
    }

    std::optional<LoadedTile> tile = load(key, *cancelled);

    std::function<void()> callback;
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      m_inFlight.erase(key);
      if (cancelled->load() || m_stopping) {
        ++m_cancelledCount;
        continue;
      }
      if (!tile) {
        // Failed: try again later if it is still wanted then.
        m_failed[key] =
            std::chrono::steady_clock::now() + m_settings.failureRetryDelay;
        ++m_failedCount;
        continue;
      }
      m_doneKeys.insert(key);
      m_done.push_back(std::move(*tile));
      callback = m_callback;
    }
    ++m_loadedCount;
    if (callback) {
      callback();
    }
  }
}

std::optional<LoadedTile> TileLoader::load(TileKey key,
                                           const std::atomic<bool>& cancelled)
{
  LoadedTile tile;
  tile.key = key;
  const std::optional<QImage> cached = m_cache.find(key);
  if (cached) {
    tile.image = *cached;
  } else {
    const ImageLoadResult result = m_provider->loadTileImage(key, cancelled);
    if (result.outcome == ImageLoadResult::Outcome::Failed ||
        result.outcome == ImageLoadResult::Outcome::Cancelled) {
      return std::nullopt;
    }
    tile.image = result.image;
    if (!tile.image.isNull() && !cancelled.load()) {
      m_cache.insert(key, tile.image,
                     static_cast<std::size_t>(tile.image.sizeInBytes()));
    }
  }
  if (m_settings.buildMeshes && !cancelled.load()) {
    tile.mesh = buildTileMesh(key, m_settings.meshSegments);
  }
  return tile;
}

} // namespace ts
