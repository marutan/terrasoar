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

#include "ts/vector/vectortileloader.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

#include "ts/elevation/heightgrid.h"
#include "ts/geo/geodetic.h"
#include "ts/providers/tileprovider.h"

namespace ts
{

namespace
{

/**
 * Puts the terrain's height and sea level under every vertex of @p mesh,
 * from @p elevation's tiles at @p level (loaded once per mesh).
 */
void liftMesh(VectorTileMesh& mesh, ElevationProvider& elevation, int32_t level,
              const std::atomic<bool>& cancelled)
{
  std::unordered_map<TileKey, std::optional<HeightTile>, TileKeyHash> tiles;
  const std::function<void(VectorVertex&)> lift = [&](VectorVertex& vertex) {
    const double lat = mesh.latDeg(vertex);
    const double lon = mesh.lonDeg(vertex);
    const TileKey key = TileKey::containing(level, Geodetic{lat, lon, 0.0});
    std::unordered_map<TileKey, std::optional<HeightTile>,
                       TileKeyHash>::iterator found = tiles.find(key);
    if (found == tiles.end()) {
      found = tiles.emplace(key, elevation.loadHeights(key, cancelled)).first;
    }
    if (!found->second) {
      return;
    }
    // Clamp onto the tile: a vertex on its edge may round just outside.
    const GeoRect bounds = key.bounds();
    const double clampedLat = std::clamp(lat, bounds.southDeg, bounds.northDeg);
    const double clampedLon = std::clamp(lon, bounds.westDeg, bounds.eastDeg);
    vertex.terrain = {
        static_cast<float>(sampleHeightM(*found->second, clampedLat, clampedLon)
                               .value_or(0.0)),
        static_cast<float>(
            sampleSeaLevelM(*found->second, clampedLat, clampedLon)
                .value_or(0.0))};
  };
  for (VectorVertex& vertex : mesh.triangles) {
    lift(vertex);
  }
  for (VectorSegment& segment : mesh.segments) {
    lift(segment.a);
    lift(segment.b);
  }
}

} // namespace

VectorWorkerPool::VectorWorkerPool(int32_t threads,
                                   const VectorMeshSettings& mesh)
    : m_mesh(mesh)
{
  const int32_t count = std::clamp(threads, 1, 16);
  m_workers.reserve(static_cast<std::size_t>(count));
  for (int32_t i = 0; i < count; ++i) {
    m_workers.emplace_back(&VectorWorkerPool::work, this);
  }
}

VectorWorkerPool::~VectorWorkerPool()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_stopping = true;
  }
  m_wake.notify_all();
  for (std::thread& worker : m_workers) {
    worker.join();
  }
}

void VectorWorkerPool::schedule(VectorTileLoader* loader)
{
  if (!loader->m_scheduled && !loader->m_queue.empty()) {
    loader->m_scheduled = true;
    m_ready.push_back(loader);
  }
}

void VectorWorkerPool::work()
{
  VectorMesher mesher(m_mesh);
  for (;;) {
    VectorTileLoader* loader = nullptr;
    TileKey key;
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_wake.wait(lock, [this]() { return m_stopping || !m_ready.empty(); });
      if (m_stopping) {
        return;
      }
      // One tile from the loader whose turn it is; it goes to the back of
      // the turn if it has more.
      loader = m_ready.front();
      m_ready.pop_front();
      key = loader->m_queue.front();
      loader->m_queue.pop_front();
      loader->m_scheduled = false;
      schedule(loader);
      cancelled = std::make_shared<std::atomic<bool>>(false);
      loader->m_inFlight.emplace(key, cancelled);
      ++loader->m_busy;
    }

    std::optional<VectorTileMesh> mesh = loader->load(key, *cancelled, mesher);

    std::function<void()> callback;
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      loader->m_inFlight.erase(key);
      if (mesh && !cancelled->load() && !loader->m_stopping) {
        loader->m_doneKeys.insert(key);
        loader->m_done.push_back(std::move(*mesh));
        callback = loader->m_callback;
        ++loader->m_loadedCount;
      }
    }
    if (callback) {
      callback();
    }
    {
      // The loader may go once this was its last job.
      const std::lock_guard<std::mutex> lock(m_mutex);
      if (--loader->m_busy == 0 && loader->m_stopping) {
        m_idle.notify_all();
      }
    }
  }
}

