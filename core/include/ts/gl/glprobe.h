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
 * Check that an OpenGL context of a given version can be created.
 */
#ifndef TS_GL_GLPROBE_H
#define TS_GL_GLPROBE_H

#include <cstdint>

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
  int32_t majorVersion = 0;
  /** Context version actually obtained: minor part. */
  int32_t minorVersion = 0;
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
GlProbeResult probeOpenGl(int32_t major, int32_t minor);

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
