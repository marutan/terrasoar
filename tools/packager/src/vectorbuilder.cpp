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

#include "vectorbuilder.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QString>

#include <gdal.h>
#include <geos_c.h>
#include <ogr_api.h>

#include "ts/providers/geopackagefeatureprovider.h"

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

/** Closes a GDAL dataset. */
struct DatasetCloser {
  void operator()(void* dataset) const
  {
    if (dataset != nullptr) {
      GDALClose(static_cast<GDALDatasetH>(dataset));
    }
  }
};
using DatasetPtr = std::unique_ptr<void, DatasetCloser>;

/** Destroys an OGR feature. */
struct FeatureDestroyer {
  void operator()(void* feature) const
  {
    if (feature != nullptr) {
      OGR_F_Destroy(static_cast<OGRFeatureH>(feature));
    }
  }
};
using FeaturePtr = std::unique_ptr<void, FeatureDestroyer>;

/** The pipeline's shape of class @p cls. */
FeatureClassShape shapeOf(const VectorClass& cls)
{
  FeatureClassShape shape;
  shape.table = cls.table;
  shape.minLevel = cls.minLevel;
  shape.maxLevel = cls.maxLevel;
  shape.lods = cls.lods;
  shape.columns.reserve(cls.attributes.size());
  for (const VectorAttribute& attribute : cls.attributes) {
    shape.columns.push_back(FeatureColumn{attribute.name, attribute.number});
  }
  return shape;
}

/** Opens @p cls's dataset with its open options. */
DatasetPtr openDataset(const VectorClass& cls, QString* errorString)
{
  std::vector<QByteArray> options;
  options.reserve(static_cast<std::size_t>(cls.openOptions.size()));
  for (const QString& option : cls.openOptions) {
    options.push_back(option.toUtf8());
  }
  std::vector<const char*> pointers;
  pointers.reserve(options.size() + 1);
  for (const QByteArray& option : options) {
    pointers.push_back(option.constData());
  }
  pointers.push_back(nullptr);
  CPLErrorReset();
  DatasetPtr dataset(GDALOpenEx(cls.path.toUtf8().constData(),
                                GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                                pointers.data(), nullptr));
  if (dataset == nullptr) {
    fail(errorString,
         QStringLiteral("Cannot open %1: %2")
             .arg(cls.path, QString::fromUtf8(CPLGetLastErrorMsg())));
  }
  return dataset;
}

/** One class's reading from its OGR layer. */
class ClassReader
{
public:
  ClassReader(const Geos& geos, const VectorClass& cls,
              FeatureClassStats* stats)
      : m_geos(&geos), m_class(&cls), m_stats(stats)
  {
  }

  /** Reads the features passing the filter into @p classWriter. */
  bool read(FeatureClassWriter* classWriter, QString* errorString)
  {
    const DatasetPtr dataset = openDataset(*m_class, errorString);
    if (dataset == nullptr) {
      return false;
    }
    auto* const handle = static_cast<GDALDatasetH>(dataset.get());
    OGRLayerH layer = m_class->layer.isEmpty()
                          ? GDALDatasetGetLayer(handle, 0)
                          : GDALDatasetGetLayerByName(
                                handle, m_class->layer.toUtf8().constData());
    if (layer == nullptr) {
      return fail(errorString, QStringLiteral("%1 has no layer %2")
                                   .arg(m_class->path, m_class->layer));
    }
    if (!m_class->where.isEmpty() &&
        OGR_L_SetAttributeFilter(layer, m_class->where.toUtf8().constData()) !=
            OGRERR_NONE) {
      return fail(errorString,
                  QStringLiteral("%1: bad filter \"%2\": %3")
                      .arg(m_class->path, m_class->where,
                           QString::fromUtf8(CPLGetLastErrorMsg())));
    }
    if (!findFields(OGR_L_GetLayerDefn(layer), errorString)) {
      return false;
    }
    OGR_L_ResetReading(layer);
    for (FeaturePtr feature(OGR_L_GetNextFeature(layer)); feature != nullptr;
         feature.reset(OGR_L_GetNextFeature(layer))) {
      readFeature(static_cast<OGRFeatureH>(feature.get()), classWriter);
    }
    return true;
  }

private:
  /** Finds the fields of the attributes and line ends. */
  bool findFields(OGRFeatureDefnH definition, QString* errorString)
  {
    const auto find = [definition](const QString& name) {
      return OGR_FD_GetFieldIndex(definition, name.toUtf8().constData());
    };
    for (const VectorAttribute& attribute : m_class->attributes) {
      const int32_t index =
          attribute.column.isEmpty() ? -1 : find(attribute.column);
      if (!attribute.column.isEmpty() && index < 0) {
        return fail(errorString, QStringLiteral("%1 has no field %2")
                                     .arg(m_class->path, attribute.column));
      }
      m_fields.push_back(index);
    }
    for (const QString& column : m_class->lineColumns) {
      const int32_t index = find(column);
      if (index < 0) {
        return fail(
            errorString,
            QStringLiteral("%1 has no field %2").arg(m_class->path, column));
      }
      m_lineFields.push_back(index);
    }
    return true;
  }

