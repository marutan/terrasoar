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
 * Tiled vector features in a GeoPackage: the provider that reads them and
 * the writer the packager and tests use.
 */
#ifndef TS_PROVIDERS_GEOPACKAGEFEATUREPROVIDER_H
#define TS_PROVIDERS_GEOPACKAGEFEATUREPROVIDER_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QJsonArray>
#include <QString>

#include "ts/providers/tileprovider.h"
#include "ts/vector/stylerules.h"

namespace ts
{

class SqliteStatementPool;

/**
 * One feature table of a tiled GeoPackage: features cut to the quadtree
 * tiles of one storage level, serving a range of display levels.
 *
 * The GeoPackage lists its tables in an attributes table
 * `ts_feature_tables(table_name, geometry, storage_level, min_level,
 * max_level)`. Each listed table has a geometry column and the integer
 * columns `tile_level`, `tile_x` and `tile_y` (indexed); further integer
 * and text columns are feature attributes (NULL where a feature has no
 * value). See docs/data/base-pack.md.
 *
 * @reentrant
 */
struct FeatureTableSpec {
  /** Kind of geometry in the table. */
  enum class Geometry {
    Polygons, ///< (Multi)polygons, cut along tile edges.
    Lines,    ///< (Multi)line strings, cut along tile edges.
    Points,   ///< (Multi)points, each in one tile (from Phase 21).
  };
  /** Table name (letters, digits and '_'). */
  QString name;
  /** Kind of geometry. */
  Geometry geometry = Geometry::Polygons;
  /** Quadtree level the features are cut to. */
  int32_t storageLevel = 0;
  /** Shallowest display level served (at least storageLevel). */
  int32_t minLevel = 0;
  /** Deepest display level served, or -1 for every deeper level. */
  int32_t maxLevel = -1;
  /** Names of the integer attribute columns. */
  std::vector<QString> attributes;
  /** Names of the text attribute columns (after the integer ones). */
  std::vector<QString> textAttributes;
};

/**
 * Writes a tiled GeoPackage through GDAL, inside one transaction
 * committed by finish().
 *
 * @reentrant
 */
class GeoPackageFeatureWriter
{
public:
  /**
   * Creates the GeoPackage at @p path (which must not exist). Without
   * @p spatialIndex the tables have no R-tree: the provider reads by tile
   * and doesn't need one, and the VMAP0 packs are a quarter smaller
   * without (GIS programs still open them).
   */
  explicit GeoPackageFeatureWriter(const QString& path,
                                   bool spatialIndex = true);

  /** Closes the file; without a successful finish() it is incomplete. */
  ~GeoPackageFeatureWriter();

  GeoPackageFeatureWriter(const GeoPackageFeatureWriter&) = delete;
  GeoPackageFeatureWriter& operator=(const GeoPackageFeatureWriter&) = delete;
  GeoPackageFeatureWriter(GeoPackageFeatureWriter&&) = delete;
  GeoPackageFeatureWriter& operator=(GeoPackageFeatureWriter&&) = delete;

  /** Adds a table. False on failure (see errorString()). */
  bool addTable(const FeatureTableSpec& spec);

  /**
   * Adds one feature to @p table, cut to @p tile: @p wkb is ISO WKB of a
   * (multi)polygon or (multi)line string in longitude/latitude degrees,
   * @p attributes values for the table's attribute columns in order.
   */
  bool putFeature(const QString& table, TileKey tile, const QByteArray& wkb,
                  const std::vector<int64_t>& attributes = {});

  /**
   * Adds one feature to @p table, cut to @p tile, with values for the
   * table's integer and text attribute columns in order: an empty
   * optional or a null QString writes NULL. @p wkb may also be a
   * (multi)point for a point table.
   */
  bool putFeature(const QString& table, TileKey tile, const QByteArray& wkb,
                  const std::vector<std::optional<int64_t>>& integers,
                  const std::vector<QString>& texts);

  /** Writes the table list and indexes, commits and closes. */
  bool finish();