VectorTileLoader::VectorTileLoader(std::shared_ptr<FeatureProvider> provider,
                                   const VectorTileLoaderSettings& settings,
                                   std::shared_ptr<ElevationProvider> elevation,
                                   std::shared_ptr<VectorWorkerPool> pool)
    : m_provider(std::move(provider)), m_settings(settings),
      m_elevation(std::move(elevation)),
      m_pool(pool ? std::move(pool)
                  : std::make_shared<VectorWorkerPool>(settings.threads,
                                                       settings.mesh))
{
}

VectorTileLoader::~VectorTileLoader()
{
  std::unique_lock<std::mutex> lock(m_pool->m_mutex);
  m_stopping = true;
  m_queue.clear();
  for (const std::pair<const TileKey, std::shared_ptr<std::atomic<bool>>>& job :
       m_inFlight) {
    job.second->store(true);
  }
  if (m_scheduled) {
    std::deque<VectorTileLoader*>& ready = m_pool->m_ready;
    ready.erase(std::remove(ready.begin(), ready.end(), this), ready.end());
    m_scheduled = false;
  }
  m_pool->m_idle.wait(lock, [this]() { return m_busy == 0; });
}

void VectorTileLoader::setTilesReadyCallback(std::function<void()> callback)
{
  const std::lock_guard<std::mutex> lock(m_pool->m_mutex);
  m_callback = std::move(callback);
}

void VectorTileLoader::setRequests(const std::vector<TileKey>& keys)
{
  {
    const std::lock_guard<std::mutex> lock(m_pool->m_mutex);
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
    if (m_queue.empty()) {
      if (m_scheduled) {
        std::deque<VectorTileLoader*>& ready = m_pool->m_ready;
        ready.erase(std::remove(ready.begin(), ready.end(), this), ready.end());
        m_scheduled = false;
      }
      return;
    }
    m_pool->schedule(this);
  }
  m_pool->m_wake.notify_all();
}

std::vector<VectorTileMesh> VectorTileLoader::takeLoaded(int32_t maximum)
{
  std::vector<VectorTileMesh> meshes;
  const std::lock_guard<std::mutex> lock(m_pool->m_mutex);
  while (!m_done.empty() && static_cast<int32_t>(meshes.size()) < maximum) {
    m_doneKeys.erase(m_done.front().key);
    meshes.push_back(std::move(m_done.front()));
    m_done.pop_front();
  }
  return meshes;
}

int32_t VectorTileLoader::outstanding() const
{
  const std::lock_guard<std::mutex> lock(m_pool->m_mutex);
  return static_cast<int32_t>(m_queue.size() + m_inFlight.size() +
                              m_done.size());
}

std::optional<VectorTileMesh>
VectorTileLoader::load(TileKey key, const std::atomic<bool>& cancelled,
                       VectorMesher& mesher) const
{
  std::optional<VectorTileMesh> mesh;
  const std::optional<FeatureTile> features =
      m_provider->loadFeatures(key, cancelled);
  if (features) {
    mesh = mesher.build(*features, cancelled);
  } else if (!cancelled.load()) {
    // No data here: an empty mesh, so the tile counts as loaded.
    VectorTileMesh empty;
    empty.key = key;
    empty.centreEcef = key.centreEcef();
    empty.centreLonDeg = key.bounds().centreLonDeg();
    empty.centreLatDeg = key.bounds().centreLatDeg();
    mesh = std::move(empty);
  }
  if (mesh) {
    // Keyed by the requested (stored) tile, whatever the source said.
    mesh->key = key;
    if (m_elevation) {
      const int32_t level =
          std::min(key.level + m_settings.terrainLevelsBelow,
                   std::max(m_elevation->maximumLevel(), key.level));
      liftMesh(*mesh, *m_elevation, level, cancelled);
    }
  }
  return mesh;
}

} // namespace ts
