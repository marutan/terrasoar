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
 * Draws the WGS 84 globe and point markers through a RenderDevice.
 */
#ifndef TS_GLOBE_GLOBERENDERER_H
#define TS_GLOBE_GLOBERENDERER_H

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <QString>

#include "ts/globe/tilerenderer.h"
#include "ts/globe/vectorrenderer.h"
#include "ts/maths/vec3.h"
#include "ts/render/renderdevice.h"

namespace ts
{

class Camera;
class ElevationProvider;
class ImageryProvider;
class TileRenderer;
struct ImageryLayerDraw;
struct TileRendererSettings;
struct TileStatistics;

/** The shape of a GlobeMarker. */
enum class MarkerShape : std::uint8_t {
  Round,    ///< A disc.
  Aircraft, ///< An aircraft seen from above, nose along its heading.
};

/**
 * A marker drawn at a fixed point, with a size in pixels: a disc, or an
 * aircraft turned to its heading.
 *
 * @reentrant
 */
struct GlobeMarker {
  /** Where the marker is, ECEF metres. */
  Vec3 positionEcef{};
  /** Colour, 8-bit RGBA, not premultiplied. */
  std::array<std::uint8_t, 4> rgba = {255, 255, 255, 255};
  /** Radius in logical pixels (half the aircraft's length or span). */
  double radiusPx = 4.0;
  /** The shape. */
  MarkerShape shape = MarkerShape::Round;
  /**
   * Aircraft: the heading the nose points along, degrees from true north
   * (on the ground below it); NaN draws it pointing up the screen.
   */
  double headingDeg = std::numeric_limits<double>::quiet_NaN();
};

/**
 * One point of a SpaceLine: where it is, the point below it (for the map),
 * and how it is drawn there.
 *
 * @reentrant
 */
struct SpaceLinePoint {
  /** Where the point is, ECEF metres. */
  Vec3 positionEcef{};
  /** Latitude of the point below it, degrees. */
  double latDeg = 0.0;
  /** Longitude of the point below it, degrees. */
  double lonDeg = 0.0;
  /** Opacity here, multiplying the line's (fading a track with age). */
  float alpha = 1.0F;
  /** Distance along the line for its dashes, in dash lengths. */
  float along = 0.0F;
  /** Its time (SpaceLine::timed), seconds on the clock's scale. */
  double timeS = 0.0;
  /** Its colour (SpaceLine::pointColours), RGBA, not premultiplied. */
  std::array<std::uint8_t, 4> rgba = {255, 255, 255, 255};
};

/**
 * A line through space or over the ground (a satellite's track, its
 * ground track, a footprint): straight between its points, a fixed width
 * in pixels, tested against the globe's depth so that the Earth hides
 * what is behind it.
 *
 * On the flat map (and during the change to it) each point moves to the
 * point below it, as markers do, and the line is broken where the map
 * wraps around or ends, so a track becomes its ground track.
 *
 * @reentrant
 */
struct SpaceLine {
  /** The points, in order. */
  std::vector<SpaceLinePoint> points;
  /** Colour, 8-bit RGBA, not premultiplied. */
  std::array<std::uint8_t, 4> rgba = {255, 255, 255, 255};
  /** Width, logical pixels. */
  float widthPx = 1.5F;
  /** Dashed: drawn for the first half of each unit of along. */
  bool dashed = false;
  /** Closed: the last point joins the first (a footprint). */
  bool closed = false;
  /** Drawn on the globe only (a ground track the map draws anyway). */
  bool globeOnly = false;
  /** Each point has its own colour (SpaceLinePoint::rgba), else rgba. */
  bool pointColours = false;
  /**
   * Timed (a trail): only the part whose points' times lie within
   * shownS before the renderer's line time (setSpaceLineTime()) is drawn,
   * cut where those times fall between points. The cut is made in the
   * shader, so moving the time rebuilds nothing.
   */
  bool timed = false;
  /** How far back a timed line is shown, seconds (infinity: all of it). */
  double shownS = std::numeric_limits<double>::infinity();
};

/** Colours and options for the globe. */
struct GlobeStyle {
  /** Clear colour behind the globe, linear RGB in [0, 1]. */
  std::array<float, 3> backgroundRgb = {0.02F, 0.03F, 0.06F};
  /** Surface colour, RGB in [0, 1]. */
  std::array<float, 3> surfaceRgb = {0.11F, 0.29F, 0.52F};
  /** Graticule colour, RGB in [0, 1]. */
  std::array<float, 3> graticuleRgb = {0.80F, 0.88F, 1.00F};
  /**
   * True to draw latitude and longitude lines on the bare globe, under any
   * imagery. The graticule layer (setVectorLayers()) draws them in the
   * layer order instead.
   */
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
 * Space lines (satellite tracks) are instanced quads per segment, placed
 * the same way and depth tested against the globe, see SpaceLine.
 *
 * Markers are instanced quads positioned relative to the eye from
 * high/low float pairs (see SplitFloat), so they stay still to well under
 * a pixel when the eye is a metre above the ground.
 *
 * While the camera's morph factor is above 0 (the flat map, or the change
 * to it) nothing is ray-cast: the surface colour and the graticule are
 * drawn on the tile meshes, whose vertices move between the globe and
 * the map, and markers are placed the same way. See docs/design/morph.md.
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

