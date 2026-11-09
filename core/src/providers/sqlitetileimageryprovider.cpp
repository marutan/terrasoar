// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/providers/sqlitetileimageryprovider.h"

#include <QFileInfo>
#include <QImage>
#include <QLocale>

#include <sqlite3.h>

#include "sqlitestatementpool.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

namespace
{

QString schemeName()
{
  return QStringLiteral("ts-epsg4326"); // ts-notr: identifier
}

/** Reads the metadata table into @p info; false and @p error if unusable. */
bool readInfo(const SqliteConnection& connection, SqliteTileStoreInfo* info,
              QString* error)
{
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(connection.get(), "SELECT name, value FROM metadata",
                         -1, &statement, nullptr) != SQLITE_OK) {
    *error = QStringLiteral("No metadata table: %1")
                 .arg(QString::fromUtf8(sqlite3_errmsg(connection.get())));
    sqlite3_finalize(statement);
    return false;
  }
  QString scheme;
  int found = 0;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const QString name = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    const QString value = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
    bool ok = true;
    if (name == QLatin1String("scheme")) {
      scheme = value;
    } else if (name == QLatin1String("format")) {
      info->format = value;
    } else if (name == QLatin1String("tile_size")) {
      info->tileSizePx = QLocale::c().toInt(value, &ok);
      found += ok ? 1 : 0;
    } else if (name == QLatin1String("min_level")) {
      info->minLevel = QLocale::c().toInt(value, &ok);
      found += ok ? 1 : 0;
    } else if (name == QLatin1String("max_level")) {
      info->maxLevel = QLocale::c().toInt(value, &ok);
      found += ok ? 1 : 0;
    } else if (name == QLatin1String("name")) {
      info->name = value;
    }
  }
  sqlite3_finalize(statement);
  if (scheme != schemeName() || found != 3 || info->tileSizePx < 1 ||
      info->tileSizePx > 4096 || info->minLevel < 0 ||
      info->maxLevel < info->minLevel ||
      info->maxLevel > TileKey::maximumLevel) {
    *error = QStringLiteral("Unsupported or incomplete tile store metadata");
    return false;
  }
  return true;
}

} // namespace

// --- Writer -----------------------------------------------------------------

