// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Loads feature tiles and builds their meshes on worker threads.
 */
#ifndef TS_VECTOR_VECTORTILELOADER_H
#define TS_VECTOR_VECTORTILELOADER_H

#include <atomic>
#include <condition_variable>
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

class FeatureProvider;

/** How a VectorTileLoader works. */
struct VectorTileLoaderSettings {
  /** Worker threads. */
  int threads = 2;
  /** How the meshes are cut. */
  VectorMeshSettings mesh;
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
  /** Starts the workers. */
  VectorTileLoader(std::shared_ptr<FeatureProvider> provider,
                   const VectorTileLoaderSettings& settings = {});

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
  std::vector<VectorTileMesh> takeLoaded(int maximum);

  /** Returns how many tiles are queued, loading or waiting to be taken. */
  int outstanding() const;

  /** Sets a function called on a worker thread when a mesh is ready. */
  void setTilesReadyCallback(std::function<void()> callback);

  /** Returns how many meshes have been built. */
  int loadedCount() const { return m_loadedCount.load(); }

private:
  void work();

  std::shared_ptr<FeatureProvider> m_provider;
  VectorTileLoaderSettings m_settings;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  bool m_stopping = false;
  std::deque<TileKey> m_queue;
  std::unordered_map<TileKey, std::shared_ptr<std::atomic<bool>>, TileKeyHash>
      m_inFlight;
  std::deque<VectorTileMesh> m_done;
  std::unordered_set<TileKey, TileKeyHash> m_doneKeys;
  std::function<void()> m_callback;
  std::atomic<int> m_loadedCount{0};
  std::vector<std::thread> m_workers;
};

} // namespace ts

#endif // TS_VECTOR_VECTORTILELOADER_H
