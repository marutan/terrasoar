// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Draws the WGS 84 globe and point markers through a RenderDevice.
 */
#ifndef TS_GLOBE_GLOBERENDERER_H
#define TS_GLOBE_GLOBERENDERER_H

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <QString>

#include "ts/maths/vec3.h"
#include "ts/render/renderdevice.h"

namespace ts
{

class Camera;
class ImageryProvider;
class TileRenderer;
struct ImageryLayerDraw;
struct TileRendererSettings;
struct TileStatistics;

/**
 * A round marker drawn at a fixed point, with a size in pixels.
 *
 * @reentrant
 */
struct GlobeMarker {
  /** Where the marker is, ECEF metres. */
  Vec3 positionEcef{};
  /** Colour, 8-bit RGBA, not premultiplied. */
  std::array<std::uint8_t, 4> rgba = {255, 255, 255, 255};
  /** Radius in logical pixels. */
  double radiusPx = 4.0;
};

/** Colours and options for the globe. */
struct GlobeStyle {
  /** Clear colour behind the globe, linear RGB in [0, 1]. */
  std::array<float, 3> backgroundRgb = {0.02F, 0.03F, 0.06F};
  /** Surface colour, RGB in [0, 1]. */
  std::array<float, 3> surfaceRgb = {0.11F, 0.29F, 0.52F};
  /** Graticule colour, RGB in [0, 1]. */
  std::array<float, 3> graticuleRgb = {0.80F, 0.88F, 1.00F};
  /** True to draw latitude and longitude lines. */
  bool graticule = true;
  /** Graticule line width in logical pixels. */
  float graticuleWidthPx = 1.0F;
};

/**
 * Draws the globe: the WGS 84 ellipsoid, shaded and with a graticule, and
 * markers on top.
 *
 * The ellipsoid is not a mesh. A full-screen triangle casts one ray per
 * pixel and intersects it with the exact ellipsoid in the fragment
 * shader, in the same numerically stable form as intersectEllipsoid(), so
 * the drawn surface is the one picking uses, at every zoom. The precision
 * near the surface comes from the quadratic's constant term
 * (|eye/radii|² − 1), which is computed in double on the CPU. Depth is
 * logarithmic (log2(1 + w) / log2(1 + far)) so that tiles drawn later in
 * the same pass can test against the globe.
 *
 * Markers are instanced quads positioned relative to the eye from
 * high/low float pairs (see SplitFloat), so they stay still to well under
 * a pixel when the eye is a metre above the ground.
 *
 * See docs/design/camera-and-precision.md and
 * docs/design/renderer-backend.md.
 *
 * @renderthread
 */
class GlobeRenderer
{
public:
  /** Creates a renderer drawing through @p device, which must outlive it. */
  explicit GlobeRenderer(RenderDevice& device);

  /** Same, with tile budgets and quality from @p tileSettings. */
  GlobeRenderer(RenderDevice& device, const TileRendererSettings& tileSettings);

  /** Releases the GPU resources; the device's context must be current. */
  ~GlobeRenderer();

  GlobeRenderer(const GlobeRenderer&) = delete;
  GlobeRenderer& operator=(const GlobeRenderer&) = delete;
  GlobeRenderer(GlobeRenderer&&) = delete;
  GlobeRenderer& operator=(GlobeRenderer&&) = delete;

  /**
   * Creates the shaders, pipelines and buffers. Returns false and sets
   * @p errorString (if not null) on failure.
   */
  bool initialise(QString* errorString);

  /** Returns true after a successful initialise(). */
  bool isInitialised() const;

  /** Sets the colours and options. */
  void setStyle(const GlobeStyle& style);

  /** Returns the colours and options. */
  const GlobeStyle& style() const { return m_style; }

  /** Replaces the markers. */
  void setMarkers(const std::vector<GlobeMarker>& markers);

  /**
   * Sets the imagery drawn on the globe from the tile quadtree, or null
   * for the plain globe. Only after initialise().
   */
  void setImageryProvider(std::shared_ptr<ImageryProvider> provider);

  /**
   * Sets the imagery layers drawn on the globe, bottom to top, with the
   * opacities of their composite groups (see TileRenderer::setLayers()).
   */
  void setImageryLayers(const std::vector<ImageryLayerDraw>& layers,
                        const std::vector<double>& groupAlphas);

  /**
   * Turns composite groups on or off; off, a group's opacity multiplies
   * into its layers (the software quality tier).
   */
  void setGroupCompositing(bool enabled);

  /**
   * Sets the function called (on a worker thread) when imagery tiles
   * finish loading, so the application can schedule a frame.
   */
  void setTilesReadyCallback(std::function<void()> callback);

  /**
   * Returns true while frames are needed without input because tiles are
   * still loading or waiting for upload.
   */
  bool isBusy() const;

  /** Returns what the tile renderer did in the last frame. */
  TileStatistics tileStatistics() const;

  /**
   * Draws a frame as seen by @p camera into @p pass (whose clear colour is
   * replaced by the style's background).
   *
   * @param camera The view, with its viewport in logical pixels.
   * @param pass Target and viewport in device pixels.
   * @param devicePixelRatio Device pixels per logical pixel.
   */
  void render(const Camera& camera, const PassDesc& pass,
              double devicePixelRatio);

private:
  struct Resources;

  RenderDevice& m_device;
  std::unique_ptr<Resources> m_resources;
  std::unique_ptr<TileRendererSettings> m_tileSettings;
  std::unique_ptr<TileRenderer> m_tiles;
  GlobeStyle m_style;
  std::vector<GlobeMarker> m_markers;
  bool m_markersDirty = true;
};

} // namespace ts

#endif // TS_GLOBE_GLOBERENDERER_H