/** The prepared insert statement. */
struct SqliteTileStoreWriter::Statement {
  sqlite3_stmt* handle = nullptr;
  Statement() = default;
  ~Statement() { sqlite3_finalize(handle); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;
};

SqliteTileStoreWriter::SqliteTileStoreWriter(const QString& path,
                                             const SqliteTileStoreInfo& info)
    : m_insert(std::make_unique<Statement>())
{
  if (QFileInfo::exists(path)) {
    m_error = QStringLiteral("%1 already exists").arg(path);
    return;
  }
  m_connection =
      SqliteConnection(path, SqliteConnection::OpenMode::ReadWriteCreate);
  const QLocale c = QLocale::c();
  // The values are numbers and fixed identifiers, apart from the name,
  // which is quoted by doubling its single quotes.
  QString name = info.name;
  name.replace(QLatin1Char('\''), QStringLiteral("''"));
  const QString schema =
      QStringLiteral(
          "PRAGMA page_size = 8192;"
          "BEGIN;"
          "CREATE TABLE metadata (name TEXT PRIMARY KEY, value TEXT);"
          "CREATE TABLE tiles (level INTEGER NOT NULL, x INTEGER NOT NULL, "
          "y INTEGER NOT NULL, data BLOB NOT NULL, "
          "PRIMARY KEY (level, x, y));"
          "INSERT INTO metadata VALUES ('scheme', '%1'), ('format', '%2'), "
          "('tile_size', '%3'), ('min_level', '%4'), ('max_level', '%5'), "
          "('name', '%6');")
          .arg(schemeName(), info.format, c.toString(info.tileSizePx),
               c.toString(info.minLevel), c.toString(info.maxLevel), name);
  if (!m_connection.isOpen() || !m_connection.execute(schema)) {
    m_error = m_connection.errorString();
    return;
  }
  if (sqlite3_prepare_v2(m_connection.get(),
                         "INSERT INTO tiles VALUES (?, ?, ?, ?)", -1,
                         &m_insert->handle, nullptr) != SQLITE_OK) {
    m_error = QString::fromUtf8(sqlite3_errmsg(m_connection.get()));
  }
}

SqliteTileStoreWriter::~SqliteTileStoreWriter()
{
  m_insert.reset();
  if (m_connection.isOpen() && !m_finished) {
    m_connection.execute(QStringLiteral("ROLLBACK;"));
  }
}

bool SqliteTileStoreWriter::putTile(TileKey key, const QByteArray& data)
{
  if (!m_error.isEmpty() || m_finished) {
    return false;
  }
  sqlite3_stmt* statement = m_insert->handle;
  sqlite3_reset(statement);
  sqlite3_bind_int(statement, 1, key.level);
  sqlite3_bind_int(statement, 2, key.x);
  sqlite3_bind_int(statement, 3, key.y);
  sqlite3_bind_blob(statement, 4, data.constData(),
                    static_cast<int>(data.size()), SQLITE_TRANSIENT);
  if (sqlite3_step(statement) != SQLITE_DONE) {
    m_error = QString::fromUtf8(sqlite3_errmsg(m_connection.get()));
    return false;
  }
  return true;
}

bool SqliteTileStoreWriter::finish()
{
  if (!m_error.isEmpty() || m_finished) {
    return false;
  }
  m_insert.reset();
  if (!m_connection.execute(QStringLiteral("COMMIT; VACUUM;"))) {
    m_error = m_connection.errorString();
    return false;
  }
  m_finished = true;
  return true;
}

// --- Provider ---------------------------------------------------------------

SqliteTileImageryProvider::SqliteTileImageryProvider(const QString& path)
    : m_path(QFileInfo(path).absoluteFilePath())
{
  if (!QFileInfo(m_path).isFile()) {
    m_error = QStringLiteral("%1 does not exist").arg(m_path);
    return;
  }
  const SqliteConnection connection(m_path,
                                    SqliteConnection::OpenMode::ReadOnly);
  if (!connection.isOpen()) {
    m_error = connection.errorString();
    return;
  }
  if (!readInfo(connection, &m_info, &m_error)) {
    return;
  }
  m_pool = std::make_unique<SqliteStatementPool>(
      m_path,
      QByteArrayLiteral(
          "SELECT data FROM tiles WHERE level = ? AND x = ? AND y = ?"));
  m_valid = true;
}

SqliteTileImageryProvider::~SqliteTileImageryProvider() = default;

QString SqliteTileImageryProvider::id() const
{
  return QStringLiteral("sqlitetiles:") + m_path; // ts-notr: identifier
}

QByteArray SqliteTileImageryProvider::loadEncoded(TileKey key)
{
  if (!m_valid || !key.isValid() || key.level < m_info.minLevel ||
      key.level > m_info.maxLevel) {
    return QByteArray();
  }
  const SqliteStatementPool::Lease lease = m_pool->acquire();
  sqlite3_stmt* statement = lease.statement();
  if (statement == nullptr) {
    return QByteArray();
  }
  sqlite3_bind_int(statement, 1, key.level);
  sqlite3_bind_int(statement, 2, key.x);
  sqlite3_bind_int(statement, 3, key.y);
  QByteArray data;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const int bytes = sqlite3_column_bytes(statement, 0);
    const void* blob = sqlite3_column_blob(statement, 0);
    if (blob != nullptr && bytes > 0) {
      data = QByteArray(static_cast<const char*>(blob), bytes);
    }
  }
  return data;
}

QImage SqliteTileImageryProvider::loadImage(TileKey key,
                                            const std::atomic<bool>& cancelled)
{
  const QByteArray data = loadEncoded(key);
  if (data.isEmpty() || cancelled.load()) {
    return QImage();
  }
  QImage image = QImage::fromData(data);
  if (image.isNull()) {
    return QImage();
  }
  if (image.width() != m_info.tileSizePx ||
      image.height() != m_info.tileSizePx) {
    image = image.scaled(m_info.tileSizePx, m_info.tileSizePx,
                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  return image;
}

} // namespace ts
