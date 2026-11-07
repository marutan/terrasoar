// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/deps/dependencies.h"

#include <mutex>

#include <gdal.h>
#include <geos_c.h>
#include <proj.h>
#include <sqlite3.h>

namespace ts
{

DependencyVersions dependencyVersions()
{
  DependencyVersions versions;
  versions.gdal = QString::fromUtf8(GDALVersionInfo("RELEASE_NAME"));
  versions.proj = QString::fromUtf8(proj_info().version);
  versions.geos = QString::fromUtf8(GEOSversion());
  versions.sqlite = QString::fromUtf8(sqlite3_libversion());
  return versions;
}

void initialiseGdal()
{
  static std::once_flag once;
  std::call_once(once, []() { GDALAllRegister(); });
}

} // namespace ts
