// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Owning handle for a PROJ thread context.
 */
#ifndef TS_DEPS_PROJCONTEXT_H
#define TS_DEPS_PROJCONTEXT_H

#include <QString>

/** PROJ's opaque context type (from proj.h). */
struct pj_ctx;

namespace ts
{

/**
 * Owns one PROJ context (PJ_CONTEXT).
 *
 * PROJ objects created with a context may only be used by one thread at a
 * time, so every thread that transforms coordinates creates its own
 * ProjContext.
 *
 * @reentrant
 */
class ProjContext
{
public:
  /** Creates a new context; check isValid(). */
  ProjContext();

  /** Destroys the context. Objects created with it must be gone by now. */
  ~ProjContext();

  ProjContext(const ProjContext&) = delete;
  ProjContext& operator=(const ProjContext&) = delete;

  /** Takes over the context of @p other, which is left empty. */
  ProjContext(ProjContext&& other) noexcept;

  /** Destroys the current context and takes over that of @p other. */
  ProjContext& operator=(ProjContext&& other) noexcept;

  /** Returns true if the context was created. */
  bool isValid() const { return m_context != nullptr; }

  /** Returns the raw PJ_CONTEXT pointer. Ownership stays here. */
  pj_ctx* get() const { return m_context; }

  /** Returns the message for the last error raised in this context. */
  QString lastErrorString() const;

private:
  pj_ctx* m_context = nullptr;
};

} // namespace ts

#endif // TS_DEPS_PROJCONTEXT_H
