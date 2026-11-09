// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Interfaces of the data sources the tiling engine loads from.
 */
#ifndef TS_PROVIDERS_TILEPROVIDER_H
#define TS_PROVIDERS_TILEPROVIDER_H

#include <atomic>
#include <optional>
#include <vector>

#include <QImage>
#include <QString>
#include <QVariantMap>

#include "ts/tiling/tilekey.h"

namespace ts
{

/**
 * What every tile source has: an identity, a name and a deepest level.
 *
 * Providers are shared between the render thread (which reads the
 * metadata) and the loader's worker threads (which call the load
 * functions of the subclasses, several at once). Implementations must be
 * thread-safe; the metadata must not change after construction. See
 * docs/design/providers.md.
 *
 * @threadsafe
 */
class TileProvider
{
public:
  /** Destroys the provider. */
  virtual ~TileProvider();

  TileProvider(const TileProvider&) = delete;
  TileProvider& operator=(const TileProvider&) = delete;
  TileProvider(TileProvider&&) = delete;
  TileProvider& operator=(TileProvider&&) = delete;

  /**
   * Returns a string identifying the source (file path, URL and layer,
   * or a test pattern with its parameters). Two providers with the same
   * id deliver the same tiles, so they can share caches.
   */
  virtual QString id() const = 0;

  /** Returns a name for the user (translated where it is fixed text). */
  virtual QString name() const = 0;

  /** Returns the deepest level the source has data for. */
  virtual int maximumLevel() const = 0;

protected:
  /** For subclasses. */
  TileProvider() = default;
};

/**
 * A source of imagery tiles: one square image per tile, row 0 at the
 * north edge, column 0 at the west edge, covering exactly the tile's
 * latitude/longitude rectangle (equirectangular within the tile).
 *
 * @threadsafe
 */
class ImageryProvider : public TileProvider
{
public:
  /** Returns the width and height of the tiles in pixels. */
  virtual int tileSizePx() const = 0;

  /**
   * Loads the image of @p key, blocking; called on worker threads, often
   * several at once. Returns a null image if there is no data there or
   * loading failed. Long loads should check @p cancelled and give up
   * early when it becomes true (the camera has moved on).
   */
  virtual QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) = 0;
};

/**
 * Heights for one tile: a square grid of samples over the tile's
 * rectangle, row 0 at the north edge, edges included (neighbouring tiles
 * share their edge samples).
 *
 * @reentrant
 */
struct HeightTile {
  /** The tile. */
  TileKey key;
  /** Samples along each side (at least 2). */
  int samples = 0;
  /** samples² heights above the WGS 84 ellipsoid, metres, row by row. */
  std::vector<float> heightsM;
};

/**
 * A source of elevation tiles. Used from Phase 6 (terrain); the interface
 * is fixed now so that the loader, cache and renderer are built for it.
 *
 * @threadsafe
 */
class ElevationProvider : public TileProvider
{
public:
  /**
   * Loads the heights of @p key, blocking; on worker threads. Returns
   * nothing where there is no data.
   */
  virtual std::optional<HeightTile>
  loadHeights(TileKey key, const std::atomic<bool>& cancelled) = 0;
};

/** A point as latitude and longitude, degrees. */
struct GeoPoint {
  /** Latitude, degrees. */
  double latDeg = 0.0;
  /** Longitude, degrees. */
  double lonDeg = 0.0;
};

/**
 * One vector feature: a point set, line string or polygon with its
 * attributes.
 *
 * @reentrant
 */
struct Feature {
  /** Kind of geometry. */
  enum class Type {
    /** Each part is one point. */
    Points,
    /** Each part is a line string. */
    LineStrings,
    /** Each part is a ring; the first is the outer ring, the rest holes. */
    Polygon,
  };
  /** Kind of geometry. */
  Type type = Type::Points;
  /** The coordinates, by part. */
  std::vector<std::vector<GeoPoint>> parts;
  /** Attribute values by field name. */
  QVariantMap attributes;
};

/** The features of one tile at one level of detail. */
struct FeatureTile {
  /**
   * The tile the features belong to: the requested key, or an ancestor
   * when the source stores coarser tiles.
   */
  TileKey key;
  /** Features intersecting the tile, clipped or not as the source keeps them.
   */
  std::vector<Feature> features;
};

/**
 * A source of vector features by tile (the base pack's shoreline, Phase 3;
 * vector layers, Phase 4).
 *
 * @threadsafe
 */
class FeatureProvider : public TileProvider
{
public:
  /**
   * Loads the features of @p key, blocking; on worker threads. Returns
   * nothing where there is no data.
   */
  virtual std::optional<FeatureTile>
  loadFeatures(TileKey key, const std::atomic<bool>& cancelled) = 0;

  /**
   * Returns the tile whose features loadFeatures(@p key) returns (its
   * FeatureTile::key): @p key itself, or an ancestor when the source
   * stores coarser tiles. Keys with the same stored tile share one load.
   */
  virtual TileKey storedKey(TileKey key) const { return key; }
};

} // namespace ts

#endif // TS_PROVIDERS_TILEPROVIDER_H
