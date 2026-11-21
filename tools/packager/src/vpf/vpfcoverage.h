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
 * The structure of a VPF database: libraries, their tiles and coverages,
 * feature classes, and the features with their geometry built from the
 * primitives (edges, faces, nodes and text) and the winged-edge topology.
 */
#ifndef TS_PACKAGER_VPF_VPFCOVERAGE_H
#define TS_PACKAGER_VPF_VPFCOVERAGE_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QString>

#include "vpftable.h"

namespace ts::packager::vpf
{

/** A rectangle in the database's coordinates (degrees in VMAP0). */
struct Bounds {
  /** West. */
  double xmin = 0.0;
  /** South. */
  double ymin = 0.0;
  /** East. */
  double xmax = 0.0;
  /** North. */
  double ymax = 0.0;
};

/** One library of a database, from the library attribute table (lat). */
struct LibraryInfo {
  /** Name (lower case), e.g. "soamafr". */
  QString name;
  /** Its extent. */
  Bounds bounds;
};

/**
 * A VPF database: a directory with the database header table (dht) and
 * the library attribute table (lat), each library a directory below it.
 *
 * @reentrant
 */
struct Database {
  /** The directory (plain or /vsizip/…). */
  QString path;
  /** Name, from the dht. */
  QString name;
  /** Description, from the dht. */
  QString description;
  /** The libraries. */
  std::vector<LibraryInfo> libraries;

  /** Opens the database at @p path. */
  static std::optional<Database> open(const QString& path,
                                      QString* errorString);
};

/** One coverage of a library, from its coverage attribute table (cat). */
struct CoverageInfo {
  /** Name (lower case), e.g. "hydro". */
  QString name;
  /** Description. */
  QString description;
  /** Topology level (0–3). */
  int32_t level = 0;
};

/**
 * A library: a directory with the coverage attribute table (cat) and,
 * for a tiled library, the tile reference coverage (tileref), which names
 * each tile's directory and holds its extent.
 *
 * @reentrant
 */
struct Library {
  /** The directory. */
  QString path;
  /** Name (lower case). */
  QString name;
  /** The coverages. */
  std::vector<CoverageInfo> coverages;
  /** Tile id → directory below each coverage ("k/g" as "K/G"). */
  std::map<int32_t, QString> tilePaths;
  /** Tile id → extent. */
  std::map<int32_t, Bounds> tileBounds;

  /** Returns true if the library is tiled. */
  bool isTiled() const { return !tilePaths.empty(); }

  /** Opens library @p name of @p database. */
  static std::optional<Library> open(const Database& database,
                                     const QString& name, QString* errorString);
};

/** What a feature class is made of. */
enum class FeatureType {
  Point, ///< Entity or connected nodes (*.pft).
  Line,  ///< Edges (*.lft).
  Area,  ///< Faces (*.aft).
  Text,  ///< Text primitives (*.tft).
};

/** One feature class of a coverage, from its feature class schema (fcs). */
struct FeatureClass {
  /** Name (lower case), e.g. "watrcrsl". */
  QString name;
  /** Its feature table, e.g. "watrcrsl.lft". */
  QString table;
  /** What it is made of. */
  FeatureType type = FeatureType::Line;
  /** The primitive table: "edg", "fac", "end", "cnd" or "txt". */
  QString primitive;
  /**
   * The feature table's column with the primitive's id ("edg_id"), or
   * empty if the primitive table names the feature instead
   * ("<table>_id" in the primitive table).
   */
  QString key;
};

/** One feature read from a feature table, with its geometry. */
struct Feature {
  /** Its row in the feature table (0-based). */
  int32_t row = -1;
  /** Its id in the feature table. */
  int32_t id = -1;
  /** Its tile id, or 0 in an untiled library. */
  int32_t tile = 0;
  /** What it is. */
  FeatureType type = FeatureType::Line;
  /**
   * The geometry: one part with one coordinate for a point or text, one
   * part for a line (several if it is made of several edges), and for an
   * area its outer ring first, then its holes (each closed).
   */
  std::vector<std::vector<Coordinate>> parts;
  /** The string of a text feature. */
  QString text;
};

/** Counters of Coverage::readFeatures(). */
struct ReadStats {
  /** Feature rows read. */
  int64_t features = 0;
  /** Features without usable geometry (missing primitive, open ring). */
  int64_t skipped = 0;
  /** Messages about the skipped ones (the first few). */
  std::vector<QString> problems;
};

/**
 * One coverage of a library: its feature classes, feature tables, value
 * description tables, and primitives (per tile in a tiled library).
 *
 * @reentrant
 */
class Coverage
{
public:
  /** Opens coverage @p name of @p library. */
  static std::optional<Coverage>
  open(const Library& library, const QString& name, QString* errorString);

  Coverage(const Coverage&) = delete;
  Coverage& operator=(const Coverage&) = delete;
  /** Moves. */
  Coverage(Coverage&&) noexcept;
  /** Moves. */
  Coverage& operator=(Coverage&&) noexcept;
  ~Coverage();

  /** Returns the coverage's directory. */
  const QString& path() const { return m_path; }
  /** Returns its name. */
  const QString& name() const { return m_name; }
  /** Returns its feature classes, in the fcs's order. */
  const std::vector<FeatureClass>& featureClasses() const { return m_classes; }
  /** Returns the feature class @p name, or null. */
  const FeatureClass* featureClass(const QString& name) const;

  /**
   * Returns the feature table @p table ("roadl.lft"), read on first use;
   * null with @p errorString if it can't be read.
   */
  const Table* table(const QString& table, QString* errorString);

  /**
   * Returns the description of @p value of @p column in @p table from the
   * coverage's value description tables (char.vdt for text, int.vdt for
   * numbers), or an empty string if they don't list it.
   */
  QString valueDescription(const QString& table, const QString& column,
                           const QString& value) const;

  /**
   * Reads every feature of @p featureClass with its geometry, appended to
   * @p features in feature table order. Features whose primitives are
   * missing or don't close are counted in @p stats and left out. Returns
   * false with @p errorString if a table can't be read.
   */
  bool readFeatures(const FeatureClass& featureClass,
                    std::vector<Feature>* features, ReadStats* stats,
                    QString* errorString);

private:
  struct Primitives;
  Coverage() = default;
  Primitives* primitives(int32_t tile, QString* errorString);
  static bool buildFeature(Primitives& primitives,
                           const FeatureClass& featureClass,
                           std::vector<int64_t> keys, Feature* feature,
                           QString* problem, QString* errorString);
  static bool buildLineFeature(Primitives& primitives,
                               const FeatureClass& featureClass,
                               std::vector<int64_t> keys, Feature* feature,
                               QString* problem, QString* errorString);
  static bool buildAreaFeature(Primitives& primitives,
                               const FeatureClass& featureClass,
                               std::vector<int64_t> keys, Feature* feature,
                               QString* problem, QString* errorString);
  static bool buildNodeFeature(Primitives& primitives,
                               const FeatureClass& featureClass,
                               std::vector<int64_t> keys, Feature* feature,
                               QString* problem, QString* errorString);

  QString m_path;
  QString m_name;
  const Library* m_library = nullptr;
  std::map<int32_t, QString> m_tilePaths;
  std::vector<FeatureClass> m_classes;
  std::map<QString, std::unique_ptr<Table>> m_tables;
  std::map<QString, QString> m_descriptions;
  std::map<int32_t, std::unique_ptr<Primitives>> m_primitives;
};

} // namespace ts::packager::vpf

#endif // TS_PACKAGER_VPF_VPFCOVERAGE_H