  /** The geometry of @p feature, or null (counted) if there is none. */
  GeomPtr geometryOf(OGRFeatureH feature)
  {
    if (!m_lineFields.empty()) {
      std::vector<double> values;
      for (const int32_t field : m_lineFields) {
        const QString text =
            QString::fromUtf8(OGR_F_GetFieldAsString(feature, field)).trimmed();
        bool ok = false;
        const double value = text.toDouble(&ok);
        if (!ok) {
          m_stats->filtered += 1;
          return m_geos->none();
        }
        values.push_back(value);
      }
      struct Point {
        double x;
        double y;
      };
      const std::vector<std::vector<Point>> parts = {
          {Point{values[0], values[1]}, Point{values[2], values[3]}}};
      m_stats->sourceVertices += 2;
      return m_geos->line(parts);
    }
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    if (geometry == nullptr || OGR_G_IsEmpty(geometry) != 0) {
      m_stats->filtered += 1; // No geometry: a CSV row without position.
      return m_geos->none();
    }
    QByteArray wkb(OGR_G_WkbSize(geometry), Qt::Uninitialized);
    OGR_G_ExportToWkb(geometry, wkbNDR,
                      reinterpret_cast<unsigned char*>(wkb.data()));
    GeomPtr result = m_geos->fromWkb(wkb);
    if (result == nullptr) {
      m_stats->skipped += 1;
      return result;
    }
    m_stats->sourceVertices += m_geos->vertices(result.get());
    result = m_geos->keep(std::move(result), m_class->dimension);
    if (result != nullptr && m_class->dimension == 2) {
      result = m_geos->valid(std::move(result), &m_stats->repaired);
    }
    if (result == nullptr) {
      m_stats->skipped += 1; // Nothing of the class's kind.
    }
    return result;
  }

  void readFeature(OGRFeatureH feature, FeatureClassWriter* classWriter)
  {
    m_stats->sourceFeatures += 1;
    FeatureItem item;
    item.geometry = geometryOf(feature);
    if (item.geometry == nullptr) {
      return;
    }
    for (std::size_t a = 0; a < m_class->attributes.size(); ++a) {
      const VectorAttribute& attribute = m_class->attributes[a];
      const int32_t field = m_fields[a];
      QString text = attribute.value; // a constant
      if (field >= 0) {
        text = OGR_F_IsFieldSetAndNotNull(feature, field) != 0
                   ? QString::fromUtf8(OGR_F_GetFieldAsString(feature, field))
                         .trimmed()
                   : QString();
      }
      item.key += text + QLatin1Char('|');
      if (attribute.number) {
        bool ok = false;
        const double value = text.toDouble(&ok);
        item.integers.push_back(
            ok ? std::optional<int64_t>(
                     static_cast<int64_t>(std::llround(value)))
               : std::nullopt);
      } else {
        item.texts.push_back(text);
      }
    }
    classWriter->add(std::move(item));
  }

  const Geos* m_geos;
  const VectorClass* m_class;
  FeatureClassStats* m_stats;
  std::vector<int32_t> m_fields;
  std::vector<int32_t> m_lineFields;
};

/** True if two classes' specs of a table agree. */
bool sameTable(const FeatureTableSpec& a, const FeatureTableSpec& b)
{
  return a.geometry == b.geometry && a.storageLevel == b.storageLevel &&
         a.minLevel == b.minLevel && a.maxLevel == b.maxLevel &&
         a.attributes == b.attributes && a.textAttributes == b.textAttributes;
}

/**
 * Adds every class's tables to @p writer, which takes features only after
 * them. Classes may share a table (WDBII's river levels, each a file) if
 * its columns are the same.
 */
bool addTables(const VectorOptions& options, GeoPackageFeatureWriter* writer,
               QString* errorString)
{
  std::map<QString, FeatureTableSpec> tables;
  for (const VectorClass& cls : options.classes) {
    for (const FeatureTableSpec& spec :
         featureTableSpecs(shapeOf(cls), cls.dimension, options.lods)) {
      if (spec.name.isEmpty()) {
        continue;
      }
      const std::map<QString, FeatureTableSpec>::const_iterator known =
          tables.find(spec.name);
      if (known != tables.end()) {
        if (!sameTable(known->second, spec)) {
          return fail(errorString,
                      QStringLiteral("Classes write the table %1 differently")
                          .arg(spec.name));
        }
        continue;
      }
      tables.emplace(spec.name, spec);
      if (!writer->addTable(spec)) {
        return fail(errorString, writer->errorString());
      }
    }
  }
  return true;
}

} // namespace

int32_t vectorDimension(const QString& name)
{
  if (name == QLatin1String("points")) {
    return 0;
  }
  if (name == QLatin1String("lines")) {
    return 1;
  }
  if (name == QLatin1String("areas")) {
    return 2;
  }
  return -1;
}

bool buildVector(const VectorOptions& options, GeoPackageFeatureWriter* writer,
                 std::vector<FeatureClassStats>* stats, QString* errorString)
{
  if (options.lods.empty() || options.classes.empty()) {
    return fail(errorString, QStringLiteral("No levels of detail or classes"));
  }
  if (!addTables(options, writer, errorString)) {
    return false;
  }
  const Geos geos;
  int64_t nextId = 1;
  for (const VectorClass& cls : options.classes) {
    if (options.progress) {
      options.progress(QStringLiteral("%1 → %2").arg(cls.path, cls.table));
    }
    FeatureClassStats classStats;
    classStats.table = cls.table;
    FeatureClassWriter classWriter(geos, options.lods, shapeOf(cls),
                                   cls.dimension, writer, &classStats, &nextId);
    ClassReader reader(geos, cls, &classStats);
    if (!reader.read(&classWriter, errorString) ||
        !classWriter.finish(cls.dissolve, options.progress, errorString)) {
      return false;
    }
    if (options.progress) {
      options.progress(QStringLiteral("  %1 read, %2 without geometry, %3 "
                                      "skipped, %4 joined, %5 written")
                           .arg(classStats.sourceFeatures)
                           .arg(classStats.filtered)
                           .arg(classStats.skipped)
                           .arg(classStats.joined)
                           .arg(classStats.features));
    }
    stats->push_back(classStats);
  }
  return true;
}

} // namespace ts::packager