  /** Returns the last error (English), empty if none. */
  QString errorString() const { return m_error; }

private:
  struct Private;
  std::unique_ptr<Private> d;
  QString m_error;
};

/**
 * Vector features from a tiled GeoPackage (see FeatureTableSpec), for
 * example the base pack's shoreline or a VMAP0 pack.
 *
 * loadFeatures() returns, for the tables serving the key's level, the
 * features stored for the key's ancestor at the table's storage level;
 * FeatureTile::key is that ancestor. Each feature's attributes hold
 * "table" (the table name) and the table's attribute columns that aren't
 * NULL; an integer column "feature_id" also becomes Feature::id. Bad
 * geometry is skipped, never a crash.
 *
 * Options narrow it down: only some tables (by name before the storage
 * level suffix, e.g. "roads" for "roads_6"), and style rules giving
 * features their style (ts::StyleRule). Files with attributes keep the
 * tiles loaded last for picking (featuresAt()).
 *
 * @threadsafe
 */
class GeoPackageFeatureProvider : public FeatureProvider
{
public:
  /**
   * Opens the GeoPackage at @p path and reads its table list; check
   * isValid().
   */
  explicit GeoPackageFeatureProvider(const QString& path);

  /** How a provider reads the file. */
  struct Options {
    /**
     * Only the tables whose name is one of these or one of these followed
     * by "_" and digits; empty for every table.
     */
    std::vector<QString> tables;
    /** Style rules (a JSON array of ts::StyleRule) applied on loading. */
    QJsonArray styleRules;
    /** Tiles kept for featuresAt() (0 keeps none). */
    int32_t keptTiles = 48;
  };

  /** Opens the GeoPackage at @p path with @p options; check isValid(). */
  GeoPackageFeatureProvider(const QString& path, const Options& options);

  ~GeoPackageFeatureProvider() override;

  GeoPackageFeatureProvider(const GeoPackageFeatureProvider&) = delete;
  GeoPackageFeatureProvider&
  operator=(const GeoPackageFeatureProvider&) = delete;
  GeoPackageFeatureProvider(GeoPackageFeatureProvider&&) = delete;
  GeoPackageFeatureProvider& operator=(GeoPackageFeatureProvider&&) = delete;

  /** Returns true if the file opened and lists at least one table. */
  bool isValid() const { return m_valid; }

  /** Returns why it is not valid (English), or an empty string. */
  QString errorString() const { return m_error; }

  /** Returns the tables, as listed in the file. */
  std::vector<FeatureTableSpec> tables() const;

  QString id() const override;

  /** Returns the file's base name (layers have their own names). */
  QString name() const override;

  /** The deepest storage level; deeper keys get that level's features. */
  int32_t maximumLevel() const override { return m_maximumLevel; }

  std::optional<FeatureTile>
  loadFeatures(TileKey key, const std::atomic<bool>& cancelled) override;

  /** The ancestor at the storage level of the tables serving @p key. */
  TileKey storedKey(TileKey key) const override;

  /**
   * The features of the tiles loaded last that lie under @p point, from
   * the finest tiles first, each feature (by its id) once. None for a file
   * without attributes (the shoreline: nothing to show).
   */
  std::vector<Feature> featuresAt(GeoPoint point,
                                  double toleranceDeg) const override;

  /** From the kept tiles; each point of a multipoint on its own. */
  std::vector<Feature>
  labelPoints(const QString& attribute,
              const std::vector<TileKey>& tiles) const override;

private:
  struct Table;
  void keep(const FeatureTile& tile);

  QString m_path;
  QString m_error;
  bool m_valid = false;
  bool m_hasAttributes = false;
  int32_t m_maximumLevel = 0;
  Options m_options;
  std::vector<StyleRule> m_rules;
  std::vector<std::unique_ptr<Table>> m_tables;
  mutable std::mutex m_keptMutex;
  std::deque<FeatureTile> m_kept;
};

/**
 * Decodes a GeoPackage geometry blob (header and WKB) into features,
 * appended to @p features: one per polygon, one per (multi)line string.
 * Returns false (appending nothing) if the blob is malformed or of an
 * unsupported type. Coordinates are longitude/latitude degrees.
 */
bool decodeGeoPackageGeometry(const unsigned char* data, std::size_t sizeBytes,
                              std::vector<Feature>* features);

} // namespace ts

#endif // TS_PROVIDERS_GEOPACKAGEFEATUREPROVIDER_H
