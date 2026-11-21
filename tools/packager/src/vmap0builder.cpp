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

#include "vmap0builder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QJsonArray>
#include <QString>

#define GEOS_USE_ONLY_R_API
#include <geos_c.h>

#include "ts/deps/geoscontext.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/tiling/tilekey.h"
#include "vpf/vpfcoverage.h"

namespace ts::packager
{

namespace
{

bool fail(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return false;
}

/** Destroys a GEOS geometry with its context. */
struct GeomDeleter {
  GEOSContextHandle_t context = nullptr;
  void operator()(GEOSGeometry* geometry) const
  {
    if (geometry != nullptr) {
      GEOSGeom_destroy_r(context, geometry);
    }
  }
};
using GeomPtr = std::unique_ptr<GEOSGeometry, GeomDeleter>;

/** A longitude/latitude bounding box, degrees. */
struct Box {
  double west = 0.0;
  double south = 0.0;
  double east = 0.0;
  double north = 0.0;

  bool overlaps(const Box& other) const
  {
    return west <= other.east && east >= other.west && south <= other.north &&
           north >= other.south;
  }
  bool inside(const Box& other) const
  {
    return west >= other.west && east <= other.east && south >= other.south &&
           north <= other.north;
  }
};

Box boxOf(TileKey key)
{
  const GeoRect rect = key.bounds();
  return Box{rect.westDeg, rect.southDeg, rect.eastDeg, rect.northDeg};
}

/** The GEOS multi-geometry type for features of @p dimension. */
int32_t multiType(int32_t dimension)
{
  switch (dimension) {
  case 2:
    return GEOS_MULTIPOLYGON;
  case 1:
    return GEOS_MULTILINESTRING;
  default:
    return GEOS_MULTIPOINT;
  }
}

/** The dimension of a single GEOS geometry type: 0, 1 or 2. */
int32_t dimensionOfGeosType(int32_t type)
{
  switch (type) {
  case GEOS_POINT:
    return 0;
  case GEOS_LINESTRING:
  case GEOS_LINEARRING:
    return 1;
  default:
    return 2;
  }
}

/** GEOS work: context, WKB writer and the operations used. */
class Geos
{
public:
  Geos()
      : m_context(static_cast<GEOSContextHandle_t>(m_owner.get())),
        m_writer(GEOSWKBWriter_create_r(m_context))
  {
    GEOSWKBWriter_setOutputDimension_r(m_context, m_writer, 2);
    GEOSWKBWriter_setByteOrder_r(m_context, m_writer, GEOS_WKB_NDR);
  }
  ~Geos() { GEOSWKBWriter_destroy_r(m_context, m_writer); }
  Geos(const Geos&) = delete;
  Geos& operator=(const Geos&) = delete;
  Geos(Geos&&) = delete;
  Geos& operator=(Geos&&) = delete;

  GEOSContextHandle_t context() const { return m_context; }

  GeomPtr wrap(GEOSGeometry* geometry) const
  {
    return GeomPtr(geometry, GeomDeleter{m_context});
  }
  GeomPtr none() const { return wrap(nullptr); }

  QByteArray toWkb(const GEOSGeometry* geometry) const
  {
    std::size_t size = 0;
    unsigned char* data =
        GEOSWKBWriter_write_r(m_context, m_writer, geometry, &size);
    if (data == nullptr) {
      return QByteArray();
    }
    QByteArray bytes(reinterpret_cast<const char*>(data),
                     static_cast<int32_t>(size));
    GEOSFree_r(m_context, data);
    return bytes;
  }

  Box box(const GEOSGeometry* geometry) const
  {
    Box result;
    GEOSGeom_getXMin_r(m_context, geometry, &result.west);
    GEOSGeom_getYMin_r(m_context, geometry, &result.south);
    GEOSGeom_getXMax_r(m_context, geometry, &result.east);
    GEOSGeom_getYMax_r(m_context, geometry, &result.north);
    return result;
  }

  bool isEmpty(const GEOSGeometry* geometry) const
  {
    return geometry == nullptr || GEOSisEmpty_r(m_context, geometry) != 0;
  }

  GEOSCoordSequence* sequence(const std::vector<vpf::Coordinate>& points) const
  {
    GEOSCoordSequence* sequence = GEOSCoordSeq_create_r(
        m_context, static_cast<uint32_t>(points.size()), 2);
    for (std::size_t i = 0; i < points.size(); ++i) {
      GEOSCoordSeq_setXY_r(m_context, sequence, static_cast<uint32_t>(i),
                           points[i].x, points[i].y);
    }
    return sequence;
  }

