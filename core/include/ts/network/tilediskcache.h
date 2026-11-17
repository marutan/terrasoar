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
 * A persistent cache of downloaded tiles, one SQLite file per source.
 */
#ifndef TS_NETWORK_TILEDISKCACHE_H
#define TS_NETWORK_TILEDISKCACHE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

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
  int64_t expiresS = 0;
  /** ETag, for revalidation; may be empty. */
  QByteArray etag;
  /** Last-Modified, for revalidation; may be empty. */
  QByteArray lastModified;
};

/** What an offline region holds in a cache, see TileDiskCache::regionUsage().
 */
struct RegionUsage {
  /** Tiles pinned by the region. */
  int64_t tiles = 0;
  /** Their bytes. */
  int64_t bytes = 0;
};

/**
 * Downloaded tiles on disk, kept between runs and for offline use.
 *
 * One SQLite database per source, holding each tile's bytes as they
 * came from the server with what HTTP said about their freshness. When
 * the tiles not pinned pass the size limit, the least recently used of
 * them go until they are under nine tenths of it. The file is opened on
 * first use (on a loader thread, not the GUI thread).
 *
 * Offline regions *pin* their tiles (identified by a region id, any
 * number but 0): a pinned tile is never evicted and doesn't count
 * against the size limit, until the last region pinning it lets go
 * (unpinRegion()).
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
  TileDiskCache(QString path, int64_t maximumBytes);

  /** Closes the database. */
  ~TileDiskCache();

  TileDiskCache(const TileDiskCache&) = delete;
  TileDiskCache& operator=(const TileDiskCache&) = delete;
  TileDiskCache(TileDiskCache&&) = delete;
  TileDiskCache& operator=(TileDiskCache&&) = delete;

  /** Returns the database's path. */
  const QString& path() const { return m_path; }

  /** Returns the size limit in bytes, of the tiles not pinned. */
  int64_t maximumBytes() const { return m_maximumBytes; }

  /**
   * Returns the tile @p key if it is stored (fresh or not), and counts
   * it as used just now.
   */
  std::optional<CachedTile> find(TileKey key);

  /**
   * Stores @p tile as @p key, replacing what was there, and pins it for
   * the region @p pinRegion unless that is 0. Then evicts down to the
   * limit if it is passed. Returns false if it could not be written.
   */
  bool store(TileKey key, const CachedTile& tile, int64_t pinRegion = 0);

  /**
   * Pins the stored tile @p key for the region @p region (not 0).
   * Returns false if the tile isn't stored.
   */
  bool pin(int64_t region, TileKey key);

  /**
   * Lets go of every tile the region @p region pinned; those no other
   * region pins become ordinary tiles again (and may be evicted at once
   * if the cache is over its limit).
   */
  void unpinRegion(int64_t region);

  /** Returns true if any region pins the tile @p key. */
  bool isPinned(TileKey key);

  /** Returns the tiles the region @p region pins, and their bytes. */
  RegionUsage regionUsage(int64_t region);

  /** Returns the ids of the regions that pin tiles here. */
  std::vector<int64_t> pinnedRegions();

  /**
   * Updates the freshness and validators of the stored tile @p key (after
   * a 304 Not Modified). Returns false if it isn't stored.
   */
  bool refresh(TileKey key, int64_t expiresS, const QByteArray& etag,
               const QByteArray& lastModified);

  /** Removes the tile @p key, if stored, and its pins. */
  void remove(TileKey key);

  /** Removes every tile and every pin. */
  void clear();

  /** Returns the bytes of tile data stored, pinned or not. */
  int64_t sizeBytes();

  /** Returns the bytes of the pinned tiles. */
  int64_t pinnedBytes();

  /** Returns the number of tiles stored. */
  int32_t count();

  /** Returns how often a damaged file was replaced by an empty one. */
  int32_t recoveries() const;

  /** Returns why the last operation failed, or an empty string. */
  QString errorString() const;

private:
  struct Database;
  enum class SetUp { Ready, Damaged, OtherVersion };

  SetUp setUp(SqliteConnection& connection);
  bool ensureOpen();
  bool openFresh(bool wipeFirst);
  bool handleFailure(int32_t code);
  void evict();
  bool isPinnedLocked(int64_t tile);
  void recountPinned();

  const QString m_path;
  const int64_t m_maximumBytes;
  mutable std::mutex m_mutex;
  std::unique_ptr<Database> m_db;
  int64_t m_bytes = 0;
  int64_t m_pinnedBytes = 0;
  int64_t m_clock = 0;
  int32_t m_recoveries = 0;
  QString m_error;
};

} // namespace ts

#endif // TS_NETWORK_TILEDISKCACHE_H
