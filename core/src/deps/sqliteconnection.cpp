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

#include "ts/deps/sqliteconnection.h"

#include <cstdint>
#include <utility>

#include <QByteArray>

#include <sqlite3.h>

namespace ts
{

SqliteConnection::SqliteConnection(const QString& path, OpenMode mode)
{
  int32_t flags = SQLITE_OPEN_NOMUTEX;
  switch (mode) {
  case OpenMode::ReadOnly:
    flags |= SQLITE_OPEN_READONLY;
    break;
  case OpenMode::ReadWrite:
    flags |= SQLITE_OPEN_READWRITE;
    break;
  case OpenMode::ReadWriteCreate:
    flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    break;
  }
  const QByteArray encoded = path.toUtf8();
  sqlite3* db = nullptr;
  const int32_t rc = sqlite3_open_v2(encoded.constData(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    m_error = db ? QString::fromUtf8(sqlite3_errmsg(db))
                 : QString::fromUtf8(sqlite3_errstr(rc));
    sqlite3_close(db);
    return;
  }
  m_db = db;
}

SqliteConnection::~SqliteConnection()
{
  close();
}

SqliteConnection::SqliteConnection(SqliteConnection&& other) noexcept
    : m_db(std::exchange(other.m_db, nullptr)),
      m_error(std::move(other.m_error))
{
}

SqliteConnection& SqliteConnection::operator=(SqliteConnection&& other) noexcept
{
  if (this != &other) {
    close();
    m_db = std::exchange(other.m_db, nullptr);
    m_error = std::move(other.m_error);
  }
  return *this;
}

void SqliteConnection::close()
{
  if (m_db != nullptr) {
    sqlite3_close(m_db);
    m_db = nullptr;
  }
}

bool SqliteConnection::execute(const QString& sql)
{
  if (m_db == nullptr) {
    return false;
  }
  const QByteArray encoded = sql.toUtf8();
  char* message = nullptr;
  const int32_t rc =
      sqlite3_exec(m_db, encoded.constData(), nullptr, nullptr, &message);
  if (rc != SQLITE_OK) {
    m_error = QString::fromUtf8(message ? message : sqlite3_errmsg(m_db));
    sqlite3_free(message);
    return false;
  }
  return true;
}

int64_t SqliteConnection::queryInteger(const QString& sql, bool* ok)
{
  if (ok != nullptr) {
    *ok = false;
  }
  if (m_db == nullptr) {
    return 0;
  }
  const QByteArray encoded = sql.toUtf8();
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(m_db, encoded.constData(), -1, &statement, nullptr) !=
      SQLITE_OK) {
    m_error = QString::fromUtf8(sqlite3_errmsg(m_db));
    return 0;
  }
  int64_t value = 0;
  const int32_t rc = sqlite3_step(statement);
  if (rc == SQLITE_ROW) {
    value = sqlite3_column_int64(statement, 0);
    if (ok != nullptr) {
      *ok = true;
    }
  } else if (rc != SQLITE_DONE) {
    m_error = QString::fromUtf8(sqlite3_errmsg(m_db));
  }
  sqlite3_finalize(statement);
  return value;
}

} // namespace ts
