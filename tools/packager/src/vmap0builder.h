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
 * Converts feature classes of VMAP0 (read by the VPF reader) into a tiled
 * GeoPackage: attributes decoded, pieces cut by VMAP0's tiles joined,
 * simplified per level of detail and cut to the quadtree.
 */
#ifndef TS_PACKAGER_VMAP0BUILDER_H
#define TS_PACKAGER_VMAP0BUILDER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QString>

#include "featurepipeline.h"

namespace ts
{
class GeoPackageFeatureWriter;
} // namespace ts

namespace ts::packager
{

/** One attribute written for a feature class. */
struct Vmap0Attribute {
  /** How the value is written. */
  enum class Kind {
    /** Text: the value description table's text for the code, else the
     *  value itself. */
    Decoded,
    /** Text: the value as it is. */
    Text,
    /** Integer: the value, NULL where the value description table lists
     *  it (VMAP0's "Unknown" codes such as 29999). */
    Number,
  };
  /** The VPF column, e.g. "rtt". */
  QString column;
  /** The GeoPackage column, e.g. "route_type". */
  QString name;
  /** How the value is written. */
  Kind kind = Kind::Decoded;
};

/** A condition on a feature's raw VPF value. */
struct Vmap0Filter {
  /** The VPF column. */
  QString column;
  /** The values (as text, e.g. "26"). */
  std::vector<QString> values;
  /** True to keep features whose value is not listed, false for listed. */
  bool exclude = false;
};

/** One feature class converted into one table per level of detail. */
struct Vmap0Class {
  /** The coverage, e.g. "trans". */
  QString coverage;
  /** The feature class, e.g. "roadl". */
  QString featureClass;
  /** The table name before the storage level, e.g. "roads". */
  QString table;
  /** Shallowest display level it is shown at. */
  int32_t minLevel = 0;
  /** Deepest display level it is shown at, -1 for all. */
  int32_t maxLevel = -1;
  /** The attributes written (feature_id comes first, always). */
  std::vector<Vmap0Attribute> attributes;
  /** Features must pass every filter. */
  std::vector<Vmap0Filter> filters;
};

/** A level of detail (see FeatureLod). */
using Vmap0Lod = FeatureLod;

/** What to convert. */
struct Vmap0Options {
  /**
   * The VPF databases (the vmaplv0 directories), plain paths or GDAL
   * virtual paths such as /vsizip/<zip>/VMAPLV0.
   */
  std::vector<QString> databases;
  /**
   * The libraries used (e.g. "noamer"); empty for every library but the
   * world reference library (rference).
   */
  std::vector<QString> libraries;
  /** The levels of detail, coarse to fine. */
  std::vector<Vmap0Lod> lods;
  /** The feature classes. */
  std::vector<Vmap0Class> classes;
  /** Called with progress messages (English); may be empty. */
  std::function<void(const QString&)> progress;
};

/** One class's statistics (see FeatureClassStats). */
using Vmap0ClassStats = FeatureClassStats;

/**
 * Converts the feature classes of @p options into @p writer: for each
 * class, every library's features are read and filtered, their attributes
 * decoded, the pieces VMAP0 cut along its 15° and 30° tile edges joined
 * again, and the result simplified and cut to the quadtree for each level
 * of detail serving the class's levels, into the tables
 * `<table>_<storage level>`. Returns false with @p errorString on failure.
 */
bool buildVmap0(const Vmap0Options& options, GeoPackageFeatureWriter* writer,
                std::vector<Vmap0ClassStats>* stats, QString* errorString);

/**
 * Returns the table name of class @p table at level of detail @p lod:
 * `<table>_<storage level>`.
 */
QString vmap0TableName(const QString& table, const Vmap0Lod& lod);

/** Reads an attribute kind ("decoded", "text", "number"). */
std::optional<Vmap0Attribute::Kind> vmap0AttributeKind(const QString& name);

} // namespace ts::packager

#endif // TS_PACKAGER_VMAP0BUILDER_H
