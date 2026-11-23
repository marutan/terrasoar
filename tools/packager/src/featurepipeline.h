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
#ifndef TS_PACKAGER_FEATUREPIPELINE_H
#define TS_PACKAGER_FEATUREPIPELINE_H

/*
 * The part of building a pack's vector dataset that doesn't depend on the
 * source format: features as GEOS geometries with their attributes are
 * dissolved by attributes, simplified for each level of detail, cut down
 * the quadtree and written to the GeoPackage's tables. The VMAP0 reader
 * (vmap0builder) and the OGR reader (vectorbuilder) feed it.
 */

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <geos_c.h>

#include "ts/deps/geoscontext.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts::packager
{

/** One level of detail: tables cut to one level and simplified. */
struct FeatureLod {
  /** Quadtree level the features are cut to. */
  int32_t storageLevel = 0;
  /** Shallowest display level served. */
  int32_t minLevel = 0;
  /** Deepest display level served, -1 for every deeper level. */
  int32_t maxLevel = -1;
  /**
   * Lines and polygons are simplified within this, degrees (0 keeps
   * every vertex). Below the finest level of detail, polygons smaller
   * than a square of twice this side and lines shorter than it are left
   * out.
   */
  double toleranceDeg = 0.0;
};

/** What became of one feature class. */
struct FeatureClassStats {
  /** Its table name before the storage level. */
  QString table;
  /** Feature rows read from the libraries. */
  int64_t sourceFeatures = 0;
  /** Of those, left out by the filters. */
  int64_t filtered = 0;
  /** Of those, without usable geometry (see vpf::ReadStats). */
  int64_t skipped = 0;
  /**
   * Of those, read but degenerate: faces whose rings enclose nothing or
   * edges of one point (a known kind of VMAP0 error), left out and
   * reported.
   */
  int64_t degenerate = 0;
  /** Pieces joined into another across a tile edge. */
  int64_t joined = 0;
  /**
   * Features after joining: sourceFeatures − filtered − skipped −
   * degenerate − joined.
   */
  int64_t features = 0;
  /** Polygons GEOS found invalid and repaired. */
  int64_t repaired = 0;
  /** Vertices read. */
  int64_t sourceVertices = 0;
  /** Rows written, per level of detail (0 where the class isn't shown). */
  std::vector<int64_t> rows;
  /** Vertices written, per level of detail. */
  std::vector<int64_t> vertices;
  /** Features left out as too small, per level of detail. */
  std::vector<int64_t> dropped;

  /** Returns the statistics as JSON. */
  QJsonObject toJson() const;
};

/** One attribute column a class writes. */
struct FeatureColumn {
  /** The GeoPackage column. */
  QString name;
  /** True for an integer column, false for text. */
  bool number = false;
};

/** What one class writes: its tables and their columns. */
struct FeatureClassShape {
  /** The table name before the storage level, e.g. "roads". */
  QString table;
  /** Shallowest display level it is shown at. */
  int32_t minLevel = 0;
  /** Deepest display level it is shown at, -1 for all. */
  int32_t maxLevel = -1;
  /** Its attribute columns, after feature_id. */
  std::vector<FeatureColumn> columns;
  /**
   * The levels of detail (indices) it writes; empty for all. Classes of one
   * table from different sources, one per level of detail (WDBII's
   * resolutions), each name theirs.
   */
  std::vector<int32_t> lods;
};

/**
 * Returns the table name of class @p table at level of detail @p lod:
 * `<table>_<storage level>`.
 */
QString featureTableName(const QString& table, const FeatureLod& lod);

/**
 * The tables class @p shape of features of @p dimension (0 points, 1 lines,
 * 2 areas) writes, one per level of detail (an empty name where the class
 * isn't shown at its levels).
 */
std::vector<FeatureTableSpec>
featureTableSpecs(const FeatureClassShape& shape, int32_t dimension,
                  const std::vector<FeatureLod>& lods);

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

inline Box boxOf(TileKey key)
{
  const GeoRect rect = key.bounds();
  return Box{rect.westDeg, rect.southDeg, rect.eastDeg, rect.northDeg};
}

/** The GEOS multi-geometry type for features of @p dimension. */
inline int32_t multiType(int32_t dimension)
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
inline int32_t dimensionOfGeosType(int32_t type)
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

  template <typename Point>
  GEOSCoordSequence* sequence(const std::vector<Point>& points) const
  {
    GEOSCoordSequence* sequence = GEOSCoordSeq_create_r(
        m_context, static_cast<uint32_t>(points.size()), 2);
    for (std::size_t i = 0; i < points.size(); ++i) {
      GEOSCoordSeq_setXY_r(m_context, sequence, static_cast<uint32_t>(i),
                           points[i].x, points[i].y);
    }
    return sequence;
  }

  template <typename Point>
  GeomPtr point(const Point& point) const
  {
    return wrap(GEOSGeom_createPointFromXY_r(m_context, point.x, point.y));
  }

  template <typename Point>
  GeomPtr line(const std::vector<std::vector<Point>>& parts) const
  {
    std::vector<GEOSGeometry*> lines;
    for (const std::vector<Point>& part : parts) {
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
  template <typename Point>
  GeomPtr polygon(const std::vector<std::vector<Point>>& rings,
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

  /** The geometry of WKB @p wkb (as OGR gives it); null if unreadable. */
  GeomPtr fromWkb(const QByteArray& wkb) const
  {
    GEOSWKBReader* reader = GEOSWKBReader_create_r(m_context);
    GEOSGeometry* geometry = GEOSWKBReader_read_r(
        m_context, reader, reinterpret_cast<const unsigned char*>(wkb.data()),
        static_cast<std::size_t>(wkb.size()));
    GEOSWKBReader_destroy_r(m_context, reader);
    return wrap(geometry);
  }

private:
  GeosContext m_owner;
  GEOSContextHandle_t m_context = nullptr;
  GEOSWKBWriter* m_writer = nullptr;
};

/** One feature on its way into the pack: its geometry and attributes. */
struct FeatureItem {
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
int64_t dissolveFeatures(const Geos& geos, int32_t dimension,
                         std::vector<FeatureItem>* items, int64_t* repaired);

/**
 * Writes one class: its features dissolved (or not), then simplified and
 * cut for each level of detail serving it, into its tables (which must
 * have been added to the writer).
 */
class FeatureClassWriter
{
public:
  /** Writes into @p writer; ids from @p nextId, which it advances. */
  FeatureClassWriter(const Geos& geos, const std::vector<FeatureLod>& lods,
                     FeatureClassShape shape, int32_t dimension,
                     GeoPackageFeatureWriter* writer, FeatureClassStats* stats,
                     int64_t* nextId);

  /** Adds a feature. Its attributes are in the shape's column order. */
  void add(FeatureItem item);

  /**
   * Dissolves the features by their attributes if @p dissolve, then writes
   * every level. @p progress may be empty.
   */
  bool finish(bool dissolve,
              const std::function<void(const QString&)>& progress,
              QString* errorString);

private:
  /** The pieces of one feature cut to the tiles, by tile. */
  using Pieces = std::map<uint64_t, std::pair<TileKey, std::vector<GeomPtr>>>;

  GeomPtr simplified(const GEOSGeometry* part, std::size_t index);
  bool writeLod(std::size_t index, const FeatureTableSpec& spec,
                QString* errorString);
  bool writePieces(const FeatureTableSpec& spec, std::size_t index,
                   const FeatureItem& item, Pieces* pieces,
                   QString* errorString);
  void cut(const FeatureTableSpec& spec, TileKey key,
           const GEOSGeometry* geometry, const Box& box, Pieces* pieces);
  bool put(const FeatureTableSpec& spec, std::size_t index, TileKey key,
           const GEOSGeometry* geometry, const FeatureItem& item,
           QString* errorString);

  const Geos* m_geos;
  const std::vector<FeatureLod>* m_lods;
  FeatureClassShape m_shape;
  int32_t m_dimension = 0;
  GeoPackageFeatureWriter* m_writer;
  FeatureClassStats* m_stats;
  int64_t* m_nextId;
  std::vector<FeatureItem> m_items;
};

} // namespace ts::packager

#endif // TS_PACKAGER_FEATUREPIPELINE_H
