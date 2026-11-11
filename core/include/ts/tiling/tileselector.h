// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Picks the quadtree tiles to draw and to load for a camera.
 */
#ifndef TS_TILING_TILESELECTOR_H
#define TS_TILING_TILESELECTOR_H

#include <functional>
#include <vector>

#include "ts/tiling/tilekey.h"

namespace ts
{

class Camera;

/** Settings of the level-of-detail selection. */
struct TileSelectionSettings {
  /**
   * Largest screen-space error in pixels: a tile is split into its
   * children when one of its texels would cover more than this many
   * pixels. Larger values load fewer, coarser tiles (lower quality tiers).
   */
  double maximumScreenSpaceErrorPx = 1.5;
  /** Texels along a tile's side (the imagery tile size). */
  int tileSizePx = 256;
  /** Deepest level to select. */
  int maximumLevel = 18;
  /**
   * Coarsest level to select while the view is the flat map or changing
   * to it (the camera's morph factor is above 0), whatever maximumLevel
   * says: the geometry of the unrolling globe needs it.
   */
  int minimumMapLevel = 0;
  /**
   * Largest error of a tile's geometry on the flat map, in pixels. A
   * tile's mesh is straight between its grid lines, where the projection
   * curves in latitude (Mercator strongly towards its edges); on the map
   * (the camera's morph factor is 1) a tile is split while the middle of
   * its most poleward grid row is off by more than this, whatever
   * maximumLevel says. See docs/design/morph.md.
   */
  double maximumMapGeometryErrorPx = 0.5;
  /** Grid segments per side of the tile meshes (TileMesh::segments). */
  int meshSegments = 16;
};

/** A tile that should be loaded, with what makes it urgent. */
struct TileLoadRequest {
  /** The tile. */
  TileKey key;
  /** Distance from the eye to the tile's bounding sphere, metres. */
  double distanceM = 0.0;
};

/** The result of TileSelector::select(). */
struct TileSelection {
  /**
   * Tiles to draw: all renderable, together covering every visible part
   * of the globe that has data, without overlapping.
   */
  std::vector<TileKey> render;
  /** Tiles wanted but not renderable yet, coarsest and nearest first. */
  std::vector<TileLoadRequest> load;
  /** Tiles looked at during the traversal (for statistics and tests). */
  int visited = 0;
};

/**
 * Level-of-detail selection on the EPSG:4326 quadtree.
 *
 * Walks down from the two root tiles. A tile is skipped when it is
 * outside the view frustum or entirely behind the horizon. A visible tile
 * is split while its screen-space error (the size of one of its texels on
 * screen) exceeds the limit. A tile is only replaced by its children once
 * all its visible children are renderable; until then it is drawn itself
 * and the missing children are requested, so the view never has holes and
 * detail arrives coarse to fine. See docs/design/tiling.md.
 *
 * On the flat map (the camera's morph factor is 1) the same walk uses the
 * map instead: a tile is skipped when its rectangle on the map, in every
 * copy of the world, is outside the view or it lies beyond the
 * projection's latitude limits, and its screen-space error is the size of
 * a texel on the map, which grows with the projection's stretching. While
 * the view changes between the two, a tile is kept if either view would
 * show it (ignoring the horizon, since the far side unrolls into view),
 * with the larger of the two errors. See docs/design/morph.md.
 *
 * @reentrant
 */
class TileSelector
{
public:
  /** Creates a selector with @p settings. */
  explicit TileSelector(TileSelectionSettings settings = {});

  /** Returns the settings. */
  TileSelectionSettings settings() const { return m_settings; }

  /** Sets the settings. */
  void setSettings(TileSelectionSettings settings);

  /**
   * Selects the tiles for @p camera. @p isRenderable tells whether a tile
   * can be drawn now (its data is on the GPU).
   */
  TileSelection
  select(const Camera& camera,
         const std::function<bool(const TileKey&)>& isRenderable) const;

  /** Returns true if any part of @p key may be seen from @p camera. */
  static bool isVisible(const Camera& camera, TileKey key);

  /**
   * Returns true if any part of @p key is in the view of the flat map of
   * @p camera (as if its morph factor were 1), in any copy of the world.
   */
  static bool isVisibleOnMap(const Camera& camera, TileKey key);

  /** Returns true if @p key is entirely inside the view frustum's sides. */
  static bool isInFrustum(const Camera& camera, TileKey key);

  /** Returns true if @p key is entirely hidden behind the horizon. */
  static bool isBehindHorizon(const Camera& camera, TileKey key);

  /** Returns the screen-space error of @p key seen from @p camera, pixels. */
  double screenSpaceErrorPx(const Camera& camera, TileKey key) const;

  /**
   * Returns how far the mesh of @p key is from the projection's true
   * shape on the flat map of @p camera, in pixels (see
   * TileSelectionSettings::maximumMapGeometryErrorPx).
   */
  double mapGeometryErrorPx(const Camera& camera, TileKey key) const;

private:
  struct View;
  bool wantsRefinement(const View& view, TileKey key) const;
  bool refine(const View& view, TileKey key, bool renderable,
              const std::function<bool(const TileKey&)>& isRenderable,
              TileSelection& selection) const;
  void visit(const View& view, TileKey key,
             const std::function<bool(const TileKey&)>& isRenderable,
             TileSelection& selection) const;

  TileSelectionSettings m_settings;
};

/**
 * A bounding sphere of a tile (its surface and skirt), ECEF metres.
 *
 * @reentrant
 */
struct TileBounds {
  /** Centre. */
  Vec3 centreEcef{};
  /** Radius. */
  double radiusM = 0.0;
};

/**
 * Returns a bounding sphere of @p key.
 *
 * @threadsafe
 */
TileBounds tileBounds(TileKey key);

} // namespace ts

#endif // TS_TILING_TILESELECTOR_H
