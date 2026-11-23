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

/** True for VPF's null values of short and long integers. */
bool isNullInteger(int64_t value)
{
  return value == -32768 || value == -32767 || value == -2147483647 ||
         value == -2147483647 - 1;
}

/** The pipeline's shape of class @p cls. */
FeatureClassShape shapeOf(const Vmap0Class& cls)
{
  FeatureClassShape shape;
  shape.table = cls.table;
  shape.minLevel = cls.minLevel;
  shape.maxLevel = cls.maxLevel;
  shape.columns.reserve(cls.attributes.size());
  for (const Vmap0Attribute& attribute : cls.attributes) {
    shape.columns.push_back(FeatureColumn{
        attribute.name, attribute.kind == Vmap0Attribute::Kind::Number});
  }
  return shape;
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
      FeatureItem item;
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
    FeatureClassWriter classWriter(*m_geos, m_options->lods, shapeOf(*m_class),
                                   dimensionOf(m_type), m_writer, m_stats,
                                   m_nextId);
    for (FeatureItem& item : m_items) {
      classWriter.add(std::move(item));
    }
    m_items.clear();
    return classWriter.finish(true, m_options->progress, errorString);
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
              FeatureItem* item) const
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

  const Geos* m_geos;
  const Vmap0Options* m_options;
  const Vmap0Class* m_class;
  GeoPackageFeatureWriter* m_writer;
  Vmap0ClassStats* m_stats;
  int64_t* m_nextId;
  vpf::FeatureType m_type = vpf::FeatureType::Point;
  bool m_typeKnown = false;
  std::vector<FeatureItem> m_items;
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
  return featureTableName(table, lod);
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
         featureTableSpecs(shapeOf(options.classes[c]), dimensionOf(types[c]),
                           options.lods)) {
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
