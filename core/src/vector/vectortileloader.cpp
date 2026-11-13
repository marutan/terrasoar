// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/vector/vectortileloader.h"

#include <algorithm>
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
void liftMesh(VectorTileMesh& mesh, ElevationProvider& elevation, int level,
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

VectorTileLoader::VectorTileLoader(std::shared_ptr<FeatureProvider> provider,
                                   const VectorTileLoaderSettings& settings,
                                   std::shared_ptr<ElevationProvider> elevation)
    : m_provider(std::move(provider)), m_settings(settings),
      m_elevation(std::move(elevation))
{
  const int threads = std::clamp(m_settings.threads, 1, 16);
  m_workers.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    m_workers.emplace_back(&VectorTileLoader::work, this);
  }
}

VectorTileLoader::~VectorTileLoader()
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

void VectorTileLoader::setTilesReadyCallback(std::function<void()> callback)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_callback = std::move(callback);
}

void VectorTileLoader::setRequests(const std::vector<TileKey>& keys)
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

std::vector<VectorTileMesh> VectorTileLoader::takeLoaded(int maximum)
{
  std::vector<VectorTileMesh> meshes;
  const std::lock_guard<std::mutex> lock(m_mutex);
  while (!m_done.empty() && static_cast<int>(meshes.size()) < maximum) {
    m_doneKeys.erase(m_done.front().key);
    meshes.push_back(std::move(m_done.front()));
    m_done.pop_front();
  }
  return meshes;
}

int VectorTileLoader::outstanding() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_queue.size() + m_inFlight.size() + m_done.size());
}

void VectorTileLoader::work()
{
  VectorMesher mesher(m_settings.mesh);
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

    std::optional<VectorTileMesh> mesh;
    const std::optional<FeatureTile> features =
        m_provider->loadFeatures(key, *cancelled);
    if (features) {
      mesh = mesher.build(*features, *cancelled);
    } else if (!cancelled->load()) {
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
        const int level =
            std::min(key.level + m_settings.terrainLevelsBelow,
                     std::max(m_elevation->maximumLevel(), key.level));
        liftMesh(*mesh, *m_elevation, level, *cancelled);
      }
    }

    std::function<void()> callback;
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      m_inFlight.erase(key);
      if (!mesh || cancelled->load() || m_stopping) {
        continue;
      }
      m_doneKeys.insert(key);
      m_done.push_back(std::move(*mesh));
      callback = m_callback;
    }
    ++m_loadedCount;
    if (callback) {
      callback();
    }
  }
}

} // namespace ts
