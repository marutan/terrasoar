// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Imagery tiles stored in a SQLite file: the provider that reads them and
 * the writer the packager and tests use.
 */
#ifndef TS_PROVIDERS_SQLITETILEIMAGERYPROVIDER_H
#define TS_PROVIDERS_SQLITETILEIMAGERYPROVIDER_H

#include <memory>

#include <QByteArray>
#include <QString>

#include "ts/deps/sqliteconnection.h"
#include "ts/providers/tileprovider.h"

namespace ts
{

class SqliteStatementPool;

/**
 * What a SQLite tile store holds (its `metadata` table).
 *
 * A tile store has two tables:
 *
 * - `metadata(name TEXT PRIMARY KEY, value TEXT)` with the rows
 *   `scheme` (= "ts-epsg4326"), `format` ("jpeg" or "png"; "heights" for
 *   height grids, see ts::encodeHeightGrid(), with `tile_size` the samples
 *   per side), `tile_size`, `min_level`, `max_level`, `name` and, for
 *   heights, optionally `vertical_datum`;
 * - `tiles(level, x, y, data)` keyed by ts::TileKey (rows from the north),
 *   `data` the encoded image or height grid.
 *
 * See docs/data/base-pack.md.
 *
 * @reentrant
 */
struct SqliteTileStoreInfo {
  /** Image format of the tiles: "jpeg" or "png". */
  QString format = QStringLiteral("jpeg");
  /** Width and height of every tile, pixels. */
  int tileSizePx = 256;
  /** Shallowest level stored (complete from here down to maxLevel). */
  int minLevel = 0;
  /** Deepest level stored. */
  int maxLevel = 0;
  /** Name of the imagery (English). */
  QString name;
  /**
   * For height grids (format "heights"): the vertical datum of the
   * heights, the id of a geoid dataset of the same pack (e.g. "egm2008"),
   * or empty for heights above the ellipsoid.
   */
  QString verticalDatum;
};

/**
 * Writes a SQLite tile store, inside one transaction committed by
 * finish().
 *
 * @reentrant
 */
class SqliteTileStoreWriter
{
public:
  /**
   * Creates the store at @p path (which must not exist) for @p info; check
   * errorString() afterwards.
   */
  SqliteTileStoreWriter(const QString& path, const SqliteTileStoreInfo& info);

  /** Rolls back unless finish() succeeded. */
  ~SqliteTileStoreWriter();

  SqliteTileStoreWriter(const SqliteTileStoreWriter&) = delete;
  SqliteTileStoreWriter& operator=(const SqliteTileStoreWriter&) = delete;
  SqliteTileStoreWriter(SqliteTileStoreWriter&&) = delete;
  SqliteTileStoreWriter& operator=(SqliteTileStoreWriter&&) = delete;

  /** Stores the encoded image @p data for @p key. False on failure. */
  bool putTile(TileKey key, const QByteArray& data);

  /** Commits and compacts the file. False on failure. */
  bool finish();

  /** Returns the last error, empty if none. */
  QString errorString() const { return m_error; }

private:
  SqliteConnection m_connection;
  struct Statement;
  std::unique_ptr<Statement> m_insert;
  QString m_error;
  bool m_finished = false;
};

/**
 * Imagery from a SQLite tile store (see SqliteTileStoreInfo), for example
 * the base pack's Blue Marble.
 *
 * Missing tiles and tiles that do not decode are reported as no data (a
 * null image), never as a crash; deeper levels than the store's are not
 * requested (maximumLevel()).
 *
 * @threadsafe
 */
class SqliteTileImageryProvider : public ImageryProvider
{
public:
  /** Opens the store at @p path and reads its metadata; check isValid(). */
  explicit SqliteTileImageryProvider(const QString& path);

  ~SqliteTileImageryProvider() override;

  SqliteTileImageryProvider(const SqliteTileImageryProvider&) = delete;
  SqliteTileImageryProvider&
  operator=(const SqliteTileImageryProvider&) = delete;
  SqliteTileImageryProvider(SqliteTileImageryProvider&&) = delete;
  SqliteTileImageryProvider& operator=(SqliteTileImageryProvider&&) = delete;

  /** Returns true if the store opened and its metadata is usable. */
  bool isValid() const { return m_valid; }

  /** Returns why the store is not valid (English), or an empty string. */
  QString errorString() const { return m_error; }

  /** Returns the store's metadata. */
  const SqliteTileStoreInfo& info() const { return m_info; }

  QString id() const override;

  /** Returns the name stored in the metadata. */
  QString name() const override { return m_info.name; }
  int maximumLevel() const override { return m_info.maxLevel; }
  int tileSizePx() const override { return m_info.tileSizePx; }
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

  /**
   * Returns the encoded bytes stored for @p key, empty if there are none
   * (for checks and tests).
   */
  QByteArray loadEncoded(TileKey key);

private:
  QString m_path;
  SqliteTileStoreInfo m_info;
  QString m_error;
  bool m_valid = false;
  std::unique_ptr<SqliteStatementPool> m_pool;
};

} // namespace ts

#endif // TS_PROVIDERS_SQLITETILEIMAGERYPROVIDER_H
