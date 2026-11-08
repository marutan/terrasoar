// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Draws the imagery layers on the tile quadtree over the globe.
 */
#ifndef TS_GLOBE_TILERENDERER_H
#define TS_GLOBE_TILERENDERER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QSize>
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
  /** Loader threads and decoded-image cache, per imagery source. */
  TileLoaderSettings loader;
  /** Budget of tile textures and meshes on the GPU (common use: 96 MB). */
  std::size_t gpuBytes = std::size_t{96} * 1024 * 1024;
  /** Most imagery tiles uploaded to the GPU per frame, all sources. */
  int uploadsPerFrame = 8;
  /** Most tile meshes built per frame. */
  int meshBuildsPerFrame = 32;
  /**
   * Draw composite groups offscreen and fade them as one image. When
   * false (the software quality tier), a group's opacity multiplies into
   * its layers instead.
   */
  bool groupCompositing = true;
};

/** One imagery layer to draw, bottom to top. */
struct ImageryLayerDraw {
  /** The source; layers with the same provider share its tiles. */
  std::shared_ptr<ImageryProvider> provider;
  /** Opacity within its group (or overall without one). */
  double alpha = 1.0;
  /** Index into the group opacities, or −1. Group members are contiguous. */
  int group = -1;
};

/** What the last frame did, for the debug overlay and tests. */
struct TileStatistics {
  /** Tiles drawn (geometry). */
  int rendered = 0;
  /** Tiles the selection visited. */
  int visited = 0;
  /** Imagery tiles requested from the loaders. */
  int requested = 0;
  /** Imagery tiles uploaded this frame. */
  int uploaded = 0;
  /** Imagery tiles on the GPU. */
  int resident = 0;
  /** Bytes on the GPU (textures and meshes). */
  std::size_t gpuBytes = 0;
  /** Deepest level drawn. */
  int deepestLevel = -1;
  /** Draw calls for tiles (a tile is drawn once per batch of 8 layers). */
  int drawCalls = 0;
  /** Composite groups drawn offscreen. */
  int groupsComposited = 0;
};

/**
 * Selects the quadtree tiles for the camera and draws the imagery layers
 * on them.
 *
 * Geometry and imagery are separate. Tile meshes depend only on the key,
 * so they are built on the render thread as the selection asks for them,
 * and the selection refines as soon as they exist. Each imagery source
 * (one per provider, shared by every layer using it) has its own loader
 * and GPU textures; for each drawn tile a layer uses its own image if it
 * is loaded, else the nearest loaded ancestor's, stretched with a texture
 * transform, so layers sharpen as their tiles arrive and a change of
 * order or opacity shows in the next frame.
 *
 * Layers are blended bottom to top in the tile fragment shader, up to 8
 * per draw (a tile is drawn again for each further 8). A composite group
 * is drawn into an offscreen target and faded onto the scene as one
 * image. See docs/design/tiling.md and docs/design/layer-model.md.
 *
 * Expects the globe's "Frame" (binding 0) and "Globe" (binding 1) uniform
 * blocks; draw() re-binds them through a callback after switching
 * targets.
 *
 * @renderthread
 */
class TileRenderer
{
public:
  /** Creates a renderer drawing through @p device. */
  explicit TileRenderer(RenderDevice& device,
                        const TileRendererSettings& settings = {});

  /** Stops the loaders and releases GPU resources. */
  ~TileRenderer();

  TileRenderer(const TileRenderer&) = delete;
  TileRenderer& operator=(const TileRenderer&) = delete;
  TileRenderer(TileRenderer&&) = delete;
  TileRenderer& operator=(TileRenderer&&) = delete;

  /** Creates shaders and shared buffers; false and @p errorString on failure.
   */
  bool initialise(QString* errorString);

  /**
   * Sets the imagery layers, bottom to top, and the opacities of their
   * composite groups. Sources no longer used are dropped with their tiles.
   */
  void setLayers(const std::vector<ImageryLayerDraw>& layers,
                 const std::vector<double>& groupAlphas);

  /** Shows only @p provider at full opacity (or nothing if null). */
  void setImageryProvider(std::shared_ptr<ImageryProvider> provider);

  /** Turns composite groups on or off (see TileRendererSettings). */
  void setGroupCompositing(bool enabled);

  /**
   * Sets the function called (on a worker thread) when imagery tiles
   * finish loading, so the application can schedule a frame.
   */
  void setTilesReadyCallback(std::function<void()> callback);

  /**
   * Selects tiles, builds meshes, uploads finished imagery, requests what
   * is missing and evicts over budget, for @p camera. Outside a pass.
   */
  void prepare(const Camera& camera);

  /**
   * Draws the layers on the selected tiles inside @p mainPass (begun by
   * the caller). Composite groups leave and resume that pass; after each
   * resumption @p rebindUniforms is called to bind the Frame and Globe
   * blocks again. Ends inside @p mainPass.
   */
  void draw(const PassDesc& mainPass,
            const std::function<void()>& rebindUniforms);

  /**
   * Returns true while frames are needed without input: tiles loading or
   * waiting for upload, or meshes still to build.
   */
  bool isBusy() const;

  /** Returns the statistics of the last prepare() and draw(). */
  const TileStatistics& statistics() const { return m_statistics; }

  /** Returns the number of distinct imagery sources (shared providers). */
  int sourceCount() const { return static_cast<int>(m_sources.size()); }

private:
  struct GpuMesh;
  struct GpuImage;
  struct Source;
  struct Layer {
    Source* source = nullptr;
    double alpha = 1.0;
    int group = -1;
  };
  struct DrawTile;
  /** A run of layers, by index into m_layers. */
  struct LayerRange {
    std::size_t first = 0;
    std::size_t count = 0;
  };

  void uploadImages();
  void buildMeshes(const std::vector<TileLoadRequest>& wanted);
  void requestImages();
  void bindImages();
  void evict();
  double layerAlpha(const Layer& layer, bool hasImage) const;
  std::vector<bool> writeBatchUniforms(LayerRange batch);
  void drawLayers(LayerRange layers);
  void composite(double alpha);

  RenderDevice& m_device;
  TileRendererSettings m_settings;
  std::function<void()> m_callback;
  TileSelector m_selector;

  std::unique_ptr<ShaderProgram> m_shader;
  std::unique_ptr<Pipeline> m_pipeline;
  std::unique_ptr<ShaderProgram> m_compositeShader;
  std::unique_ptr<Pipeline> m_compositePipeline;
  std::unique_ptr<GpuBuffer> m_indices;
  int m_indexCount = 0;
  std::unique_ptr<GpuTexture> m_emptyTexture;
  std::unique_ptr<GpuBuffer> m_tileUniforms;
  std::size_t m_tileUniformStride = 0;
  std::size_t m_tileUniformCapacity = 0;
  std::unique_ptr<GpuBuffer> m_compositeUniforms;
  std::unique_ptr<RenderTarget> m_groupTarget;

  std::unordered_map<TileKey, std::unique_ptr<GpuMesh>, TileKeyHash> m_meshes;
  std::vector<std::unique_ptr<Source>> m_sources;
  std::vector<Layer> m_layers;
  std::vector<double> m_groupAlphas;
  std::vector<DrawTile> m_drawTiles;
  bool m_meshesPending = false;
  std::uint64_t m_frame = 0;
  std::size_t m_meshBytes = 0;
  TileStatistics m_statistics;
};

} // namespace ts

#endif // TS_GLOBE_TILERENDERER_H
