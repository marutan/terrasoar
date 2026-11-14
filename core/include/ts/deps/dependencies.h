// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Versions and one-time set-up of the third-party libraries.
 */
#ifndef TS_DEPS_DEPENDENCIES_H
#define TS_DEPS_DEPENDENCIES_H

#include <QString>
#include <QStringList>

namespace ts
{

/**
 * Release versions of the linked third-party libraries, as reported by the
 * libraries themselves at run time.
 *
 * @threadsafe
 */
struct DependencyVersions {
  /** GDAL release name, e.g. "3.8.4". */
  QString gdal;
  /** PROJ version, e.g. "9.4.0". */
  QString proj;
  /** GEOS C API version, e.g. "3.12.1-CAPI-1.18.1". */
  QString geos;
  /** SQLite library version, e.g. "3.45.1". */
  QString sqlite;
  /**
   * GeographicLib version, e.g. "2.3". GeographicLib has no run-time query,
   * so this is the version of the headers the core was built with.
   */
  QString geographicLib;
};

/**
 * Returns the versions of GDAL, PROJ, GEOS, SQLite and GeographicLib.
 *
 * @threadsafe
 */
DependencyVersions dependencyVersions();

/**
 * Registers the GDAL drivers. Runs the registration once per process; later
 * calls return at once. Call it before opening a dataset (GdalDataset does).
 *
 * This is the one piece of process-wide set-up in the core; GDAL itself
 * keeps its driver registry global.
 *
 * @threadsafe
 */
void initialiseGdal();

/**
 * Makes PROJ (its default context) and GDAL look for their database
 * (proj.db) and grids in @p directory before PROJ's built-in places. The
 * packages ship proj.db next to the program, since the built-in place is
 * where the library was built. Call it at start-up, before any coordinate
 * system work. Returns false, changing nothing, if there's no proj.db in
 * @p directory.
 *
 * @threadsafe
 */
bool useProjData(const QString& directory);

/**
 * Returns the directories PROJ looks in for proj.db and grids: those of
 * useProjData(), then PROJ's built-in ones. For PROJ contexts of their own
 * (GeoidModel).
 *
 * @threadsafe
 */
QStringList projSearchPaths();

/**
 * Returns true if GDAL can look up a coordinate reference system by its
 * EPSG code (UTM zone 32 N), that is, if PROJ's database is there.
 *
 * @threadsafe
 */
bool projDatabaseUsable();

} // namespace ts

#endif // TS_DEPS_DEPENDENCIES_H
