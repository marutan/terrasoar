// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Elevation from a tile store of height grids (the base pack's DEM).
 */
#ifndef TS_ELEVATION_SQLITEELEVATIONPROVIDER_H
#define TS_ELEVATION_SQLITEELEVATIONPROVIDER_H

#include <memory>
#include <optional>

#include <QString>

#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/providers/tileprovider.h"

namespace ts
{

class GeoidModel;

/**
 * Heights from a SQLite tile store of format "heights" (see
 * SqliteTileStoreInfo and encodeHeightGrid()). Heights above a geoid
 * (`vertical_datum`) are turned into heights above the ellipsoid with
 * @p geoid, as HeightTile requires. Keys deeper than the store's deepest
 * level are interpolated from their ancestor there, so every level has
 * heights; keys shallower than its shallowest have none.
 *
 * @threadsafe
 */
class SqliteElevationProvider : public ElevationProvider
{
public:
  /**
   * Opens the store at @p path. @p geoid must be the model of the store's
   * vertical datum; it may be null only for heights above the ellipsoid.
   * Check isValid().
   */
  SqliteElevationProvider(const QString& path,
                          std::shared_ptr<const GeoidModel> geoid);
  ~SqliteElevationProvider() override;

  SqliteElevationProvider(const SqliteElevationProvider&) = delete;
  SqliteElevationProvider& operator=(const SqliteElevationProvider&) = delete;
  SqliteElevationProvider(SqliteElevationProvider&&) = delete;
  SqliteElevationProvider& operator=(SqliteElevationProvider&&) = delete;

  /** Returns true if the store and the geoid can be used. */
  bool isValid() const { return m_error.isEmpty(); }

  /** Returns why the provider isn't valid (English), or an empty string. */
  const QString& errorString() const { return m_error; }

  /** Returns the store's metadata. */
  const SqliteTileStoreInfo& info() const { return m_store.info(); }

  QString id() const override;
  QString name() const override { return m_store.name(); }
  /** The deepest level stored (deeper keys are interpolated). */
  int maximumLevel() const override { return m_store.maximumLevel(); }
  /** Samples along a tile's side. */
  int samples() const { return m_store.tileSizePx(); }

  /** Heights above the WGS 84 ellipsoid. */
  std::optional<HeightTile>
  loadHeights(TileKey key, const std::atomic<bool>& cancelled) override;

  /**
   * Returns the heights of @p key as stored, above the store's vertical
   * datum, for a key the store has; nothing otherwise.
   */
  std::optional<HeightTile> loadStoredHeights(TileKey key);

private:
  SqliteTileImageryProvider m_store;
  std::shared_ptr<const GeoidModel> m_geoid;
  QString m_error;
};

} // namespace ts

#endif // TS_ELEVATION_SQLITEELEVATIONPROVIDER_H
