// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Interfaces of the data sources the tiling engine loads from.
 */
#ifndef TS_PROVIDERS_TILEPROVIDER_H
#define TS_PROVIDERS_TILEPROVIDER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include <QImage>
#include <QString>
#include <QVariantMap>

#include "ts/tiling/tilekey.h"

namespace ts
{

/**
 * How a source is doing, for the user: shown next to its layers.
 *
 * @reentrant
 */
struct ProviderStatus {
  /** The state. */
  enum class State {
    Ok,      ///< Working (or nothing has been asked of it yet).
    Offline, ///< Its server can't be reached; saved tiles are shown.
    Error,   ///< It can't deliver tiles; message() says why.
  };
  /** The state. */
  State state = State::Ok;
  /** What is wrong, for the user (translated); empty when Ok. */
  QString message;

  /** Returns true if both have the same state and message. */
  bool operator==(const ProviderStatus& other) const
  {
    return state == other.state && message == other.message;
  }
  /** Returns true if they differ. */
  bool operator!=(const ProviderStatus& other) const
  {
    return !(*this == other);
  }
};

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

  /**
   * Returns how many loader threads the source wants: 0 for the loader's
   * default. Sources that wait on a network rather than a disk or the CPU
   * ask for more, so that several requests are in flight at once.
   */
  virtual int preferredLoaderThreads() const { return 0; }

  /** Returns how the source is doing (Ok until it says otherwise). */
  ProviderStatus status() const;

  /**
   * Sets the function called when status() changes. It is called on the
   * thread that changed it, often a loader thread, so it must only hand
   * over (see GlobeView's tiles-ready callback).
   */
  void setStatusCallback(std::function<void()> callback);

protected:
  /** For subclasses. */
  TileProvider() = default;

  /** Sets the status, calling the status callback if it changed. */
  void reportStatus(const ProviderStatus& status);

private:
  mutable std::mutex m_statusMutex;
  ProviderStatus m_status;
  std::function<void()> m_statusCallback;
};

/** What became of a request for one tile's image. */
struct ImageLoadResult {
  /** The outcome. */
  enum class Outcome {
    Image,     ///< image holds the tile.
    NoData,    ///< The source has nothing there (for good).
    Failed,    ///< Nothing now; worth asking again later (a network error).
    Cancelled, ///< Given up because it was cancelled.
  };
  /** The outcome. */
  Outcome outcome = Outcome::NoData;
  /** The image (Outcome::Image only). */
  QImage image;
};

/** How a tile's image maps to its latitude/longitude rectangle. */
enum class TileImageProjection {
  /**
   * Equirectangular: rows evenly spaced in latitude, row 0 at the north
   * edge.
   */
  Geographic,
  /**
   * Web Mercator (EPSG:3857): rows evenly spaced in Mercator northing,
   * covering the tile's rectangle clipped to the Mercator limit
   * (±85.051 129°). The renderer reprojects it per pixel.
   */
  WebMercator,
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

  /** Returns how the images map to their tiles (Geographic by default). */
  virtual TileImageProjection imageProjection() const
  {
    return TileImageProjection::Geographic;
  }

  /**
   * Loads the image of @p key, blocking, like loadImage(), but tells a
   * failure that is worth retrying from a tile with no data. The default
   * calls loadImage(): a null image is NoData.
   */
  virtual ImageLoadResult loadTileImage(TileKey key,
                                        const std::atomic<bool>& cancelled);

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
  /**
   * samples² heights above the WGS 84 ellipsoid, metres, row by row; NaN
   * where the source has no data (CompositeElevationProvider fills those
   * before the renderer sees them).
   */
  std::vector<float> heightsM;
  /**
   * samples² heights of sea level (the geoid) above the ellipsoid at the
   * same samples, metres; empty if the source doesn't know them (then
   * sea level is taken as the ellipsoid).
   */
  std::vector<float> seaLevelM;
  /**
   * True if part of it couldn't be had for now (a web source that failed
   * to answer): the samples are what there is, and the renderer asks for
   * the tile again later.
   */
  bool incomplete = false;
};

/**
 * A source of elevation tiles (Phase 7): the base pack's DEM, DEM files,
 * or several of them combined.
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
 * user vector formats such as GeoJSON later).
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
