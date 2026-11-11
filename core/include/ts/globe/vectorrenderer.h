// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Draws vector layers (filled polygons, outlines, lines) over the globe.
 */
#ifndef TS_GLOBE_VECTORRENDERER_H
#define TS_GLOBE_VECTORRENDERER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QString>

#include "ts/render/renderdevice.h"
#include "ts/tiling/tilekey.h"
#include "ts/tiling/tileselector.h"
#include "ts/vector/vectorstyle.h"
#include "ts/vector/vectortileloader.h"

namespace ts
{

class Camera;
class FeatureProvider;

/** One vector layer to draw, in tree order (bottom first). */
struct VectorLayerDraw {
  /** What the layer is. */
  enum class Kind {
    Features,  ///< Features from a provider.
    Graticule, ///< The generated graticule (drawn by GlobeRenderer).
  };
  /** What the layer is. */
  Kind kind = Kind::Features;
  /** The source (Features); layers with the same provider share tiles. */
  std::shared_ptr<FeatureProvider> provider;
  /** Colours and widths. */
  VectorStyle style;
  /** Effective opacity (product of the tree's). */
  double alpha = 1.0;
};

/** Counters for the debug overlay and tests. */
struct VectorStatistics {
  /** Stored feature tiles drawn (once per layer and tile, or per piece). */
  int tilesDrawn = 0;
  /** Draw calls. */
  int drawCalls = 0;
  /** Meshes on the GPU. */
  int resident = 0;
  /** Meshes uploaded this frame. */
  int uploaded = 0;
  /** Bytes on the GPU. */
  std::size_t gpuBytes = 0;
  /** Triangles drawn. */
  long long triangles = 0;
  /** Outline segments drawn. */
  long long segments = 0;
};

/**
 * The default selection for vectors: tiles counted as 1024 pixels wide,
 * two levels coarser than imagery. That matches the base pack's shoreline,
 * whose levels of detail are simplified to about a pixel of a tile shown
 * 1024 pixels wide (GSHHG's 25, 5, 1 and 0.2 km at levels 0, 2, 4 and 6),
 * so outlines carry no detail far below a pixel.
 */
inline TileSelectionSettings defaultVectorSelection()
{
  TileSelectionSettings selection;
  selection.tileSizePx = 1024;
  return selection;
}

/** Tuning. */
struct VectorRendererSettings {
  /** Selection of the tiles to draw (per source). */
  TileSelectionSettings selection = defaultVectorSelection();
  /** Loading and meshing. */
  VectorTileLoaderSettings loader;
  /** Meshes uploaded per frame, all sources. */
  int uploadsPerFrame = 8;
  /** GPU memory for meshes before the least recently used go. */
  std::size_t gpuBytes = std::size_t{64} * 1024 * 1024;
};

/**
 * Draws vector layers over the globe: each source's stored feature tiles
 * are selected for the camera like imagery tiles, loaded and meshed on
 * worker threads (VectorTileLoader), uploaded, and drawn as filled
 * triangles and antialiased outline segments of a fixed width in pixels.
 *
 * Layers draw without the depth test, over everything drawn before them;
 * fragments on the far side of the ellipsoid are discarded, and each draw
 * is clipped to its tile's rectangle, so where tiles of two levels of
 * detail meet nothing is drawn twice. See docs/design/vector-rendering.md.
 *
 * Like tiles, vertices have a position on the globe and one on the flat
 * map, blended by the camera's morph factor in the vertex shader. While
 * the factor is above 0 the far side is hidden by the depth the tiles
 * wrote instead (the globe's "far side" means nothing on a map), and what
 * lies near the edge where the world wraps around is drawn on both
 * sides. See docs/design/morph.md.
 *
 * @renderthread
 */
class VectorRenderer
{
public:
  /** Uses @p device, which must outlive the renderer. */
  explicit VectorRenderer(RenderDevice& device,
                          const VectorRendererSettings& settings = {});

  /** Stops the loaders and releases GPU resources. */
  ~VectorRenderer();

  VectorRenderer(const VectorRenderer&) = delete;
  VectorRenderer& operator=(const VectorRenderer&) = delete;
  VectorRenderer(VectorRenderer&&) = delete;
  VectorRenderer& operator=(VectorRenderer&&) = delete;

  /** Creates the shaders and pipelines. False and @p errorString on error. */
  bool initialise(QString* errorString);

  /**
   * Sets the layers, bottom to top. Graticule entries keep their place
   * (drawLayer() does nothing for them). Sources no longer used are
   * dropped with their meshes.
   */
  void setLayers(const std::vector<VectorLayerDraw>& layers);

  /** Sets the function called (on a worker) when meshes are ready. */
  void setTilesReadyCallback(std::function<void()> callback);

  /**
   * Uploads finished meshes, selects tiles, requests what is missing and
   * evicts over budget, for @p camera. Outside a pass.
   */
  void prepare(const Camera& camera);

  /**
   * Draws layer @p index (of setLayers()) inside the current pass, with
   * the "Frame" and "Globe" uniform blocks bound.
   */
  void drawLayer(std::size_t index);

  /** Returns true while meshes are loading or waiting for upload. */
  bool isBusy() const;

  /** Returns this frame's counters. */
  VectorStatistics statistics() const { return m_statistics; }

private:
  struct GpuMesh;
  struct Source;
  struct DrawItem {
    const GpuMesh* mesh = nullptr;
    GeoRect clip;
    /** Which copy of the world on the flat map: its shift in x, metres. */
    double mapShiftM = 0.0;
  };

  std::unique_ptr<GpuMesh> createGpuMesh(const VectorTileMesh& mesh) const;
  void upload();
  void select(Source& source, const Camera& camera) const;
  static void addItem(Source& source, const Camera& camera, const GpuMesh* mesh,
                      const GeoRect& clip);
  void evict();
  void writeUniforms(const Camera& camera);

  RenderDevice& m_device;
  VectorRendererSettings m_settings;
  std::unique_ptr<ShaderProgram> m_fillShader;
  std::unique_ptr<ShaderProgram> m_lineShader;
  std::unique_ptr<Pipeline> m_fillPipeline;
  std::unique_ptr<Pipeline> m_linePipeline;
  std::unique_ptr<Pipeline> m_fillMapPipeline;
  std::unique_ptr<Pipeline> m_lineMapPipeline;
  double m_morph = 0.0;
  std::unique_ptr<GpuBuffer> m_uniforms;
  std::size_t m_uniformCapacity = 0;
  std::size_t m_uniformStride = 0;
  std::vector<std::unique_ptr<Source>> m_sources;
  /** Per layer: its source (null for the graticule) and style. */
  std::vector<std::pair<Source*, VectorLayerDraw>> m_layers;
  /** Per layer: the first uniform block of its draws this frame. */
  std::vector<std::size_t> m_layerUniformStart;
  std::function<void()> m_callback;
  std::uint64_t m_frame = 0;
  VectorStatistics m_statistics;
};

} // namespace ts

#endif // TS_GLOBE_VECTORRENDERER_H
