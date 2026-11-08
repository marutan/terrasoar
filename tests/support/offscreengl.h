// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * An offscreen OpenGL 3.3 core context and render device for GPU tests.
 */
#ifndef TS_TESTS_OFFSCREENGL_H
#define TS_TESTS_OFFSCREENGL_H

#include <memory>

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>
#include <QSurfaceFormat>
#include <QtGlobal>

#include "ts/render/glrenderdevice.h"

namespace ts::test
{

/**
 * Creates an OpenGL 3.3 core context on an offscreen surface, makes it
 * current and creates a GlRenderDevice on it. Needs a QGuiApplication.
 *
 * GPU tests start with TS_TEST_REQUIRE_GL(gl): without OpenGL 3.3 they
 * are skipped, unless TS_REQUIRE_GL=1 (set in CI) makes that a failure.
 *
 * @guithread
 */
class OffscreenGl
{
public:
  /** Creates the context, surface and device. */
  OffscreenGl()
  {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    m_context.setFormat(format);
    if (!m_context.create()) {
      m_error = QStringLiteral("Could not create an OpenGL 3.3 context.");
      return;
    }
    m_surface.setFormat(m_context.format());
    m_surface.create();
    if (!m_context.makeCurrent(&m_surface)) {
      m_error = QStringLiteral("Could not make the context current.");
      return;
    }
    m_device = ts::GlRenderDevice::create(&m_context, &m_error);
  }

  /** Destroys the device with the context current. */
  ~OffscreenGl()
  {
    if (m_device) {
      m_context.makeCurrent(&m_surface);
      m_device.reset();
      m_context.doneCurrent();
    }
  }

  OffscreenGl(const OffscreenGl&) = delete;
  OffscreenGl& operator=(const OffscreenGl&) = delete;
  OffscreenGl(OffscreenGl&&) = delete;
  OffscreenGl& operator=(OffscreenGl&&) = delete;

  /** Returns true if the device exists. */
  bool isValid() const { return m_device != nullptr; }

  /** Returns why the device could not be created. */
  const QString& errorString() const { return m_error; }

  /** Returns the device; only when isValid(). */
  ts::GlRenderDevice& device() { return *m_device; }

  /** Makes the context current again (after other GL code ran). */
  void makeCurrent() { m_context.makeCurrent(&m_surface); }

private:
  QOpenGLContext m_context;
  QOffscreenSurface m_surface;
  std::unique_ptr<ts::GlRenderDevice> m_device;
  QString m_error;
};

/** Returns true if TS_REQUIRE_GL is set to a non-zero value. */
inline bool glRequired()
{
  return qEnvironmentVariableIntValue("TS_REQUIRE_GL") != 0;
}

} // namespace ts::test

/**
 * Skips the current test if @p gl (an OffscreenGl) has no device, or fails
 * it when TS_REQUIRE_GL=1.
 */
// A macro because QSKIP and QFAIL return from the calling test function.
// NOLINTBEGIN(cppcoreguidelines-macro-usage,cppcoreguidelines-avoid-do-while)
#define TS_TEST_REQUIRE_GL(gl)                                                 \
  do {                                                                         \
    if (!(gl).isValid()) {                                                     \
      if (ts::test::glRequired()) {                                            \
        QFAIL(qPrintable((gl).errorString()));                                 \
      }                                                                        \
      QSKIP(qPrintable(QStringLiteral("No OpenGL 3.3 (%1); set "               \
                                      "TS_REQUIRE_GL=1 to fail instead.")      \
                           .arg((gl).errorString())));                         \
    }                                                                          \
  } while (false)
// NOLINTEND(cppcoreguidelines-macro-usage,cppcoreguidelines-avoid-do-while)

#endif // TS_TESTS_OFFSCREENGL_H
