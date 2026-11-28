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
#include <optional>
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
  /** Worker threads, when the loader has its own (no VectorWorkerPool). */
  int32_t threads = 2;
  /** How the meshes are cut (a pool's own settings when it has one). */
  VectorMeshSettings mesh;
  /**
   * The terrain's heights are sampled this many levels below a stored
   * tile's level (at most the elevation source's deepest level).
   */
  int32_t terrainLevelsBelow = 3;
};

class VectorTileLoader;

/**
 * Worker threads shared by VectorTileLoader objects, each thread with its
 * own VectorMesher (so its own GEOS context): many sources, one set of
 * threads. Loaders with work take turns, one tile each. Every loader
 * using a pool must be gone before it is.
 *
 * @threadsafe
 */
class VectorWorkerPool
{
public:
  /** Starts @p threads workers (1 to 16) meshing with @p mesh. */
  explicit VectorWorkerPool(int32_t threads = 2,
                            const VectorMeshSettings& mesh = {});

  /** Joins the workers. */
  ~VectorWorkerPool();

  VectorWorkerPool(const VectorWorkerPool&) = delete;
  VectorWorkerPool& operator=(const VectorWorkerPool&) = delete;
  VectorWorkerPool(VectorWorkerPool&&) = delete;
  VectorWorkerPool& operator=(VectorWorkerPool&&) = delete;

  /** Returns how many worker threads there are. */
  int32_t threadCount() const { return static_cast<int32_t>(m_workers.size()); }

private:
  friend class VectorTileLoader;

  void work();
  /** Puts @p loader in the turn, if not already. With the lock held. */
  void schedule(VectorTileLoader* loader);

  VectorMeshSettings m_mesh;
  std::mutex m_mutex;
  /** Workers wait for work on it. */
  std::condition_variable m_wake;
  /** Loaders going away wait on it for their jobs to finish. */
  std::condition_variable m_idle;
  bool m_stopping = false;
  /** Loaders with tiles queued, in turn. */
  std::deque<VectorTileLoader*> m_ready;
  std::vector<std::thread> m_workers;
};

/**
 * Loads the stored feature tiles the renderer asks for from one
 * FeatureProvider and turns them into VectorTileMesh objects, on worker
 * threads: a VectorWorkerPool's, or its own. The same model as
 * ts::TileLoader: the wanted list is replaced every frame, tiles no longer
 * wanted are cancelled, finished meshes are taken on the render thread.
 *
 * @threadsafe
 */
class VectorTileLoader
{
public:
  /**
   * Loads on @p pool's workers, or (null) on workers of its own. With
   * @p elevation, every vertex gets the terrain's height under it
   * (VectorVertex::terrain), so the features lie on the terrain when it
   * is shown.
   */
  VectorTileLoader(std::shared_ptr<FeatureProvider> provider,
                   const VectorTileLoaderSettings& settings = {},
                   std::shared_ptr<ElevationProvider> elevation = nullptr,
                   std::shared_ptr<VectorWorkerPool> pool = nullptr);

  /** Cancels everything and waits for its tiles in progress. */
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
  friend class VectorWorkerPool;

  /** Loads and meshes @p key (on a worker, without the lock). */
  std::optional<VectorTileMesh> load(TileKey key,
                                     const std::atomic<bool>& cancelled,
                                     VectorMesher& mesher) const;

  std::shared_ptr<FeatureProvider> m_provider;
  VectorTileLoaderSettings m_settings;
  std::shared_ptr<ElevationProvider> m_elevation;
  std::shared_ptr<VectorWorkerPool> m_pool;
  // The rest is guarded by m_pool->m_mutex.
  bool m_stopping = false;
  /** In the pool's turn. */
  bool m_scheduled = false;
  /** Jobs of this loader a worker is doing (until its callback returns). */
  int32_t m_busy = 0;
  std::deque<TileKey> m_queue;
  std::unordered_map<TileKey, std::shared_ptr<std::atomic<bool>>, TileKeyHash>
      m_inFlight;
  std::deque<VectorTileMesh> m_done;
  std::unordered_set<TileKey, TileKeyHash> m_doneKeys;
  std::function<void()> m_callback;
  std::atomic<int32_t> m_loadedCount{0};
};

} // namespace ts

#endif // TS_VECTOR_VECTORTILELOADER_H
