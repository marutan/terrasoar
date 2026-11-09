// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Tiled vector features in a GeoPackage: the provider that reads them and
 * the writer the packager and tests use.
 */
#ifndef TS_PROVIDERS_GEOPACKAGEFEATUREPROVIDER_H
#define TS_PROVIDERS_GEOPACKAGEFEATUREPROVIDER_H

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QString>

#include "ts/providers/tileprovider.h"

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
 * columns are feature attributes. See docs/data/base-pack.md.
 *
 * @reentrant
 */
struct FeatureTableSpec {
  /** Kind of geometry in the table. */
  enum class Geometry {
    Polygons, ///< (Multi)polygons, cut along tile edges.
    Lines,    ///< (Multi)line strings, cut along tile edges.
  };
  /** Table name (letters, digits and '_'). */
  QString name;
  /** Kind of geometry. */
  Geometry geometry = Geometry::Polygons;
  /** Quadtree level the features are cut to. */
  int storageLevel = 0;
  /** Shallowest display level served (at least storageLevel). */
  int minLevel = 0;
  /** Deepest display level served, or -1 for every deeper level. */
  int maxLevel = -1;
  /** Names of the integer attribute columns. */
  std::vector<QString> attributes;
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
  /** Creates the GeoPackage at @p path (which must not exist). */
  explicit GeoPackageFeatureWriter(const QString& path);

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
                  const std::vector<long long>& attributes = {});

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
 * example the base pack's shoreline.
 *
 * loadFeatures() returns, for the tables serving the key's level, the
 * features stored for the key's ancestor at the table's storage level;
 * FeatureTile::key is that ancestor. Each feature's attributes hold
 * "table" (the table name) and the table's attribute columns. Bad
 * geometry is skipped, never a crash.
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
  int maximumLevel() const override { return m_maximumLevel; }

  std::optional<FeatureTile>
  loadFeatures(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  struct Table;
  QString m_path;
  QString m_error;
  bool m_valid = false;
  int m_maximumLevel = 0;
  std::vector<std::unique_ptr<Table>> m_tables;
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