  GeomPtr point(const vpf::Coordinate& point) const
  {
    return wrap(GEOSGeom_createPointFromXY_r(m_context, point.x, point.y));
  }

  GeomPtr line(const std::vector<std::vector<vpf::Coordinate>>& parts) const
  {
    std::vector<GEOSGeometry*> lines;
    for (const std::vector<vpf::Coordinate>& part : parts) {
      if (part.size() >= 2) {
        lines.push_back(GEOSGeom_createLineString_r(m_context, sequence(part)));
      }
    }
    if (lines.empty()) {
      return none();
    }
    if (lines.size() == 1) {
      return wrap(lines.front());
    }
    return wrap(GEOSGeom_createCollection_r(
        m_context, GEOS_MULTILINESTRING, lines.data(),
        static_cast<uint32_t>(lines.size())));
  }

  /** The polygon of @p rings (outer first), valid; null if degenerate. */
  GeomPtr polygon(const std::vector<std::vector<vpf::Coordinate>>& rings,
                  int64_t* repaired) const
  {
    if (rings.empty() || rings.front().size() < 4) {
      return none();
    }
    GEOSGeometry* shell =
        GEOSGeom_createLinearRing_r(m_context, sequence(rings.front()));
    std::vector<GEOSGeometry*> holes;
    for (std::size_t i = 1; i < rings.size(); ++i) {
      if (rings[i].size() >= 4) {
        holes.push_back(
            GEOSGeom_createLinearRing_r(m_context, sequence(rings[i])));
      }
    }
    GeomPtr polygon = wrap(GEOSGeom_createPolygon_r(
        m_context, shell, holes.data(), static_cast<uint32_t>(holes.size())));
    return valid(std::move(polygon), repaired);
  }

  /** Appends the parts of @p geometry of @p wanted's dimension, cloned. */
  void collect(const GEOSGeometry* geometry, int32_t dimension,
               std::vector<GEOSGeometry*>* parts) const
  {
    const int32_t type = GEOSGeomTypeId_r(m_context, geometry);
    if (type == GEOS_MULTIPOINT || type == GEOS_MULTILINESTRING ||
        type == GEOS_MULTIPOLYGON || type == GEOS_GEOMETRYCOLLECTION) {
      const int32_t count = GEOSGetNumGeometries_r(m_context, geometry);
      for (int32_t i = 0; i < count; ++i) {
        collect(GEOSGetGeometryN_r(m_context, geometry, i), dimension, parts);
      }
      return;
    }
    if (dimensionOfGeosType(type) == dimension && !isEmpty(geometry)) {
      parts->push_back(
          type == GEOS_LINEARRING
              ? GEOSGeom_createLineString_r(
                    m_context,
                    GEOSCoordSeq_clone_r(
                        m_context, GEOSGeom_getCoordSeq_r(m_context, geometry)))
              : GEOSGeom_clone_r(m_context, geometry));
    }
  }

  /**
   * Returns only the parts of @p geometry of @p dimension (0 points, 1
   * lines, 2 polygons), as one geometry; null if there are none.
   */
  GeomPtr keep(GeomPtr geometry, int32_t dimension) const
  {
    if (geometry == nullptr || isEmpty(geometry.get())) {
      return none();
    }
    const int32_t type = GEOSGeomTypeId_r(m_context, geometry.get());
    if ((dimension == 2 &&
         (type == GEOS_POLYGON || type == GEOS_MULTIPOLYGON)) ||
        (dimension == 1 &&
         (type == GEOS_LINESTRING || type == GEOS_MULTILINESTRING)) ||
        (dimension == 0 && (type == GEOS_POINT || type == GEOS_MULTIPOINT))) {
      return geometry;
    }
    std::vector<GEOSGeometry*> parts;
    collect(geometry.get(), dimension, &parts);
    if (parts.empty()) {
      return none();
    }
    if (parts.size() == 1) {
      return wrap(parts.front());
    }
    return wrap(GEOSGeom_createCollection_r(
        m_context, multiType(dimension), parts.data(),
        static_cast<uint32_t>(parts.size())));
  }

  /** Makes a polygonal @p geometry valid if it is not; counts repairs. */
  GeomPtr valid(GeomPtr geometry, int64_t* repaired) const
  {
    if (geometry == nullptr || GEOSisValid_r(m_context, geometry.get()) == 1) {
      return geometry;
    }
    if (repaired != nullptr) {
      *repaired += 1;
    }
    return keep(wrap(GEOSMakeValid_r(m_context, geometry.get())), 2);
  }

