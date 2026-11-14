// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/deps/dependencies.h"

#include <mutex>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>

#include <GeographicLib/Config.h>
#include <gdal.h>
#include <geos_c.h>
#include <ogr_srs_api.h>
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
  versions.geographicLib = QStringLiteral(GEOGRAPHICLIB_VERSION_STRING);
  return versions;
}

void initialiseGdal()
{
  static std::once_flag once;
  std::call_once(once, []() { GDALAllRegister(); });
}

namespace
{

/** The directories set by useProjData(), searched before PROJ's own. */
struct ProjPaths {
  std::mutex mutex;
  QStringList paths;
};

ProjPaths& projPathState()
{
  static ProjPaths state;
  return state;
}

/** PROJ's built-in search path, as a list. */
QStringList builtInProjPaths()
{
#ifdef Q_OS_WIN
  const QChar separator = QLatin1Char(';');
#else
  const QChar separator = QLatin1Char(':');
#endif
  const char* paths = proj_info().searchpath;
  return paths != nullptr
             ? QString::fromUtf8(paths).split(separator, Qt::SkipEmptyParts)
             : QStringList();
}

} // namespace

bool useProjData(const QString& directory)
{
  if (!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("proj.db")))) {
    return false;
  }
  initialiseGdal();
  ProjPaths& state = projPathState();
  const std::lock_guard<std::mutex> lock(state.mutex);
  QStringList& projPaths = state.paths;
  if (projPaths.isEmpty()) {
    // Before the first change, while proj_info() still gives the built-in
    // places.
    projPaths = builtInProjPaths();
  }
  projPaths.prepend(QDir::toNativeSeparators(QDir(directory).absolutePath()));
  projPaths.removeDuplicates();
  std::vector<QByteArray> encoded;
  encoded.reserve(static_cast<std::size_t>(projPaths.size()));
  for (const QString& path : projPaths) {
    encoded.push_back(path.toUtf8());
  }
  std::vector<const char*> pointers;
  pointers.reserve(encoded.size() + 1);
  for (const QByteArray& path : encoded) {
    pointers.push_back(path.constData());
  }
  proj_context_set_search_paths(nullptr, static_cast<int>(pointers.size()),
                                pointers.data());
  // GDAL keeps its own PROJ contexts (one per thread): a null-terminated
  // list for them.
  pointers.push_back(nullptr);
  OSRSetPROJSearchPaths(pointers.data());
  return true;
}

QStringList projSearchPaths()
{
  ProjPaths& state = projPathState();
  const std::lock_guard<std::mutex> lock(state.mutex);
  return state.paths.isEmpty() ? builtInProjPaths() : state.paths;
}

bool projDatabaseUsable()
{
  initialiseGdal();
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  const bool found = OSRImportFromEPSG(srs, 32632) == OGRERR_NONE;
  OSRDestroySpatialReference(srs);
  return found;
}

} // namespace ts
