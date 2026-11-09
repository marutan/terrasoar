// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A pool of read-only SQLite connections, each with one prepared statement
 * (private to the providers).
 */
#ifndef TS_PROVIDERS_SQLITESTATEMENTPOOL_H
#define TS_PROVIDERS_SQLITESTATEMENTPOOL_H

#include <memory>
#include <mutex>
#include <vector>

#include <QByteArray>
#include <QString>

#include "ts/deps/sqliteconnection.h"

struct sqlite3_stmt;

namespace ts
{

/**
 * Lends out read-only connections to one database file, each with one
 * statement prepared from fixed SQL. A connection is used by one thread
 * at a time (SQLite connections are opened without their own mutex), and
 * is returned to the pool afterwards, so a provider needs at most one per
 * worker thread.
 *
 * @threadsafe
 */
class SqliteStatementPool
{
public:
  /** One connection with its prepared statement. */
  struct Entry {
    SqliteConnection connection;       ///< The connection.
    sqlite3_stmt* statement = nullptr; ///< Prepared from the pool's SQL.
    Entry() = default;
    ~Entry();
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;
  };

  /** A borrowed entry, given back when destroyed. */
  class Lease
  {
  public:
    Lease(SqliteStatementPool* pool, std::unique_ptr<Entry> entry);
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&&) = delete;

    /** Returns the statement, reset and with its bindings cleared. */
    sqlite3_stmt* statement() const;

    /** Returns true if a connection could be opened and prepared. */
    explicit operator bool() const { return m_entry != nullptr; }

  private:
    SqliteStatementPool* m_pool = nullptr;
    std::unique_ptr<Entry> m_entry;
  };

  /** Lends connections to @p path, preparing @p sql on each. */
  SqliteStatementPool(QString path, QByteArray sql);

  /** Closes the idle connections. No lease may be outstanding. */
  ~SqliteStatementPool();

  SqliteStatementPool(const SqliteStatementPool&) = delete;
  SqliteStatementPool& operator=(const SqliteStatementPool&) = delete;
  SqliteStatementPool(SqliteStatementPool&&) = delete;
  SqliteStatementPool& operator=(SqliteStatementPool&&) = delete;

  /**
   * Borrows an idle entry or opens a new one. The lease is empty (false)
   * if the file cannot be opened or the SQL not prepared.
   */
  Lease acquire();

private:
  void giveBack(std::unique_ptr<Entry> entry);

  const QString m_path;
  const QByteArray m_sql;
  std::mutex m_mutex;
  std::vector<std::unique_ptr<Entry>> m_idle;
};

} // namespace ts

#endif // TS_PROVIDERS_SQLITESTATEMENTPOOL_H