  GeomPtr clip(const GEOSGeometry* geometry, const Box& box,
               int32_t dimension) const
  {
    return keep(wrap(GEOSClipByRect_r(m_context, geometry, box.west, box.south,
                                      box.east, box.north)),
                dimension);
  }

  int64_t vertices(const GEOSGeometry* geometry) const
  {
    return GEOSGetNumCoordinates_r(m_context, geometry);
  }

private:
  GeosContext m_owner;
  GEOSContextHandle_t m_context = nullptr;
  GEOSWKBWriter* m_writer = nullptr;
};

/** Returns the dimension of a feature type: 0 points, 1 lines, 2 areas. */
int32_t dimensionOf(vpf::FeatureType type)
{
  switch (type) {
  case vpf::FeatureType::Line:
    return 1;
  case vpf::FeatureType::Area:
    return 2;
  default:
    return 0;
  }
}

/** One feature on its way into the pack. */
struct Item {
  GeomPtr geometry;
  std::vector<std::optional<int64_t>> integers;
  std::vector<QString> texts;
  /** The attributes as one string: pieces with the same one are joined. */
  QString key;
};

/**
 * Dissolves @p items by their attributes: all the pieces of one key become
 * one feature, lines merged end to end, areas united (so the pieces VMAP0
 * cut along its tile edges are joined, and so are neighbours with the same
 * attributes), points gathered. Returns how many pieces were merged into
 * another.
 */
int64_t dissolve(const Geos& geos, int32_t dimension, std::vector<Item>* items,
                 int64_t* repaired)
{
  GEOSContextHandle_t context = geos.context();
  std::map<QString, std::vector<std::size_t>> members;
  for (std::size_t i = 0; i < items->size(); ++i) {
    members[(*items)[i].key].push_back(i);
  }
  int64_t joined = 0;
  std::vector<Item> dissolved;
  dissolved.reserve(members.size());
  for (std::pair<const QString, std::vector<std::size_t>>& group : members) {
    Item& first = (*items)[group.second.front()];
    if (group.second.size() > 1) {
      std::vector<GEOSGeometry*> parts;
      for (const std::size_t member : group.second) {
        geos.collect((*items)[member].geometry.get(), dimension, &parts);
        (*items)[member].geometry.reset();
      }
      // Areas are united below: any collection will do.
      const int32_t type =
          dimension == 2 ? GEOS_GEOMETRYCOLLECTION : multiType(dimension);
      GeomPtr collection = geos.wrap(GEOSGeom_createCollection_r(
          context, type, parts.data(), static_cast<uint32_t>(parts.size())));
      if (dimension == 2) {
        collection = geos.valid(
            geos.keep(geos.wrap(GEOSUnaryUnion_r(context, collection.get())),
                      2),
            repaired);
      } else if (dimension == 1) {
        collection =
            geos.keep(geos.wrap(GEOSLineMerge_r(context, collection.get())), 1);
      }
      if (collection == nullptr) {
        continue;
      }
      first.geometry = std::move(collection);
      joined += static_cast<int64_t>(group.second.size()) - 1;
    }
    dissolved.push_back(std::move(first));
  }
  *items = std::move(dissolved);
  return joined;
}

/** True for VPF's null values of short and long integers. */
bool isNullInteger(int64_t value)
{
  return value == -32768 || value == -32767 || value == -2147483647 ||
         value == -2147483647 - 1;
}

/** The table geometry of features of @p type. */
FeatureTableSpec::Geometry geometryOf(vpf::FeatureType type)
{
  switch (type) {
  case vpf::FeatureType::Area:
    return FeatureTableSpec::Geometry::Polygons;
  case vpf::FeatureType::Line:
    return FeatureTableSpec::Geometry::Lines;
  default:
    return FeatureTableSpec::Geometry::Points;
  }
}

/** The tables one class writes, per level of detail (empty name: none). */
std::vector<FeatureTableSpec> tableSpecs(const Vmap0Class& cls,
                                         vpf::FeatureType type,
                                         const std::vector<Vmap0Lod>& lods)
{
  std::vector<FeatureTableSpec> specs;
  for (const Vmap0Lod& lod : lods) {
    FeatureTableSpec spec;
    const int32_t minLevel = std::max(lod.minLevel, cls.minLevel);
    int32_t maxLevel = lod.maxLevel;
    if (cls.maxLevel >= 0) {
      maxLevel = maxLevel < 0 ? cls.maxLevel : std::min(maxLevel, cls.maxLevel);
    }
    if (maxLevel >= 0 && maxLevel < minLevel) {
      specs.push_back(spec); // Not shown at these levels.
      continue;
    }
    spec.name = vmap0TableName(cls.table, lod);
    spec.geometry = geometryOf(type);
    spec.storageLevel = lod.storageLevel;
    spec.minLevel = minLevel;
    spec.maxLevel = maxLevel;
    spec.attributes.push_back(QStringLiteral("feature_id"));
    for (const Vmap0Attribute& attribute : cls.attributes) {
      if (attribute.kind == Vmap0Attribute::Kind::Number) {
        spec.attributes.push_back(attribute.name);
      } else {
        spec.textAttributes.push_back(attribute.name);
      }
    }
    specs.push_back(spec);
  }
  return specs;
}

/** Returns true if @p library has coverage @p name. */
bool hasCoverage(const vpf::Library& library, const QString& name)
{
  return std::any_of(
      library.coverages.begin(), library.coverages.end(),
      [&name](const vpf::CoverageInfo& info) { return info.name == name; });
}

/** One class's conversion. */
class ClassBuild
{
public:
  ClassBuild(const Geos& geos, const Vmap0Options& options,
             const Vmap0Class& cls, GeoPackageFeatureWriter* writer,
             Vmap0ClassStats* stats, int64_t* nextId)
      : m_geos(&geos), m_options(&options), m_class(&cls), m_writer(writer),
        m_stats(stats), m_nextId(nextId)
  {
  }

