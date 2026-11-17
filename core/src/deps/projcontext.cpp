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

#include "ts/deps/projcontext.h"

#include <utility>

#include <proj.h>

namespace ts
{

ProjContext::ProjContext() : m_context(proj_context_create()) {}

ProjContext::~ProjContext()
{
  if (m_context != nullptr) {
    proj_context_destroy(m_context);
  }
}

ProjContext::ProjContext(ProjContext&& other) noexcept
    : m_context(std::exchange(other.m_context, nullptr))
{
}

ProjContext& ProjContext::operator=(ProjContext&& other) noexcept
{
  if (this != &other) {
    if (m_context != nullptr) {
      proj_context_destroy(m_context);
    }
    m_context = std::exchange(other.m_context, nullptr);
  }
  return *this;
}

QString ProjContext::lastErrorString() const
{
  if (m_context == nullptr) {
    return QString();
  }
  const int errorNumber = proj_context_errno(m_context);
  if (errorNumber == 0) {
    return QString();
  }
  return QString::fromUtf8(proj_context_errno_string(m_context, errorNumber));
}

} // namespace ts
