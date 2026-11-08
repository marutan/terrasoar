// SPDX-License-Identifier: GPL-2.0-or-later
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