  /** Reads the class from every library that has its coverage. */
  bool readAll(const std::vector<vpf::Library>& libraries, QString* errorString)
  {
    for (const vpf::Library& library : libraries) {
      if (!hasCoverage(library, m_class->coverage)) {
        continue;
      }
      std::optional<vpf::Coverage> coverage =
          vpf::Coverage::open(library, m_class->coverage, errorString);
      if (!coverage || !read(*coverage, errorString)) {
        return false;
      }
    }
    return true;
  }

  /** Reads the class from one library's coverage. */
  bool read(vpf::Coverage& coverage, QString* errorString)
  {
    const vpf::FeatureClass* featureClass =
        coverage.featureClass(m_class->featureClass);
    if (featureClass == nullptr) {
      return true; // Not every library has every class.
    }
    m_type = featureClass->type;
    m_typeKnown = true;
    const vpf::Table* table = coverage.table(featureClass->table, errorString);
    if (table == nullptr) {
      return false;
    }
    std::vector<vpf::Feature> features;
    vpf::ReadStats readStats;
    if (!coverage.readFeatures(*featureClass, &features, &readStats,
                               errorString)) {
      return false;
    }
    m_stats->sourceFeatures += readStats.features;
    m_stats->skipped += readStats.skipped;
    for (const QString& problem : readStats.problems) {
      m_problems.push_back(problem);
    }
    std::vector<int32_t> filterColumns;
    filterColumns.reserve(m_class->filters.size());
    for (const Vmap0Filter& filter : m_class->filters) {
      filterColumns.push_back(table->columnIndex(filter.column));
    }
    std::vector<int32_t> columns;
    columns.reserve(m_class->attributes.size());
    for (const Vmap0Attribute& attribute : m_class->attributes) {
      const int32_t column = table->columnIndex(attribute.column);
      if (column < 0) {
        return fail(errorString, QStringLiteral("%1 has no column %2")
                                     .arg(table->path(), attribute.column));
      }
      columns.push_back(column);
    }
    for (const vpf::Feature& feature : features) {
      if (!passesFilters(*table, feature.row, filterColumns)) {
        m_stats->filtered += 1;
        continue;
      }
      Item item;
      item.geometry = geometryOfFeature(feature);
      if (item.geometry == nullptr) {
        m_stats->degenerate += 1;
        m_problems.push_back(
            QStringLiteral("%1: feature %2 (tile %3) is degenerate")
                .arg(table->path())
                .arg(feature.id)
                .arg(feature.tile));
        continue;
      }
      decode(coverage, *featureClass, *table, feature.row, columns, &item);
      m_items.push_back(std::move(item));
    }
    return true;
  }

