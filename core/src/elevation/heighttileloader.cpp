// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/heighttileloader.h"

#include <algorithm>
#include <utility>

namespace ts
{

HeightTileLoader::HeightTileLoader(std::shared_ptr<ElevationProvider> provider,
                                   int threads)
    : m_provider(std::move(provider))
{
  const int count = std::clamp(threads, 1, 16);
  m_workers.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    m_workers.emplace_back(&HeightTileLoader::work, this);
  }
}

HeightTileLoader::~HeightTileLoader()
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

void HeightTileLoader::setTilesReadyCallback(std::function<void()> callback)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_callback = std::move(callback);
}

void HeightTileLoader::setRequests(const std::vector<TileKey>& keys)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_set<TileKey, TileKeyHash> wanted;
    m_queue.clear();
    for (const TileKey& key : keys) {
      if (wanted.insert(key).second && m_inFlight.count(key) == 0 &&
          m_doneKeys.count(key) == 0) {
        m_queue.push_back(key);
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

std::vector<LoadedHeights> HeightTileLoader::takeLoaded(int maximum)
{
  std::vector<LoadedHeights> loaded;
  const std::lock_guard<std::mutex> lock(m_mutex);
  while (!m_done.empty() && static_cast<int>(loaded.size()) < maximum) {
    m_doneKeys.erase(m_done.front().key);
    loaded.push_back(std::move(m_done.front()));
    m_done.pop_front();
  }
  return loaded;
}

int HeightTileLoader::outstanding() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_queue.size() + m_inFlight.size() + m_done.size());
}

void HeightTileLoader::work()
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
    LoadedHeights result;
    result.key = key;
    result.heights = m_provider->loadHeights(key, *cancelled);
    std::function<void()> callback;
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      m_inFlight.erase(key);
      if (cancelled->load() || m_stopping) {
        continue;
      }
      m_doneKeys.insert(key);
      m_done.push_back(std::move(result));
      callback = m_callback;
    }
    if (callback) {
      callback();
    }
  }
}

} // namespace ts
