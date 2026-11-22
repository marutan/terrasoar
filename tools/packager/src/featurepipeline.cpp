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

#include "featurepipeline.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include <QJsonArray>
#include <QString>

#include <geos_c.h>

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

/** The table geometry of features of @p dimension. */
FeatureTableSpec::Geometry geometryOfDimension(int32_t dimension)
{
  switch (dimension) {
  case 2:
    return FeatureTableSpec::Geometry::Polygons;
  case 1:
    return FeatureTableSpec::Geometry::Lines;
  default:
    return FeatureTableSpec::Geometry::Points;
  }
}

} // namespace

QString featureTableName(const QString& table, const FeatureLod& lod)
{
  return table + QLatin1Char('_') + QString::number(lod.storageLevel);
}

std::vector<FeatureTableSpec>
featureTableSpecs(const FeatureClassShape& shape, int32_t dimension,
                  const std::vector<FeatureLod>& lods)
{
  std::vector<FeatureTableSpec> specs;
  specs.reserve(lods.size());
  for (const FeatureLod& lod : lods) {
    FeatureTableSpec spec;
    const int32_t minLevel = std::max(lod.minLevel, shape.minLevel);
    int32_t maxLevel = lod.maxLevel;
    if (shape.maxLevel >= 0) {
      maxLevel =
          maxLevel < 0 ? shape.maxLevel : std::min(maxLevel, shape.maxLevel);
    }
    if (maxLevel >= 0 && maxLevel < minLevel) {
      specs.push_back(spec); // Not shown at these levels.
      continue;
    }
    spec.name = featureTableName(shape.table, lod);
    spec.geometry = geometryOfDimension(dimension);
    spec.storageLevel = lod.storageLevel;
    spec.minLevel = minLevel;
    spec.maxLevel = maxLevel;
    spec.attributes.push_back(QStringLiteral("feature_id"));
    for (const FeatureColumn& column : shape.columns) {
      if (column.number) {
        spec.attributes.push_back(column.name);
      } else {
        spec.textAttributes.push_back(column.name);
      }
    }
    specs.push_back(spec);
  }
  return specs;
}