  /** Dissolves the pieces by their attributes, then writes every level. */
  bool finish(QString* errorString)
  {
    if (!m_typeKnown) {
      m_stats->rows.assign(m_options->lods.size(), 0);
      m_stats->vertices.assign(m_options->lods.size(), 0);
      m_stats->dropped.assign(m_options->lods.size(), 0);
      return true;
    }
    const int32_t dimension = dimensionOf(m_type);
    m_stats->joined =
        dissolve(*m_geos, dimension, &m_items, &m_stats->repaired);
    m_stats->features = static_cast<int64_t>(m_items.size());
    for (Item& item : m_items) {
      item.integers.insert(item.integers.begin(),
                           std::optional<int64_t>(*m_nextId));
      *m_nextId += 1;
    }
    const std::vector<FeatureTableSpec> specs =
        tableSpecs(*m_class, m_type, m_options->lods);
    for (std::size_t l = 0; l < m_options->lods.size(); ++l) {
      m_stats->rows.push_back(0);
      m_stats->vertices.push_back(0);
      m_stats->dropped.push_back(0);
      if (specs[l].name.isEmpty()) {
        continue;
      }
      if (m_options->progress) {
        m_options->progress(QStringLiteral("  %1: %2 features")
                                .arg(specs[l].name)
                                .arg(m_items.size()));
      }
      if (!writeLod(l, specs[l], dimension, errorString)) {
        return false;
      }
    }
    return true;
  }

  const std::vector<QString>& problems() const { return m_problems; }

  /** Returns the feature type, if a library had the class. */
  std::optional<vpf::FeatureType> type() const
  {
    return m_typeKnown ? std::optional<vpf::FeatureType>(m_type) : std::nullopt;
  }

private:
  /** Returns true if row @p row of @p table passes every filter. */
  bool passesFilters(const vpf::Table& table, int32_t row,
                     const std::vector<int32_t>& columns) const
  {
    for (std::size_t f = 0; f < m_class->filters.size(); ++f) {
      const Vmap0Filter& filter = m_class->filters[f];
      const QString value = table.text(row, columns[f]);
      const bool listed = std::find(filter.values.begin(), filter.values.end(),
                                    value) != filter.values.end();
      if (listed == filter.exclude) {
        return false;
      }
    }
    return true;
  }

  /** The feature's geometry, valid; null if degenerate. */
  GeomPtr geometryOfFeature(const vpf::Feature& feature)
  {
    for (const std::vector<vpf::Coordinate>& part : feature.parts) {
      m_stats->sourceVertices += static_cast<int64_t>(part.size());
    }
    switch (feature.type) {
    case vpf::FeatureType::Area:
      return m_geos->polygon(feature.parts, &m_stats->repaired);
    case vpf::FeatureType::Line:
      return m_geos->line(feature.parts);
    default:
      return m_geos->point(feature.parts.front().front());
    }
  }

  /** Decodes the class's attributes of row @p row into @p item. */
  void decode(const vpf::Coverage& coverage,
              const vpf::FeatureClass& featureClass, const vpf::Table& table,
              int32_t row, const std::vector<int32_t>& columns,
              Item* item) const
  {
    for (std::size_t a = 0; a < m_class->attributes.size(); ++a) {
      const Vmap0Attribute& attribute = m_class->attributes[a];
      const QString raw = table.text(row, columns[a]).trimmed();
      const QString description = coverage.valueDescription(
          featureClass.table,
          table.columns().at(static_cast<std::size_t>(columns[a])).name, raw);
      if (attribute.kind == Vmap0Attribute::Kind::Number) {
        const int64_t value = table.integer(row, columns[a]);
        const bool null =
            !description.isEmpty() || isNullInteger(value) || raw.isEmpty();
        item->integers.push_back(null ? std::nullopt
                                      : std::optional<int64_t>(value));
        item->key += raw + QLatin1Char('|');
        continue;
      }
      QString text = attribute.kind == Vmap0Attribute::Kind::Decoded &&
                             !description.isEmpty()
                         ? description
                         : raw;
      if (raw.isEmpty() || raw == QLatin1String("UNK") ||
          text == QLatin1String("No entry present")) {
        text = QString();
      }
      item->key += text + QLatin1Char('|');
      item->texts.push_back(text);
    }
  }

  /** The pieces of one feature cut to the tiles, by tile. */
  using Pieces = std::map<uint64_t, std::pair<TileKey, std::vector<GeomPtr>>>;

