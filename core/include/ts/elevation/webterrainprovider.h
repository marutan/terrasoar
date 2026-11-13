// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Elevation from web terrain tiles: XYZ Web Mercator PNGs with heights
 * encoded in their colours (Terrarium, Terrain-RGB).
 */
#ifndef TS_ELEVATION_WEBTERRAINPROVIDER_H
#define TS_ELEVATION_WEBTERRAINPROVIDER_H

#include <memory>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QRgb>
#include <QString>
#include <QUrl>

#include "ts/network/cachedtilefetcher.h"
#include "ts/providers/tileprovider.h"
#include "ts/tiling/lrucache.h"

namespace ts
{

class GeoidModel;
class NetworkClient;

/** How a web terrain tile's colours hold heights. */
enum class TerrainEncoding {
  /** Mapzen Terrarium: R·256 + G + B/256 − 32768 metres. */
  Terrarium,
  /** Mapbox Terrain-RGB: −10000 + (R·65536 + G·256 + B)·0.1 metres. */
  TerrainRgb,
};

/**
 * Returns the height in metres a pixel of @p encoding holds, or NaN for a
 * transparent pixel (no data).
 */
double decodeTerrainPixel(QRgb pixel, TerrainEncoding encoding);

/** A web terrain tile set: what a layer's `{"kind": "web", …}` holds. */
struct WebTerrainSource {
  /**
   * Tile URL with `{z}`, `{x}` and `{y}` (XYZ: row 0 at the north), http
   * or https.
   */
  QString urlTemplate;
  /** How heights are encoded. */
  TerrainEncoding encoding = TerrainEncoding::Terrarium;
  /** Deepest zoom level the server has. */
  int maximumZoom = 15;
  /** Pixels along a tile's side (256 or 512). */
  int tileSizePx = 256;
  /** What the heights are measured from: "egm2008", "egm96", "ellipsoid". */
  QString verticalDatum = QStringLiteral("egm96");
  /** Name for the user; empty for the server's host. */
  QString name;
  /** Attribution the provider requires, shown while it is drawn. */
  QString attribution;

  /**
   * Reads a layer source (`"kind": "web"`, `"url"`, `"encoding"`
   * (`"terrarium"` or `"terrain-rgb"`), `"maximumZoom"`, `"tileSize"`,
   * `"verticalDatum"`, `"name"`, `"attribution"`), or returns nothing
   * and @p errorString.
   */
  static std::optional<WebTerrainSource> fromJson(const QJsonObject& json,
                                                  QString* errorString);
  /** Returns the layer source form. */
  QJsonObject toJson() const;
  /** Identifies the tile set (URL, encoding, size and datum). */
  QString id() const;
  /** Returns the URL of tile (@p zoom, @p x, @p y). */
  QUrl tileUrl(int zoom, int x, int y) const;
};

/**
 * Heights from a web terrain tile set, through a CachedTileFetcher (disk
 * cache, revalidation, offline back-off, status). A quadtree tile of
 * level L is sampled from zoom L − 1 (L − 2 for 512-pixel tiles, at most
 * maximumZoom), where the web tiles' pixels are about as far apart as the
 * samples at the equator (closer towards the poles). Each sample is
 * bilinear between the centres of the four pixels around it on the
 * zoom's global pixel grid, so neighbouring tiles share their edge values.
 * Beyond ±85.05° (the Mercator limit), where a tile has no data or is
 * transparent, samples are NaN; where a web tile failed to load they are
 * NaN too and the tile is incomplete (HeightTile::incomplete), so it is
 * asked for again later. Decoded web tiles are kept in a small cache, as
 * neighbouring height tiles share them.
 *
 * @threadsafe
 */
class WebTerrainProvider : public ElevationProvider
{
public:
  /**
   * Fetches @p source's tiles through @p network. @p datumGeoid is the
   * geoid of the source's datum (null for the ellipsoid); @p seaLevelGeoid,
   * the global one, gives sea level where it is null or has no value (see
   * applyVerticalDatum()).
   */
  WebTerrainProvider(WebTerrainSource source,
                     std::shared_ptr<NetworkClient> network,
                     WebTileSettings settings,
                     std::shared_ptr<const GeoidModel> datumGeoid,
                     std::shared_ptr<const GeoidModel> seaLevelGeoid);
  ~WebTerrainProvider() override;

  WebTerrainProvider(const WebTerrainProvider&) = delete;
  WebTerrainProvider& operator=(const WebTerrainProvider&) = delete;
  WebTerrainProvider(WebTerrainProvider&&) = delete;
  WebTerrainProvider& operator=(WebTerrainProvider&&) = delete;

  /** Samples along a tile's side. */
  static constexpr int samples = 65;

  /** Returns the source. */
  const WebTerrainSource& source() const { return m_source; }

  /** Returns the web zoom level quadtree tiles of @p level are read from. */
  int zoomForLevel(int level) const;

  QString id() const override;
  QString name() const override;
  /** The level whose web zoom is the source's deepest. */
  int maximumLevel() const override;
  int preferredLoaderThreads() const override;

  std::optional<HeightTile>
  loadHeights(TileKey key, const std::atomic<bool>& cancelled) override;

  /** Returns what the fetcher has done (tests). */
  WebTileStatistics statistics() const { return m_fetcher->statistics(); }

private:
  /** A decoded web tile: tileSize² heights, NaN for no data. */
  using Heights = std::vector<float>;
  /** A web tile as fetched: its heights, or why there are none. */
  struct Fetched {
    FetchOutcome outcome = FetchOutcome::Failed;
    std::shared_ptr<const Heights> heights;
  };
  Fetched webTile(int zoom, int x, int y, const std::atomic<bool>& cancelled);

  const WebTerrainSource m_source;
  const std::shared_ptr<const GeoidModel> m_datumGeoid;
  const std::shared_ptr<const GeoidModel> m_seaLevelGeoid;
  std::unique_ptr<CachedTileFetcher> m_fetcher;
  /** Decoded web tiles by (zoom, x, y) as a TileKey. */
  LruCache<TileKey, std::shared_ptr<const Heights>, TileKeyHash> m_tiles;
};

} // namespace ts

#endif // TS_ELEVATION_WEBTERRAINPROVIDER_H
