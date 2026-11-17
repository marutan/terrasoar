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

#include "sqlitestatementpool.h"

#include <cstdint>
#include <utility>

#include <sqlite3.h>

namespace ts
{

SqliteStatementPool::Entry::~Entry()
{
  sqlite3_finalize(statement);
}

SqliteStatementPool::Lease::Lease(SqliteStatementPool* pool,
                                  std::unique_ptr<Entry> entry)
    : m_pool(pool), m_entry(std::move(entry))
{
}

SqliteStatementPool::Lease::~Lease()
{
  if (m_pool != nullptr && m_entry != nullptr) {
    m_pool->giveBack(std::move(m_entry));
  }
}

SqliteStatementPool::Lease::Lease(Lease&& other) noexcept
    : m_pool(std::exchange(other.m_pool, nullptr)),
      m_entry(std::move(other.m_entry))
{
}

sqlite3_stmt* SqliteStatementPool::Lease::statement() const
{
  if (m_entry == nullptr) {
    return nullptr;
  }
  sqlite3_reset(m_entry->statement);
  sqlite3_clear_bindings(m_entry->statement);
  return m_entry->statement;
}

SqliteStatementPool::SqliteStatementPool(QString path, QByteArray sql)
    : m_path(std::move(path)), m_sql(std::move(sql))
{
}

SqliteStatementPool::~SqliteStatementPool() = default;

SqliteStatementPool::Lease SqliteStatementPool::acquire()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_idle.empty()) {
      std::unique_ptr<Entry> entry = std::move(m_idle.back());
      m_idle.pop_back();
      return Lease(this, std::move(entry));
    }
  }
  std::unique_ptr<Entry> entry = std::make_unique<Entry>();
  entry->connection =
      SqliteConnection(m_path, SqliteConnection::OpenMode::ReadOnly);
  if (!entry->connection.isOpen() ||
      sqlite3_prepare_v2(entry->connection.get(), m_sql.constData(),
                         static_cast<int32_t>(m_sql.size()), &entry->statement,
                         nullptr) != SQLITE_OK) {
    return Lease(nullptr, nullptr);
  }
  return Lease(this, std::move(entry));
}

void SqliteStatementPool::giveBack(std::unique_ptr<Entry> entry)
{
  sqlite3_reset(entry->statement);
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_idle.push_back(std::move(entry));
}

} // namespace ts