  /**
   * Simplifies @p part for level of detail @p index; null (counted) when it
   * is too small for that level or nothing is left. Points are kept as
   * they are (a clone).
   */
  // The level of detail, then the dimension, as in writeLod().
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  GeomPtr simplified(const GEOSGeometry* part, std::size_t index,
                     int32_t dimension)
  // NOLINTEND(bugprone-easily-swappable-parameters)
  {
    GEOSContextHandle_t context = m_geos->context();
    const Vmap0Lod& lod = m_options->lods[index];
    if (dimension == 0 || lod.toleranceDeg <= 0.0) {
      return m_geos->wrap(GEOSGeom_clone_r(context, part));
    }
    const bool finest = index + 1 == m_options->lods.size();
    double size = 0.0;
    if (dimension == 2) {
      GEOSArea_r(context, part, &size);
    } else {
      GEOSLength_r(context, part, &size);
    }
    const double least = dimension == 2
                             ? 4.0 * lod.toleranceDeg * lod.toleranceDeg
                             : lod.toleranceDeg;
    GeomPtr result = m_geos->none();
    if (finest || size >= least) {
      result = m_geos->keep(m_geos->wrap(GEOSTopologyPreserveSimplify_r(
                                context, part, lod.toleranceDeg)),
                            dimension);
    }
    if (dimension == 2) {
      result = m_geos->valid(std::move(result), &m_stats->repaired);
    }
    if (result == nullptr) {
      m_stats->dropped[index] += 1;
    }
    return result;
  }

  bool writeLod(std::size_t index, const FeatureTableSpec& spec,
                int32_t dimension, QString* errorString)
  {
    GEOSContextHandle_t context = m_geos->context();
    for (const Item& item : m_items) {
      // Each part on its own (a dissolved feature may span a continent),
      // its pieces gathered per tile: one row per tile.
      Pieces pieces;
      const int32_t parts =
          GEOSGetNumGeometries_r(context, item.geometry.get());
      for (int32_t p = 0; p < parts; ++p) {
        const GeomPtr part =
            simplified(GEOSGetGeometryN_r(context, item.geometry.get(), p),
                       index, dimension);
        if (part == nullptr) {
          continue;
        }
        const Box box = m_geos->box(part.get());
        if (dimension == 0) {
          const TileKey key = TileKey::containing(
              spec.storageLevel, Geodetic{box.south, box.west, 0.0});
          std::pair<TileKey, std::vector<GeomPtr>>& tile = pieces[key.packed()];
          tile.first = key;
          tile.second.push_back(
              m_geos->wrap(GEOSGeom_clone_r(context, part.get())));
          continue;
        }
        for (int32_t x = 0; x < 2; ++x) {
          cut(spec, TileKey{0, x, 0}, part.get(), box, dimension, &pieces);
        }
      }
      if (!writePieces(spec, index, dimension, item, &pieces, errorString)) {
        return false;
      }
    }
    return true;
  }

  /** Writes one row per tile of @p pieces. */
  // The level of detail, then the dimension, as in writeLod().
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  bool writePieces(const FeatureTableSpec& spec, std::size_t index,
                   int32_t dimension, const Item& item, Pieces* pieces,
                   QString* errorString)
  // NOLINTEND(bugprone-easily-swappable-parameters)
  {
    GEOSContextHandle_t context = m_geos->context();
    for (std::pair<const uint64_t, std::pair<TileKey, std::vector<GeomPtr>>>&
             tile : *pieces) {
      std::vector<GEOSGeometry*> owned;
      for (GeomPtr& piece : tile.second.second) {
        m_geos->collect(piece.get(), dimension, &owned);
      }
      if (owned.empty()) {
        continue;
      }
      GeomPtr collection = m_geos->wrap(GEOSGeom_createCollection_r(
          context, multiType(dimension), owned.data(),
          static_cast<uint32_t>(owned.size())));
      // Parts simplified on their own may now touch or overlap.
      if (dimension == 2 && owned.size() > 1) {
        collection = m_geos->valid(std::move(collection), &m_stats->repaired);
        if (collection == nullptr) {
          continue;
        }
      }
      if (!put(spec, index, tile.second.first, collection.get(), item,
               errorString)) {
        return false;
      }
    }
    return true;
  }

  /** Cuts @p geometry down the quadtree from @p key into @p pieces. */
  void cut(const FeatureTableSpec& spec, TileKey key,
           const GEOSGeometry* geometry, const Box& box, int32_t dimension,
           Pieces* pieces)
  {
    const Box tile = boxOf(key);
    if (!box.overlaps(tile)) {
      return;
    }
    GeomPtr clipped = m_geos->none();
    Box clippedBox = box;
    if (!box.inside(tile)) {
      clipped = m_geos->clip(geometry, tile, dimension);
      if (clipped == nullptr) {
        return;
      }
      geometry = clipped.get();
      clippedBox = m_geos->box(geometry);
    }
    if (key.level == spec.storageLevel) {
      // Clipping keeps valid polygons valid nearly always; checked once,
      // on the tile's piece (checking at every level costs minutes).
      if (dimension == 2 && clipped != nullptr) {
        clipped = m_geos->valid(std::move(clipped), &m_stats->repaired);
        if (clipped == nullptr) {
          return;
        }
      }
      std::pair<TileKey, std::vector<GeomPtr>>& entry = (*pieces)[key.packed()];
      entry.first = key;
      entry.second.push_back(
          clipped != nullptr
              ? std::move(clipped)
              : m_geos->wrap(GEOSGeom_clone_r(m_geos->context(), geometry)));
      return;
    }
    for (int32_t i = 0; i < 4; ++i) {
      cut(spec, key.child(i), geometry, clippedBox, dimension, pieces);
    }
  }

