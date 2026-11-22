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

#ifndef TS_PACKAGER_VECTORBUILDER_H
#define TS_PACKAGER_VECTORBUILDER_H

#include <cstdint>
#include <functional>
#include <vector>

#include <QString>
#include <QStringList>

#include "featurepipeline.h"

namespace ts
{
class GeoPackageFeatureWriter;
} // namespace ts

namespace ts::packager
{

/** One attribute a vector class writes. */
struct VectorAttribute {
  /** The source's field; empty for a constant (value). */
  QString column;
  /** The GeoPackage column. */
  QString name;
  /** True for an integer column (NULL where the field is empty). */
  bool number = false;
  /** The value every feature gets (text, or a number), when column is
   *  empty. */
  QString value;
};

/**
 * One class of a vector dataset: the features of one source layer that
 * pass a filter, written to one table. The sources are read through
 * GDAL/OGR (shapefiles, GeoJSON, CSV, …) and must be in longitude and
 * latitude (WGS 84).
 */
struct VectorClass {
  /** The dataset GDAL opens: a path, or a virtual one (/vsizip/…). */
  QString path;
  /** Its layer; empty for the first. */
  QString layer;
  /** GDAL open options ("SEPARATOR=TAB", "X_POSSIBLE_NAMES=lon", …). */
  QStringList openOptions;
  /** OGR SQL attribute filter (OGR_L_SetAttributeFilter); empty for all. */
  QString where;
  /** The table name before the storage level, e.g. "roads". */
  QString table;
  /** Shallowest display level it is shown at. */
  int32_t minLevel = 0;
  /** Deepest display level it is shown at, -1 for all. */
  int32_t maxLevel = -1;
  /** 0 points, 1 lines, 2 areas: the parts of other kinds are left out. */
  int32_t dimension = 1;
  /** The levels of detail (indices) it writes; empty for all. */
  std::vector<int32_t> lods;
  /** True to join the features with the same attributes into one. */
  bool dissolve = false;
  /** The attributes written (feature_id comes first, always). */
  std::vector<VectorAttribute> attributes;
  /**
   * For sources without geometry (CSV tables of runways), the fields of a
   * line's two ends: longitude and latitude of one, then of the other.
   * Features with any of them empty are left out (counted as filtered).
   */
  QStringList lineColumns;
};

/** What buildVector() converts. */
struct VectorOptions {
  /** The levels of detail, coarse to fine. */
  std::vector<FeatureLod> lods;
  /** The classes. */
  std::vector<VectorClass> classes;
  /** Called with progress messages (English); may be empty. */
  std::function<void(const QString&)> progress;
};

/**
 * Converts the classes of @p options into @p writer: each class's features
 * are read, filtered, optionally dissolved by their attributes, and
 * simplified and cut to the quadtree for each level of detail serving it,
 * into the tables `<table>_<storage level>`. Features without geometry
 * count as filtered; unreadable ones or ones with no part of the class's
 * kind as skipped. Returns false with @p errorString on failure.
 */
bool buildVector(const VectorOptions& options, GeoPackageFeatureWriter* writer,
                 std::vector<FeatureClassStats>* stats, QString* errorString);

/** Reads a class's geometry kind ("points", "lines", "areas"), or -1. */
int32_t vectorDimension(const QString& name);

} // namespace ts::packager

#endif // TS_PACKAGER_VECTORBUILDER_H