int64_t dissolveFeatures(const Geos& geos, int32_t dimension,
                         std::vector<FeatureItem>* items, int64_t* repaired)
{
  GEOSContextHandle_t context = geos.context();
  std::map<QString, std::vector<std::size_t>> members;
  for (std::size_t i = 0; i < items->size(); ++i) {
    members[(*items)[i].key].push_back(i);
  }
  int64_t joined = 0;
  std::vector<FeatureItem> dissolved;
  dissolved.reserve(members.size());
  for (std::pair<const QString, std::vector<std::size_t>>& group : members) {
    FeatureItem& first = (*items)[group.second.front()];
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

QJsonObject FeatureClassStats::toJson() const
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

FeatureClassWriter::FeatureClassWriter(
    const Geos& geos, const std::vector<FeatureLod>& lods,
    FeatureClassShape shape, int32_t dimension, GeoPackageFeatureWriter* writer,
    FeatureClassStats* stats, int64_t* nextId)
    : m_geos(&geos), m_lods(&lods), m_shape(std::move(shape)),
      m_dimension(dimension), m_writer(writer), m_stats(stats), m_nextId(nextId)
{
}

void FeatureClassWriter::add(FeatureItem item)
{
  m_items.push_back(std::move(item));
}

bool FeatureClassWriter::finish(
    bool dissolve, const std::function<void(const QString&)>& progress,
    QString* errorString)
{
  if (dissolve) {
    m_stats->joined =
        dissolveFeatures(*m_geos, m_dimension, &m_items, &m_stats->repaired);
  }
  m_stats->features = static_cast<int64_t>(m_items.size());
  for (FeatureItem& item : m_items) {
    item.integers.insert(item.integers.begin(),
                         std::optional<int64_t>(*m_nextId));
    *m_nextId += 1;
  }
  const std::vector<FeatureTableSpec> specs =
      featureTableSpecs(m_shape, m_dimension, *m_lods);
  for (std::size_t l = 0; l < m_lods->size(); ++l) {
    m_stats->rows.push_back(0);
    m_stats->vertices.push_back(0);
    m_stats->dropped.push_back(0);
    if (specs[l].name.isEmpty()) {
      continue;
    }
    if (progress) {
      progress(QStringLiteral("  %1: %2 features")
                   .arg(specs[l].name)
                   .arg(m_items.size()));
    }
    if (!writeLod(l, specs[l], errorString)) {
      return false;
    }
  }
  return true;
}

/**
 * Simplifies @p part for level of detail @p index; null (counted) when it
 * is too small for that level or nothing is left. Points are kept as
 * they are (a clone).
 */
GeomPtr FeatureClassWriter::simplified(const GEOSGeometry* part,
                                       std::size_t index)
{
  GEOSContextHandle_t context = m_geos->context();
  const FeatureLod& lod = (*m_lods)[index];
  if (m_dimension == 0 || lod.toleranceDeg <= 0.0) {
    return m_geos->wrap(GEOSGeom_clone_r(context, part));
  }
  const bool finest = index + 1 == (*m_lods).size();
  double size = 0.0;
  if (m_dimension == 2) {
    GEOSArea_r(context, part, &size);
  } else {
    GEOSLength_r(context, part, &size);
  }
  const double least = m_dimension == 2
                           ? 4.0 * lod.toleranceDeg * lod.toleranceDeg
                           : lod.toleranceDeg;
  GeomPtr result = m_geos->none();
  if (finest || size >= least) {
    result = m_geos->keep(m_geos->wrap(GEOSTopologyPreserveSimplify_r(
                              context, part, lod.toleranceDeg)),
                          m_dimension);
  }
  if (m_dimension == 2) {
    result = m_geos->valid(std::move(result), &m_stats->repaired);
  }
  if (result == nullptr) {
    m_stats->dropped[index] += 1;
  }
  return result;
}

bool FeatureClassWriter::writeLod(std::size_t index,
                                  const FeatureTableSpec& spec,
                                  QString* errorString)
{
  GEOSContextHandle_t context = m_geos->context();
  for (const FeatureItem& item : m_items) {
    // Each part on its own (a dissolved feature may span a continent),
    // its pieces gathered per tile: one row per tile.
    Pieces pieces;
    const int32_t parts = GEOSGetNumGeometries_r(context, item.geometry.get());
    for (int32_t p = 0; p < parts; ++p) {
      const GeomPtr part = simplified(
          GEOSGetGeometryN_r(context, item.geometry.get(), p), index);
      if (part == nullptr) {
        continue;
      }
      const Box box = m_geos->box(part.get());
      if (m_dimension == 0) {
        const TileKey key = TileKey::containing(
            spec.storageLevel, Geodetic{box.south, box.west, 0.0});
        std::pair<TileKey, std::vector<GeomPtr>>& tile = pieces[key.packed()];
        tile.first = key;
        tile.second.push_back(
            m_geos->wrap(GEOSGeom_clone_r(context, part.get())));
        continue;
      }
      for (int32_t x = 0; x < 2; ++x) {
        cut(spec, TileKey{0, x, 0}, part.get(), box, &pieces);
      }
    }
    if (!writePieces(spec, index, item, &pieces, errorString)) {
      return false;
    }
  }
  return true;
}

/** Writes one row per tile of @p pieces. */
bool FeatureClassWriter::writePieces(const FeatureTableSpec& spec,
                                     std::size_t index, const FeatureItem& item,
                                     Pieces* pieces, QString* errorString)
{
  GEOSContextHandle_t context = m_geos->context();
  for (std::pair<const uint64_t, std::pair<TileKey, std::vector<GeomPtr>>>&
           tile : *pieces) {
    std::vector<GEOSGeometry*> owned;
    for (GeomPtr& piece : tile.second.second) {
      m_geos->collect(piece.get(), m_dimension, &owned);
    }
    if (owned.empty()) {
      continue;
    }
    GeomPtr collection = m_geos->wrap(GEOSGeom_createCollection_r(
        context, multiType(m_dimension), owned.data(),
        static_cast<uint32_t>(owned.size())));
    // Parts simplified on their own may now touch or overlap.
    if (m_dimension == 2 && owned.size() > 1) {
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
void FeatureClassWriter::cut(const FeatureTableSpec& spec, TileKey key,
                             const GEOSGeometry* geometry, const Box& box,
                             Pieces* pieces)
{
  const Box tile = boxOf(key);
  if (!box.overlaps(tile)) {
    return;
  }
  GeomPtr clipped = m_geos->none();
  Box clippedBox = box;
  if (!box.inside(tile)) {
    clipped = m_geos->clip(geometry, tile, m_dimension);
    if (clipped == nullptr) {
      return;
    }
    geometry = clipped.get();
    clippedBox = m_geos->box(geometry);
  }
  if (key.level == spec.storageLevel) {
    // Clipping keeps valid polygons valid nearly always; checked once,
    // on the tile's piece (checking at every level costs minutes).
    if (m_dimension == 2 && clipped != nullptr) {
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
    cut(spec, key.child(i), geometry, clippedBox, pieces);
  }
}

bool FeatureClassWriter::put(const FeatureTableSpec& spec, std::size_t index,
                             TileKey key, const GEOSGeometry* geometry,
                             const FeatureItem& item, QString* errorString)
{
  m_stats->rows[index] += 1;
  m_stats->vertices[index] += m_geos->vertices(geometry);
  if (!m_writer->putFeature(spec.name, key, m_geos->toWkb(geometry),
                            item.integers, item.texts)) {
    return fail(errorString, m_writer->errorString());
  }
  return true;
}

} // namespace ts::packager