  bool put(const FeatureTableSpec& spec, std::size_t index, TileKey key,
           const GEOSGeometry* geometry, const Item& item, QString* errorString)
  {
    m_stats->rows[index] += 1;
    m_stats->vertices[index] += m_geos->vertices(geometry);
    if (!m_writer->putFeature(spec.name, key, m_geos->toWkb(geometry),
                              item.integers, item.texts)) {
      return fail(errorString, m_writer->errorString());
    }
    return true;
  }

  const Geos* m_geos;
  const Vmap0Options* m_options;
  const Vmap0Class* m_class;
  GeoPackageFeatureWriter* m_writer;
  Vmap0ClassStats* m_stats;
  int64_t* m_nextId;
  vpf::FeatureType m_type = vpf::FeatureType::Point;
  bool m_typeKnown = false;
  std::vector<Item> m_items;
  std::vector<QString> m_problems;
};

/** The type of a class by VMAP0's names, which end in l, a or p. */
std::optional<vpf::FeatureType> typeFromName(const QString& name)
{
  if (name.endsWith(QLatin1Char('l'))) {
    return vpf::FeatureType::Line;
  }
  if (name.endsWith(QLatin1Char('a'))) {
    return vpf::FeatureType::Area;
  }
  if (name.endsWith(QLatin1Char('p'))) {
    return vpf::FeatureType::Point;
  }
  return std::nullopt;
}

/** The type of class @p cls in @p library, if it has it. */
std::optional<vpf::FeatureType> typeIn(const vpf::Library& library,
                                       const Vmap0Class& cls, bool* failed,
                                       QString* errorString)
{
  if (!hasCoverage(library, cls.coverage)) {
    return std::nullopt;
  }
  const std::optional<vpf::Coverage> coverage =
      vpf::Coverage::open(library, cls.coverage, errorString);
  if (!coverage) {
    *failed = true;
    return std::nullopt;
  }
  const vpf::FeatureClass* featureClass =
      coverage->featureClass(cls.featureClass);
  return featureClass != nullptr
             ? std::optional<vpf::FeatureType>(featureClass->type)
             : std::nullopt;
}

/**
 * The feature type of each class, from the first library that has it. A
 * class no library used has (a regional pack's) still gets its tables,
 * empty, so that the layers naming them stay readable: its type from its
 * name.
 */
bool classTypes(const std::vector<vpf::Library>& libraries,
                const std::vector<Vmap0Class>& classes,
                std::vector<vpf::FeatureType>* types, QString* errorString)
{
  types->clear();
  for (const Vmap0Class& cls : classes) {
    std::optional<vpf::FeatureType> type;
    for (const vpf::Library& library : libraries) {
      bool failed = false;
      type = typeIn(library, cls, &failed, errorString);
      if (failed) {
        return false;
      }
      if (type) {
        break;
      }
    }
    if (!type) {
      type = typeFromName(cls.featureClass);
    }
    if (!type.has_value()) {
      return fail(errorString,
                  QStringLiteral("No library has the feature class %1/%2")
                      .arg(cls.coverage, cls.featureClass));
    }
    types->push_back(type.value_or(vpf::FeatureType::Point));
  }
  return true;
}

/** Opens the wanted libraries of every database. */
bool openLibraries(const Vmap0Options& options,
                   const std::function<void(const QString&)>& progress,
                   std::vector<vpf::Library>* libraries, QString* errorString)
{
  for (const QString& path : options.databases) {
    const std::optional<vpf::Database> database =
        vpf::Database::open(path, errorString);
    if (!database) {
      return false;
    }
    for (const vpf::LibraryInfo& info : database->libraries) {
      // Every library but the world reference library, unless named.
      const bool wanted =
          options.libraries.empty()
              ? info.name != QLatin1String("rference")
              : std::find(options.libraries.begin(), options.libraries.end(),
                          info.name) != options.libraries.end();
      if (!wanted) {
        continue;
      }
      std::optional<vpf::Library> library =
          vpf::Library::open(*database, info.name, errorString);
      if (!library) {
        return false;
      }
      progress(QStringLiteral("VMAP0 library %1: %2 tiles")
                   .arg(library->name)
                   .arg(library->tilePaths.size()));
      libraries->push_back(std::move(*library));
    }
  }
  return !libraries->empty() ||
         fail(errorString, QStringLiteral("No VMAP0 library found"));
}

} // namespace

QString vmap0TableName(const QString& table, const Vmap0Lod& lod)
{
  return table + QLatin1Char('_') + QString::number(lod.storageLevel);
}

std::optional<Vmap0Attribute::Kind> vmap0AttributeKind(const QString& name)
{
  if (name.isEmpty() || name == QLatin1String("decoded")) {
    return Vmap0Attribute::Kind::Decoded;
  }
  if (name == QLatin1String("text")) {
    return Vmap0Attribute::Kind::Text;
  }
  if (name == QLatin1String("number")) {
    return Vmap0Attribute::Kind::Number;
  }
  return std::nullopt;
}

QJsonObject Vmap0ClassStats::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("table"), table);
  json.insert(QStringLiteral("sourceFeatures"),
              static_cast<double>(sourceFeatures));
  json.insert(QStringLiteral("filtered"), static_cast<double>(filtered));
  json.insert(QStringLiteral("skipped"), static_cast<double>(skipped));
  json.insert(QStringLiteral("degenerate"), static_cast<double>(degenerate));
  json.insert(QStringLiteral("joined"), static_cast<double>(joined));
  json.insert(QStringLiteral("features"), static_cast<double>(features));
  json.insert(QStringLiteral("repaired"), static_cast<double>(repaired));
  json.insert(QStringLiteral("sourceVertices"),
              static_cast<double>(sourceVertices));
  QJsonArray rowArray;
  QJsonArray vertexArray;
  QJsonArray droppedArray;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rowArray.append(static_cast<double>(rows[i]));
    vertexArray.append(static_cast<double>(vertices[i]));
    droppedArray.append(static_cast<double>(dropped[i]));
  }
  json.insert(QStringLiteral("rows"), rowArray);
  json.insert(QStringLiteral("vertices"), vertexArray);
  json.insert(QStringLiteral("dropped"), droppedArray);
  return json;
}

