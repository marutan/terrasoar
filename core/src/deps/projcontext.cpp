// SPDX-License-Identifier: GPL-2.0-or-later
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
