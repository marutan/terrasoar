// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/network/tilediskcache.h"

#include <utility>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sqlite3.h>

#include "ts/deps/sqliteconnection.h"

namespace ts
{

namespace
{

/** Format of the cache file; a file of another version starts over. */
constexpr int formatVersion = 1;

/** Evict down to this fraction of the limit, so not every store evicts. */
constexpr double evictTarget = 0.9;

const char* const schemaSql =
    "CREATE TABLE IF NOT EXISTS tiles ("
    " tile INTEGER PRIMARY KEY,"
    " data BLOB NOT NULL,"
    " content_type TEXT NOT NULL,"
    " expires INTEGER NOT NULL,"
    " etag TEXT NOT NULL,"
    " last_modified TEXT NOT NULL,"
    " last_used INTEGER NOT NULL,"
    " bytes INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS tiles_last_used ON tiles(last_used);";

/** A prepared statement, finalized when it goes. */
class Statement
{
public:
  Statement(sqlite3* db, const char* sql)
      : m_code(sqlite3_prepare_v2(db, sql, -1, &m_statement, nullptr))
  {
  }
  ~Statement() { sqlite3_finalize(m_statement); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;

  /** SQLITE_OK if prepared, else the error. */
  int code() const { return m_code; }

  void bind(int index, long long value)
  {
    sqlite3_bind_int64(m_statement, index, value);
  }
  void bind(int index, const QByteArray& value)
  {
    sqlite3_bind_blob(m_statement, index, value.constData(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
  }

  /** SQLITE_ROW, SQLITE_DONE or an error. */
  int step() { return sqlite3_step(m_statement); }

  long long integer(int column) const
  {
    return sqlite3_column_int64(m_statement, column);
  }
  QByteArray blob(int column) const
  {
    const void* data = sqlite3_column_blob(m_statement, column);
    const int size = sqlite3_column_bytes(m_statement, column);
    return data == nullptr ? QByteArray()
                           : QByteArray(static_cast<const char*>(data), size);
  }

private:
  sqlite3_stmt* m_statement = nullptr;
  int m_code = SQLITE_OK;
};

/** The key as SQLite's signed 64-bit integer. */
long long rowId(TileKey key)
{
  return static_cast<long long>(key.packed());
}

bool isCorruption(int code)
{
  const int primary = code & 0xff;
  return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

} // namespace

struct TileDiskCache::Database {
  SqliteConnection connection;
};

TileDiskCache::TileDiskCache(QString path, long long maximumBytes)
    : m_path(std::move(path)), m_maximumBytes(maximumBytes)
{
}

TileDiskCache::~TileDiskCache() = default;

bool TileDiskCache::openFresh(bool wipeFirst)
{
  m_db.reset();
  m_bytes = 0;
  m_clock = 0;
  if (wipeFirst) {
    for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
      QFile::remove(m_path + QLatin1String(suffix));
    }
  }
  QDir().mkpath(QFileInfo(m_path).absolutePath());
  std::unique_ptr<Database> db = std::make_unique<Database>();
  db->connection =
      SqliteConnection(m_path, SqliteConnection::OpenMode::ReadWriteCreate);
  if (!db->connection.isOpen()) {
    m_error = db->connection.errorString();
    return false;
  }
  sqlite3_busy_timeout(db->connection.get(), 5000);
  const SetUp result = setUp(db->connection);
  if (result != SetUp::Ready) {
    m_error = db->connection.errorString();
    if (wipeFirst) {
      return false; // even a new file fails: not the file's fault
    }
    db.reset();
    if (result == SetUp::Damaged) {
      ++m_recoveries;
    }
    return openFresh(true);
  }
  m_db = std::move(db);
  m_error.clear();
  return true;
}

TileDiskCache::SetUp TileDiskCache::setUp(SqliteConnection& connection)
{
  // The first statement reads the header: a foreign or damaged file fails
  // here (SQLITE_NOTADB, SQLITE_CORRUPT).
  bool ok = false;
  const long long version =
      connection.queryInteger(QStringLiteral("PRAGMA user_version"), &ok);
  if (!ok) {
    return SetUp::Damaged;
  }
  const long long tables = connection.queryInteger(
      QStringLiteral("SELECT count(*) FROM sqlite_master"), &ok);
  if (!ok) {
    return SetUp::Damaged;
  }
  if (version != formatVersion && tables > 0) {
    return SetUp::OtherVersion;
  }
  const bool ready =
      connection.execute(QStringLiteral("PRAGMA journal_mode=WAL;"
                                        "PRAGMA synchronous=NORMAL;")) &&
      connection.execute(QString::fromLatin1(schemaSql)) &&
      connection.execute(
          QStringLiteral("PRAGMA user_version=%1").arg(formatVersion));
  if (!ready) {
    return SetUp::Damaged;
  }
  m_bytes = connection.queryInteger(
      QStringLiteral("SELECT coalesce(sum(bytes), 0) FROM tiles"), &ok);
  if (!ok) {
    return SetUp::Damaged;
  }
  m_clock = connection.queryInteger(
      QStringLiteral("SELECT coalesce(max(last_used), 0) FROM tiles"), &ok);
  return ok ? SetUp::Ready : SetUp::Damaged;
}

bool TileDiskCache::ensureOpen()
{
  return m_db != nullptr || openFresh(false);
}

bool TileDiskCache::handleFailure(int code)
{
  // A damaged file found in use: start over with an empty one.
  if (isCorruption(code)) {
    m_error = QString::fromUtf8(sqlite3_errstr(code));
    if (openFresh(true)) {
      ++m_recoveries;
      return true;
    }
  }
  if (m_db) {
    m_error = QString::fromUtf8(sqlite3_errmsg(m_db->connection.get()));
  }
  return false;
}

std::optional<CachedTile> TileDiskCache::find(TileKey key)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  for (int attempt = 0; attempt < 2 && ensureOpen(); ++attempt) {
    sqlite3* db = m_db->connection.get();
    int code = SQLITE_OK;
    {
      Statement select(db, "SELECT data, content_type, expires, etag, "
                           "last_modified FROM tiles WHERE tile = ?1");
      code = select.code();
      if (code == SQLITE_OK) {
        select.bind(1, rowId(key));
        code = select.step();
        if (code == SQLITE_DONE) {
          return std::nullopt;
        }
        if (code == SQLITE_ROW) {
          CachedTile tile;
          tile.data = select.blob(0);
          tile.contentType = select.blob(1);
          tile.expiresS = select.integer(2);
          tile.etag = select.blob(3);
          tile.lastModified = select.blob(4);
          Statement touch(db,
                          "UPDATE tiles SET last_used = ?1 WHERE tile = ?2");
          touch.bind(1, ++m_clock);
          touch.bind(2, rowId(key));
          touch.step();
          return tile;
        }
      }
    }
    if (!handleFailure(code)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool TileDiskCache::store(TileKey key, const CachedTile& tile)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  for (int attempt = 0; attempt < 2 && ensureOpen(); ++attempt) {
    sqlite3* db = m_db->connection.get();
    long long previousBytes = 0;
    int code = SQLITE_OK;
    {
      Statement size(db, "SELECT bytes FROM tiles WHERE tile = ?1");
      code = size.code();
      if (code == SQLITE_OK) {
        size.bind(1, rowId(key));
        code = size.step();
        if (code == SQLITE_ROW) {
          previousBytes = size.integer(0);
          code = SQLITE_DONE;
        }
      }
    }
    if (code == SQLITE_DONE) {
      Statement insert(db, "INSERT OR REPLACE INTO tiles (tile, data, "
                           "content_type, expires, etag, last_modified, "
                           "last_used, bytes) VALUES (?1, ?2, ?3, ?4, ?5, "
                           "?6, ?7, ?8)");
      code = insert.code();
      if (code == SQLITE_OK) {
        insert.bind(1, rowId(key));
        insert.bind(2, tile.data);
        insert.bind(3, tile.contentType);
        insert.bind(4, tile.expiresS);
        insert.bind(5, tile.etag);
        insert.bind(6, tile.lastModified);
        insert.bind(7, ++m_clock);
        insert.bind(8, static_cast<long long>(tile.data.size()));
        code = insert.step();
      }
    }
    if (code == SQLITE_DONE) {
      m_bytes += static_cast<long long>(tile.data.size()) - previousBytes;
      if (m_bytes > m_maximumBytes) {
        evict();
      }
      return true;
    }
    if (!handleFailure(code)) {
      return false;
    }
  }
  return false;
}

bool TileDiskCache::refresh(TileKey key, long long expiresS,
                            const QByteArray& etag,
                            const QByteArray& lastModified)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!ensureOpen()) {
    return false;
  }
  sqlite3* db = m_db->connection.get();
  Statement update(db, "UPDATE tiles SET expires = ?1, etag = ?2, "
                       "last_modified = ?3, last_used = ?4 WHERE tile = ?5");
  if (update.code() != SQLITE_OK) {
    handleFailure(update.code());
    return false;
  }
  update.bind(1, expiresS);
  update.bind(2, etag);
  update.bind(3, lastModified);
  update.bind(4, ++m_clock);
  update.bind(5, rowId(key));
  const int code = update.step();
  if (code != SQLITE_DONE) {
    handleFailure(code);
    return false;
  }
  return sqlite3_changes(db) > 0;
}

void TileDiskCache::remove(TileKey key)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!ensureOpen()) {
    return;
  }
  sqlite3* db = m_db->connection.get();
  long long bytes = 0;
  {
    Statement size(db, "SELECT bytes FROM tiles WHERE tile = ?1");
    size.bind(1, rowId(key));
    if (size.step() != SQLITE_ROW) {
      return;
    }
    bytes = size.integer(0);
  }
  Statement erase(db, "DELETE FROM tiles WHERE tile = ?1");
  erase.bind(1, rowId(key));
  if (erase.step() == SQLITE_DONE) {
    m_bytes -= bytes;
  }
}

void TileDiskCache::clear()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (ensureOpen() &&
      m_db->connection.execute(QStringLiteral("DELETE FROM tiles"))) {
    m_bytes = 0;
  }
}

