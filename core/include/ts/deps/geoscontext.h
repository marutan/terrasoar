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
 * Owning handle for a GEOS reentrant-API context.
 */
#ifndef TS_DEPS_GEOSCONTEXT_H
#define TS_DEPS_GEOSCONTEXT_H

#include <QString>

#include <memory>

namespace ts
{

/**
 * Owns one GEOS context handle for the reentrant `_r` C API, and collects
 * the error and notice messages GEOS reports through it.
 *
 * Each thread that uses GEOS creates its own GeosContext.
 *
 * @warning GEOS's WKT reader and writer (`GEOSWKTReader_*`,
 * `GEOSWKTWriter_*`) switch the process-wide C locale while they run, so
 * they are not safe on several threads at once even with separate contexts.
 * Pass geometry between threads and read it on workers as WKB.
 *
 * @reentrant
 */
class GeosContext
{
public:
  /** Creates a new context; check isValid(). */
  GeosContext();

  /** Finishes the context. Geometries created with it must be gone. */
  ~GeosContext();

  GeosContext(const GeosContext&) = delete;
  GeosContext& operator=(const GeosContext&) = delete;

  /** Takes over the context of @p other, which is left empty. */
  GeosContext(GeosContext&& other) noexcept;

  /** Finishes the current context and takes over that of @p other. */
  GeosContext& operator=(GeosContext&& other) noexcept;

  /** Returns true if the context was created. */
  bool isValid() const { return m_handle != nullptr; }

  /** Returns the raw GEOSContextHandle_t. Ownership stays here. */
  void* get() const { return m_handle; }

  /** Returns the last error message GEOS reported, or an empty string. */
  QString lastError() const;

  /** Clears the stored error message. */
  void clearError();

private:
  struct Messages;

  void* m_handle = nullptr;
  std::unique_ptr<Messages> m_messages;
};

} // namespace ts

#endif // TS_DEPS_GEOSCONTEXT_H
