// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/deps/geoscontext.h"

#include <utility>

#include <geos_c.h>

namespace ts
{

/** Message store handed to GEOS as handler user data. */
struct GeosContext::Messages {
  QString lastError;
};

namespace
{

void errorHandler(const char* message, void* userData)
{
  *static_cast<QString*>(userData) = QString::fromUtf8(message);
}

void noticeHandler(const char* /*message*/, void* /*userData*/) {}

} // namespace

GeosContext::GeosContext() : m_messages(std::make_unique<Messages>())
{
  GEOSContextHandle_t handle = GEOS_init_r();
  if (handle != nullptr) {
    // The QString lives on the heap, so its address survives moves.
    GEOSContext_setErrorMessageHandler_r(handle, errorHandler,
                                         &m_messages->lastError);
    GEOSContext_setNoticeMessageHandler_r(handle, noticeHandler, nullptr);
  }
  m_handle = handle;
}

GeosContext::~GeosContext()
{
  if (m_handle != nullptr) {
    GEOS_finish_r(static_cast<GEOSContextHandle_t>(m_handle));
  }
}

GeosContext::GeosContext(GeosContext&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)),
      m_messages(std::move(other.m_messages))
{
}

GeosContext& GeosContext::operator=(GeosContext&& other) noexcept
{
  if (this != &other) {
    if (m_handle != nullptr) {
      GEOS_finish_r(static_cast<GEOSContextHandle_t>(m_handle));
    }
    m_handle = std::exchange(other.m_handle, nullptr);
    m_messages = std::move(other.m_messages);
  }
  return *this;
}

QString GeosContext::lastError() const
{
  return m_messages ? m_messages->lastError : QString();
}

void GeosContext::clearError()
{
  if (m_messages) {
    m_messages->lastError.clear();
  }
}

} // namespace ts
