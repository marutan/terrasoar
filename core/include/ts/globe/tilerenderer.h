// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Draws imagery tiles of the quadtree over the globe.
 */
#ifndef TS_GLOBE_TILERENDERER_H
#define TS_GLOBE_TILERENDERER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QString>

#include "ts/render/renderdevice.h"
#include "ts/tiling/tilekey.h"
#include "ts/tiling/tileloader.h"
#include "ts/tiling/tileselector.h"

namespace ts
{

class Camera;
class ImageryProvider;

/** Budgets and quality of the tile renderer. */
struct TileRendererSettings {
  /** Level-of-detail selection. */
  TileSelectionSettings selection;
  /** Loader threads, cache and mesh density. */
  TileLoaderSettings loader;
  /** Budget of tile textures and meshes on the GPU (common use: 96 MB). */
  std::size_t gpuBytes = std::size_t{96} * 1024 * 1024;
  /** Most tiles uploaded to the GPU per frame. */
  int uploadsPerFrame = 8;
};

/** What the last frame did, for the debug overlay and tests. */
struct TileStatistics {
  /** Tiles drawn. */
  int rendered = 0;
  /** Tiles the selection visited. */
  int visited = 0;
  /** Tiles requested from the loader. */
  int requested = 0;
  /** Tiles uploaded this frame. */
  int uploaded = 0;
  /** Tiles on the GPU. */
  int resident = 0;
  /** Bytes on the GPU. */
  std::size_t gpuBytes = 0;
  /** Deepest level drawn. */
  int deepestLevel = -1;
};

/**
 * Selects, loads, uploads and draws the imagery tiles of one provider.
 *
 * Each frame: prepare() selects the tiles for the camera (TileSelector),
 * passes the missing ones to the loader, uploads a few finished ones
 * (the GPU upload queue) and evicts the least recently drawn tiles over
 * the GPU budget; draw(), inside the globe's pass, draws the selected
 * tiles. A tile the provider has no image for is drawn as nothing, so the
 * plain globe shows there.
 *
 * Expects the globe renderer's "Frame" (binding 0) and "Globe" (binding
 * 1) uniform blocks to be bound during draw().
 *
 * @renderthread
 */
class TileRenderer
{
public:
  /** Creates a renderer drawing through @p device. */
  explicit TileRenderer(RenderDevice& device,
                        const TileRendererSettings& settings = {});

  /** Releases GPU resources and stops the loader. */
  ~TileRenderer();

  TileRenderer(const TileRenderer&) = delete;
  TileRenderer& operator=(const TileRenderer&) = delete;
  TileRenderer(TileRenderer&&) = delete;
  TileRenderer& operator=(TileRenderer&&) = delete;

  /** Creates shaders and shared buffers; false and @p errorString on failure.
   */
  bool initialise(QString* errorString);

  /**
   * Sets the imagery source, or null for none. Drops all tiles of the
   * previous one.
   */
  void setImageryProvider(std::shared_ptr<ImageryProvider> provider);

  /** Returns the imagery source. */
  std::shared_ptr<ImageryProvider> imageryProvider() const;

  /**
   * Sets the function called (on a worker thread) when tiles finish
   * loading, so the application can schedule a frame.
   */
  void setTilesReadyCallback(std::function<void()> callback);

  /** Selects, requests, uploads and evicts for @p camera; outside a pass. */
  void prepare(const Camera& camera);

  /** Draws the selected tiles; inside the globe's pass. */
  void draw();

  /**
   * Returns true while more frames are needed without input: tiles are
   * loading or waiting for upload.
   */
  bool isBusy() const;

  /** Returns the statistics of the last prepare() and draw(). */
  const TileStatistics& statistics() const { return m_statistics; }

  /** Returns true if @p key is on the GPU (drawable). */
  bool isResident(TileKey key) const;

private:
  struct GpuTile;

  void upload(const LoadedTile& tile);
  void evict();

  RenderDevice& m_device;
  TileRendererSettings m_settings;
  std::shared_ptr<ImageryProvider> m_provider;
  std::unique_ptr<TileLoader> m_loader;
  std::function<void()> m_callback;
  TileSelector m_selector;

  std::unique_ptr<ShaderProgram> m_shader;
  std::unique_ptr<Pipeline> m_pipeline;
  std::unique_ptr<GpuBuffer> m_indices;
  int m_indexCount = 0;
  std::unique_ptr<GpuBuffer> m_tileUniforms;
  std::size_t m_tileUniformStride = 0;
  std::size_t m_tileUniformCapacity = 0;

  std::unordered_map<TileKey, std::unique_ptr<GpuTile>, TileKeyHash> m_tiles;
  std::vector<TileKey> m_render;
  std::uint64_t m_frame = 0;
  std::size_t m_gpuBytes = 0;
  TileStatistics m_statistics;
};

} // namespace ts

#endif // TS_GLOBE_TILERENDERER_H