void TileDiskCache::evict()
{
  // Under m_mutex, open. Least recently used first, in batches.
  sqlite3* db = m_db->connection.get();
  const long long target =
      static_cast<long long>(evictTarget * static_cast<double>(m_maximumBytes));
  while (m_bytes > target) {
    std::vector<std::pair<long long, long long>> victims;
    {
      Statement oldest(db, "SELECT tile, bytes FROM tiles ORDER BY last_used "
                           "LIMIT 64");
      while (oldest.step() == SQLITE_ROW) {
        victims.emplace_back(oldest.integer(0), oldest.integer(1));
      }
    }
    if (victims.empty()) {
      m_bytes = 0;
      return;
    }
    m_db->connection.execute(QStringLiteral("BEGIN"));
    for (const std::pair<long long, long long>& victim : victims) {
      if (m_bytes <= target) {
        break;
      }
      Statement erase(db, "DELETE FROM tiles WHERE tile = ?1");
      erase.bind(1, victim.first);
      if (erase.step() == SQLITE_DONE) {
        m_bytes -= victim.second;
      }
    }
    m_db->connection.execute(QStringLiteral("COMMIT"));
  }
}

long long TileDiskCache::sizeBytes()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return ensureOpen() ? m_bytes : 0;
}

int TileDiskCache::count()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!ensureOpen()) {
    return 0;
  }
  return static_cast<int>(m_db->connection.queryInteger(
      QStringLiteral("SELECT count(*) FROM tiles")));
}

int TileDiskCache::recoveries() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_recoveries;
}

QString TileDiskCache::errorString() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_error;
}

} // namespace ts
