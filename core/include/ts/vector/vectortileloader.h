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
 * Loads feature tiles and builds their meshes on worker threads.
 */
#ifndef TS_VECTOR_VECTORTILELOADER_H
#define TS_VECTOR_VECTORTILELOADER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ts/tiling/tilekey.h"
#include "ts/vector/vectormesh.h"

namespace ts
{

class ElevationProvider;
class FeatureProvider;

/** How a VectorTileLoader works. */
struct VectorTileLoaderSettings {
  /** Worker threads. */
  int32_t threads = 2;
  /** How the meshes are cut. */
  VectorMeshSettings mesh;
  /**
   * The terrain's heights are sampled this many levels below a stored
   * tile's level (at most the elevation source's deepest level).
   */
  int32_t terrainLevelsBelow = 3;
};

/**
 * Loads the stored feature tiles the renderer asks for from one
 * FeatureProvider and turns them into VectorTileMesh objects, on its own
 * worker threads (each with its own VectorMesher, so its own GEOS
 * context). The same model as ts::TileLoader: the wanted list is replaced
 * every frame, tiles no longer wanted are cancelled, finished meshes are
 * taken on the render thread.
 *
 * @threadsafe
 */
class VectorTileLoader
{
public:
  /**
   * Starts the workers. With @p elevation, every vertex gets the
   * terrain's height under it (VectorVertex::terrain), so the features
   * lie on the terrain when it is shown.
   */
  VectorTileLoader(std::shared_ptr<FeatureProvider> provider,
                   const VectorTileLoaderSettings& settings = {},
                   std::shared_ptr<ElevationProvider> elevation = nullptr);

  /** Cancels everything and joins the workers. */
  ~VectorTileLoader();

  VectorTileLoader(const VectorTileLoader&) = delete;
  VectorTileLoader& operator=(const VectorTileLoader&) = delete;
  VectorTileLoader(VectorTileLoader&&) = delete;
  VectorTileLoader& operator=(VectorTileLoader&&) = delete;

  /** Returns the provider. */
  const std::shared_ptr<FeatureProvider>& provider() const
  {
    return m_provider;
  }

  /**
   * Replaces the wanted stored tiles, most urgent first. Tiles in flight
   * that are no longer wanted are cancelled.
   */
  void setRequests(const std::vector<TileKey>& keys);

  /**
   * Takes up to @p maximum finished meshes. A tile without features comes
   * back as a mesh without triangles or segments.
   */
  std::vector<VectorTileMesh> takeLoaded(int32_t maximum);

  /** Returns how many tiles are queued, loading or waiting to be taken. */
  int32_t outstanding() const;

  /** Sets a function called on a worker thread when a mesh is ready. */
  void setTilesReadyCallback(std::function<void()> callback);

  /** Returns how many meshes have been built. */
  int32_t loadedCount() const { return m_loadedCount.load(); }

private:
  void work();

  std::shared_ptr<FeatureProvider> m_provider;
  VectorTileLoaderSettings m_settings;
  std::shared_ptr<ElevationProvider> m_elevation;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  bool m_stopping = false;
  std::deque<TileKey> m_queue;
  std::unordered_map<TileKey, std::shared_ptr<std::atomic<bool>>, TileKeyHash>
      m_inFlight;
  std::deque<VectorTileMesh> m_done;
  std::unordered_set<TileKey, TileKeyHash> m_doneKeys;
  std::function<void()> m_callback;
  std::atomic<int32_t> m_loadedCount{0};
  std::vector<std::thread> m_workers;
};

} // namespace ts

#endif // TS_VECTOR_VECTORTILELOADER_H
