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
 * Build and version information for Terra Soar.
 */
#ifndef TS_BASE_BUILDINFO_H
#define TS_BASE_BUILDINFO_H

#include <QString>

namespace ts
{

/**
 * Facts about the running build: program version, Qt versions and compiler.
 *
 * @threadsafe
 */
struct BuildInfo {
  /** Program version, e.g. "0.0.1". */
  QString version;
  /** Qt version the program was compiled against, e.g. "5.15.13". */
  QString qtCompileVersion;
  /** Qt version loaded at run time. */
  QString qtRuntimeVersion;
  /** Compiler name and version, e.g. "GCC 13.2.0". */
  QString compiler;
  /** True for a build without optimisation and with assertions. */
  bool debugBuild = false;
};

/**
 * Returns the build information of the running program.
 *
 * @threadsafe
 */
BuildInfo buildInfo();

/**
 * Returns the user-visible program name, "Terra Soar", translated.
 *
 * @threadsafe once the translators are installed.
 */
QString programName();

} // namespace ts

#endif // TS_BASE_BUILDINFO_H
