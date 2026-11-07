// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Owning handle for a SQLite connection.
 */
#ifndef TS_DEPS_SQLITECONNECTION_H
#define TS_DEPS_SQLITECONNECTION_H

#include <QString>

/** SQLite's opaque connection type (from sqlite3.h). */
struct sqlite3;

namespace ts
{

/**
 * Owns one SQLite connection, opened without SQLite's internal mutex
 * (SQLITE_OPEN_NOMUTEX), so each thread opens its own connection.
 *
 * @reentrant
 */
class SqliteConnection
{
public:
  /** How the database file is opened. */
  enum class OpenMode {
    ReadOnly,       ///< The file must exist; no writes.
    ReadWrite,      ///< The file must exist.
    ReadWriteCreate ///< The file is created if it does not exist.
  };

  /** Creates a closed connection. */
  SqliteConnection() = default;

  /**
   * Opens the database at @p path.
   *
   * @param path File name, or ":memory:" for a private in-memory database.
   * @param mode Open mode.
   * @see isOpen(), errorString()
   */
  SqliteConnection(const QString& path, OpenMode mode);

  /** Closes the connection. */
  ~SqliteConnection();

  SqliteConnection(const SqliteConnection&) = delete;
  SqliteConnection& operator=(const SqliteConnection&) = delete;

  /** Takes over the connection of @p other, which is left closed. */
  SqliteConnection(SqliteConnection&& other) noexcept;

  /** Closes the current connection and takes over that of @p other. */
  SqliteConnection& operator=(SqliteConnection&& other) noexcept;

  /** Returns true if the connection is open. */
  bool isOpen() const { return m_db != nullptr; }

  /** Returns the message for the last failed operation. */
  QString errorString() const { return m_error; }

  /**
   * Runs one or more SQL statements that return no rows.
   *
   * @return true on success; on failure errorString() says why.
   */
  bool execute(const QString& sql);

  /**
   * Runs a query that returns one integer (first column of the first row).
   *
   * @param sql The query.
   * @param ok Set to true on success, false on error or no rows. May be null.
   * @return The value, or 0 on failure.
   */
  long long queryInteger(const QString& sql, bool* ok = nullptr);

  /** Returns the raw sqlite3 pointer. Ownership stays here. */
  sqlite3* get() const { return m_db; }

private:
  void close();

  sqlite3* m_db = nullptr;
  QString m_error;
};

} // namespace ts

#endif // TS_DEPS_SQLITECONNECTION_H