  /** Groups of space lines, uploaded separately (setSpaceLines()). */
  static constexpr int32_t spaceLineGroups = 4;

  /**
   * Replaces the lines through space (see SpaceLine) of @p group (0 to
   * spaceLineGroups − 1), drawn under markers. Each group is uploaded on
   * its own: changing one (satellites' tracks every tick) leaves the
   * others (aircraft trails) on the GPU as they are.
   */
  void setSpaceLines(std::vector<SpaceLine> lines, int32_t group = 0);

  /**
   * Sets the time timed space lines are drawn up to (seconds on their
   * points' scale, see SpaceLine::timed). Cheap: nothing is uploaded.
   */
  void setSpaceLineTime(double timeS);

  /** Returns how many line segments the last frame drew. */
  int32_t spaceSegmentsDrawn() const;

  /**
   * Returns how many times the space lines have been placed and uploaded
   * (for tests: moving the line time doesn't, nor a still view).
   */
  int64_t spaceLineUploads() const { return m_spaceLineUploads; }

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
   * Sets the vector layers, bottom to top, drawn over all imagery: feature
   * layers through the VectorRenderer, graticule layers as the graticule
   * with the layer's outline colour and width.
   */
  void setVectorLayers(const std::vector<VectorLayerDraw>& layers);

  /** Returns what the vector renderer did in the last frame. */
  VectorStatistics vectorStatistics() const;

  /**
   * Sets the terrain's source (null for none); see
   * TileRenderer::setElevationProvider(). Only after initialise().
   */
  void setElevationProvider(std::shared_ptr<ElevationProvider> provider);

  /** Shows the terrain with @p exaggeration, or the plain ellipsoid. */
  void setTerrain(bool visible, double exaggeration);

  /**
   * Sets how strongly the terrain is hillshaded on the flat map (0 for
   * not at all, 1 fully; default 1). It fades in with the morph, and only
   * while the terrain is shown.
   */
  void setHillshade(double strength);

  /** Returns the hillshade's strength. */
  double hillshade() const { return m_hillshade; }

  /**
   * How much steeper the hillshade takes the slopes than the meshes give
   * them: meshes smooth the terrain to their vertices, and on a map seen
   * from above relief needs the help.
   */
  static constexpr double hillshadeSlopeFactor = 4.0;

  /** Returns true if terrain is shown. */
  bool terrainShown() const;

  /** See TileRenderer::terrainHeightM(). */
  std::optional<double> terrainHeightM(double latDeg, double lonDeg);

  /** See TileRenderer::groundHeight(). */
  std::optional<GroundHeight> groundHeight(double latDeg, double lonDeg);

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

  void uploadMarkers(const Camera& camera);
  void uploadSpaceLines(const Camera& camera);
  void drawSpaceLines();
  std::vector<std::size_t> writeGraticuleUniforms(double fade,
                                                  double devicePixelRatio);
  void drawGraticule(std::size_t index);

  RenderDevice& m_device;
  std::unique_ptr<Resources> m_resources;
  std::unique_ptr<TileRendererSettings> m_tileSettings;
  std::unique_ptr<TileRenderer> m_tiles;
  std::unique_ptr<VectorRenderer> m_vectors;
  std::vector<VectorLayerDraw> m_vectorLayers;
  GlobeStyle m_style;
  std::vector<GlobeMarker> m_markers;
  /** One group of space lines (setSpaceLines()). */
  struct SpaceLineGroup {
    std::vector<SpaceLine> lines;
    bool dirty = true;
  };
  std::array<SpaceLineGroup, spaceLineGroups> m_spaceLineGroups;
  bool m_spaceLinesMorphed = false;
  /** The view the morphed lines were last placed for. */
  std::array<double, 6> m_spaceLinesView{};
  const void* m_spaceLinesProjection = nullptr;
  int64_t m_spaceLineUploads = 0;
  /** The time timed lines are drawn up to (setSpaceLineTime()). */
  double m_spaceLineTimeS = 0.0;
  /** The time their float times count from (chosen at upload). */
  double m_spaceLineOriginS = 0.0;
  bool m_markersDirty = true;
  bool m_markersMorphed = false;
  double m_morph = 0.0;
  /** The flat map's hillshade strength (setHillshade()). */
  double m_hillshade = 1.0;
};

} // namespace ts

#endif // TS_GLOBE_GLOBERENDERER_H
