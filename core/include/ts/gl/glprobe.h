// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Check that an OpenGL context of a given version can be created.
 */
#ifndef TS_GL_GLPROBE_H
#define TS_GL_GLPROBE_H

#include <QString>

namespace ts
{

/**
 * What an OpenGL probe found.
 *
 * @reentrant
 */
struct GlProbeResult {
  /** True if a context of at least the requested version was made current. */
  bool ok = false;
  /** Context version actually obtained: major part. */
  int majorVersion = 0;
  /** Context version actually obtained: minor part. */
  int minorVersion = 0;
  /** True if the context uses the core profile. */
  bool coreProfile = false;
  /** GL_VENDOR string. */
  QString vendor;
  /** GL_RENDERER string. */
  QString renderer;
  /** GL_VERSION string. */
  QString version;
  /** True if the renderer is a known software rasteriser. */
  bool softwareRenderer = false;
  /** Translated reason for failure; empty when ok is true. */
  QString errorString;
};

/**
 * Creates an offscreen OpenGL context of at least version
 * @p major.@p minor, core profile, and reports what it got.
 *
 * Needs a QGuiApplication.
 *
 * @guithread
 *
 * @param major Minimum major version, e.g. 3.
 * @param minor Minimum minor version, e.g. 3.
 */
GlProbeResult probeOpenGl(int major, int minor);

/**
 * Returns true if @p renderer (a GL_RENDERER string) names a software
 * rasteriser: Mesa llvmpipe/softpipe/swrast, Microsoft WARP or GDI, or
 * SwiftShader.
 *
 * @threadsafe
 */
bool isSoftwareRenderer(const QString& renderer);

} // namespace ts

#endif // TS_GL_GLPROBE_H
