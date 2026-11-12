// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A persistent cache of downloaded tiles, one SQLite file per source.
 */
#ifndef TS_NETWORK_TILEDISKCACHE_H
#define TS_NETWORK_TILEDISKCACHE_H

#include <memory>
#include <mutex>
#include <optional>

#include <QByteArray>
#include <QString>

#include "ts/tiling/tilekey.h"

namespace ts
{

class SqliteConnection;

/** One stored tile: the bytes as downloaded and their HTTP freshness. */
struct CachedTile {
  /** The response body (an encoded image). */
  QByteArray data;
  /** Its Content-Type. */
  QByteArray contentType;
  /** Fresh until then, seconds since 1970 (see HttpFreshness). */
  long long expiresS = 0;
  /** ETag, for revalidation; may be empty. */
  QByteArray etag;
  /** Last-Modified, for revalidation; may be empty. */
  QByteArray lastModified;
};

/**
 * Downloaded tiles on disk, kept between runs and for offline use.
 *
 * One SQLite database per source, holding each tile's bytes as they
 * came from the server with what HTTP said about their freshness. When
 * the total passes the size limit, the least recently used tiles go
 * until it is under nine tenths of it. The file is opened on first use
 * (on a loader thread, not the GUI thread).
 *
 * A damaged or foreign file is not an error the user sees: when SQLite
 * reports the database corrupt or not a database, the file is deleted
 * and the cache starts empty (recoveries() counts how often). The same
 * happens to a file of another format version.
 *
 * See docs/design/network-and-cache.md.
 *
 * @threadsafe
 */
class TileDiskCache
{
public:
  /**
   * Uses the database at @p path (created with its directory if needed),
   * holding at most @p maximumBytes of tiles.
   */
  TileDiskCache(QString path, long long maximumBytes);

  /** Closes the database. */
  ~TileDiskCache();

  TileDiskCache(const TileDiskCache&) = delete;
  TileDiskCache& operator=(const TileDiskCache&) = delete;
  TileDiskCache(TileDiskCache&&) = delete;
  TileDiskCache& operator=(TileDiskCache&&) = delete;

  /** Returns the database's path. */
  const QString& path() const { return m_path; }

  /** Returns the size limit in bytes. */
  long long maximumBytes() const { return m_maximumBytes; }

  /**
   * Returns the tile @p key if it is stored (fresh or not), and counts
   * it as used just now.
   */
  std::optional<CachedTile> find(TileKey key);

  /**
   * Stores @p tile as @p key, replacing what was there, then evicts down
   * to the limit if it is passed. Returns false if it could not be
   * written.
   */
  bool store(TileKey key, const CachedTile& tile);

  /**
   * Updates the freshness and validators of the stored tile @p key (after
   * a 304 Not Modified). Returns false if it isn't stored.
   */
  bool refresh(TileKey key, long long expiresS, const QByteArray& etag,
               const QByteArray& lastModified);

  /** Removes the tile @p key, if stored. */
  void remove(TileKey key);

  /** Removes every tile. */
  void clear();

  /** Returns the bytes of tile data stored. */
  long long sizeBytes();

  /** Returns the number of tiles stored. */
  int count();

  /** Returns how often a damaged file was replaced by an empty one. */
  int recoveries() const;

  /** Returns why the last operation failed, or an empty string. */
  QString errorString() const;

private:
  struct Database;
  enum class SetUp { Ready, Damaged, OtherVersion };

  SetUp setUp(SqliteConnection& connection);
  bool ensureOpen();
  bool openFresh(bool wipeFirst);
  bool handleFailure(int code);
  void evict();

  const QString m_path;
  const long long m_maximumBytes;
  mutable std::mutex m_mutex;
  std::unique_ptr<Database> m_db;
  long long m_bytes = 0;
  long long m_clock = 0;
  int m_recoveries = 0;
  QString m_error;
};

} // namespace ts

#endif // TS_NETWORK_TILEDISKCACHE_H