bool buildVmap0(const Vmap0Options& options, GeoPackageFeatureWriter* writer,
                std::vector<Vmap0ClassStats>* stats, QString* errorString)
{
  const std::function<void(const QString&)> progress =
      options.progress ? options.progress : [](const QString&) {};
  if (options.lods.empty() || options.classes.empty()) {
    return fail(errorString, QStringLiteral("No levels of detail or classes"));
  }
  std::vector<vpf::Library> libraries;
  if (!openLibraries(options, progress, &libraries, errorString)) {
    return false;
  }
  // Every table first: the writer takes features only after them.
  std::vector<vpf::FeatureType> types;
  if (!classTypes(libraries, options.classes, &types, errorString)) {
    return false;
  }
  for (std::size_t c = 0; c < options.classes.size(); ++c) {
    for (const FeatureTableSpec& spec :
         tableSpecs(options.classes[c], types[c], options.lods)) {
      if (!spec.name.isEmpty() && !writer->addTable(spec)) {
        return fail(errorString, writer->errorString());
      }
    }
  }
  const Geos geos;
  int64_t nextId = 1;
  for (const Vmap0Class& cls : options.classes) {
    progress(QStringLiteral("VMAP0 %1/%2 → %3")
                 .arg(cls.coverage, cls.featureClass, cls.table));
    Vmap0ClassStats classStats;
    classStats.table = cls.table;
    ClassBuild build(geos, options, cls, writer, &classStats, &nextId);
    if (!build.readAll(libraries, errorString) || !build.finish(errorString)) {
      return false;
    }
    for (const QString& problem : build.problems()) {
      progress(QStringLiteral("  left out: ") + problem);
    }
    progress(QStringLiteral("  %1 read, %2 filtered, %3 skipped, %4 "
                            "degenerate, %5 joined, %6 written")
                 .arg(classStats.sourceFeatures)
                 .arg(classStats.filtered)
                 .arg(classStats.skipped)
                 .arg(classStats.degenerate)
                 .arg(classStats.joined)
                 .arg(classStats.features));
    stats->push_back(classStats);
  }
  return true;
}

} // namespace ts::packager
