// SPDX-License-Identifier: GPL-2.0-or-later
#include "shorelinebuilder.h"

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <utility>

#include <QByteArray>

#include <GeographicLib/PolygonArea.hpp>
#include <GeographicLib/Rhumb.hpp>
#define GEOS_USE_ONLY_R_API
#include <cpl_vsi.h>
#include <gdal.h>
#include <geos_c.h>
#include <ogr_api.h>

#include "ts/deps/gdaldataset.h"
#include "ts/deps/geoscontext.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts::packager
{

namespace
{

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

/** A polygon with the GSHHG level it came from. */
struct Piece {
  GeomPtr geometry;
  int level = 0;
  Box box;
};

/** GEOS work for one thread: context, WKB reader and writer. */
class Geos
{
public:
  Geos()
      : m_context(static_cast<GEOSContextHandle_t>(m_owner.get())),
        m_reader(GEOSWKBReader_create_r(m_context)),
        m_writer(GEOSWKBWriter_create_r(m_context))
  {
    GEOSWKBWriter_setOutputDimension_r(m_context, m_writer, 2);
    GEOSWKBWriter_setByteOrder_r(m_context, m_writer, GEOS_WKB_NDR);
  }
  ~Geos()
  {
    GEOSWKBReader_destroy_r(m_context, m_reader);
    GEOSWKBWriter_destroy_r(m_context, m_writer);
  }
  Geos(const Geos&) = delete;
  Geos& operator=(const Geos&) = delete;
  Geos(Geos&&) = delete;
  Geos& operator=(Geos&&) = delete;

  GEOSContextHandle_t context() const { return m_context; }

  GeomPtr wrap(GEOSGeometry* geometry) const
  {
    return GeomPtr(geometry, GeomDeleter{m_context});
  }

  GeomPtr fromWkb(const std::vector<unsigned char>& wkb) const
  {
    return wrap(
        GEOSWKBReader_read_r(m_context, m_reader, wkb.data(), wkb.size()));
  }

  QByteArray toWkb(const GEOSGeometry* geometry) const
  {
    std::size_t size = 0;
    unsigned char* data =
        GEOSWKBWriter_write_r(m_context, m_writer, geometry, &size);
    if (data == nullptr) {
      return QByteArray();
    }
    QByteArray bytes(reinterpret_cast<const char*>(data),
                     static_cast<int>(size));
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

  /** Appends the polygons of @p geometry as clones. */
  void collectPolygons(const GEOSGeometry* geometry,
                       std::vector<GEOSGeometry*>* parts) const
  {
    const int type = GEOSGeomTypeId_r(m_context, geometry);
    if (type == GEOS_POLYGON) {
      if (!isEmpty(geometry)) {
        parts->push_back(GEOSGeom_clone_r(m_context, geometry));
      }
      return;
    }
    if (type == GEOS_MULTIPOLYGON || type == GEOS_GEOMETRYCOLLECTION) {
      const int count = GEOSGetNumGeometries_r(m_context, geometry);
      for (int i = 0; i < count; ++i) {
        collectPolygons(GEOSGetGeometryN_r(m_context, geometry, i), parts);
      }
    }
  }

  /** Builds a multipolygon, taking the parts; null if there are none. */
  GeomPtr multiPolygon(std::vector<GEOSGeometry*>* parts) const
  {
    if (parts->empty()) {
      return GeomPtr(nullptr, GeomDeleter{m_context});
    }
    GeomPtr result = wrap(
        GEOSGeom_createCollection_r(m_context, GEOS_MULTIPOLYGON, parts->data(),
                                    static_cast<unsigned int>(parts->size())));
    parts->clear();
    return result;
  }

  /** Returns only the polygonal part of @p geometry, or null if none. */
  GeomPtr keep(GeomPtr geometry) const
  {
    if (geometry == nullptr || isEmpty(geometry.get())) {
      return GeomPtr(nullptr, GeomDeleter{m_context});
    }
    const int type = GEOSGeomTypeId_r(m_context, geometry.get());
    if (type == GEOS_POLYGON || type == GEOS_MULTIPOLYGON) {
      return geometry;
    }
    std::vector<GEOSGeometry*> parts;
    collectPolygons(geometry.get(), &parts);
    return multiPolygon(&parts);
  }

  /** Clips the piece to @p box, keeping polygons; null if nothing is left. */
  GeomPtr clip(const Piece& piece, const Box& box) const
  {
    if (piece.box.inside(box)) {
      return wrap(GEOSGeom_clone_r(m_context, piece.geometry.get()));
    }
    return keep(wrap(GEOSClipByRect_r(m_context, piece.geometry.get(), box.west,
                                      box.south, box.east, box.north)));
  }

  /** Union of polygons, or null if there are none. */
  GeomPtr unionOf(const std::vector<const GEOSGeometry*>& geometries) const
  {
    std::vector<GEOSGeometry*> parts;
    for (const GEOSGeometry* geometry : geometries) {
      collectPolygons(geometry, &parts);
    }
    if (parts.empty()) {
      return GeomPtr(nullptr, GeomDeleter{m_context});
    }
    const GeomPtr collection = wrap(GEOSGeom_createCollection_r(
        m_context, GEOS_GEOMETRYCOLLECTION, parts.data(),
        static_cast<unsigned int>(parts.size())));
    return keep(wrap(GEOSUnaryUnion_r(m_context, collection.get())));
  }

  /** Makes @p geometry valid if it is not; counts repairs. */
  GeomPtr repair(GeomPtr geometry, int* repaired) const
  {
    if (geometry == nullptr || GEOSisValid_r(m_context, geometry.get()) == 1) {
      return geometry;
    }
    *repaired += 1;
    return keep(wrap(GEOSMakeValid_r(m_context, geometry.get())));
  }

private:
  GeosContext m_owner;
  GEOSContextHandle_t m_context = nullptr;
  GEOSWKBReader* m_reader = nullptr;
  GEOSWKBWriter* m_writer = nullptr;
};

/** Reads the coordinates of a ring or line string as lon, lat pairs. */
std::vector<double> coordinates(const Geos& geos, const GEOSGeometry* line)
{
  std::vector<double> lonLat;
  const GEOSCoordSequence* sequence =
      GEOSGeom_getCoordSeq_r(geos.context(), line);
  unsigned int size = 0;
  if (sequence == nullptr ||
      GEOSCoordSeq_getSize_r(geos.context(), sequence, &size) == 0) {
    return lonLat;
  }
  lonLat.reserve(static_cast<std::size_t>(size) * 2);
  for (unsigned int i = 0; i < size; ++i) {
    double x = 0.0;
    double y = 0.0;
    GEOSCoordSeq_getXY_r(geos.context(), sequence, i, &x, &y);
    lonLat.push_back(x);
    lonLat.push_back(y);
  }
  return lonLat;
}

/** Calls @p visit for every ring of the polygons in @p geometry. */
void forEachRing(const Geos& geos, const GEOSGeometry* geometry,
                 const std::function<void(const GEOSGeometry*, bool)>& visit)
{
  GEOSContextHandle_t context = geos.context();
  const int type = GEOSGeomTypeId_r(context, geometry);
  if (type == GEOS_POLYGON) {
    visit(GEOSGetExteriorRing_r(context, geometry), true);
    const int holes = GEOSGetNumInteriorRings_r(context, geometry);
    for (int i = 0; i < holes; ++i) {
      visit(GEOSGetInteriorRingN_r(context, geometry, i), false);
    }
  } else if (type == GEOS_MULTIPOLYGON || type == GEOS_GEOMETRYCOLLECTION) {
    const int count = GEOSGetNumGeometries_r(context, geometry);
    for (int i = 0; i < count; ++i) {
      forEachRing(geos, GEOSGetGeometryN_r(context, geometry, i), visit);
    }
  }
}

/** True if @p path exists, also inside GDAL's virtual files (/vsizip/). */
bool gdalPathExists(const QString& path)
{
  const QByteArray encoded = path.toUtf8();
  VSIStatBufL status{};
  return VSIStatL(encoded.constData(), &status) == 0;
}

/** File name part of @p path. */
QString baseName(const QString& path)
{
  return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

/** Area of the polygons of @p geometry with rhumb-line edges, km². */
double polygonAreaKm2(const Geos& geos, const GEOSGeometry* geometry)
{
  double area = 0.0;
  forEachRing(geos, geometry, [&](const GEOSGeometry* ring, bool outer) {
    const double ringArea = rhumbRingAreaKm2(coordinates(geos, ring));
    area += outer ? ringArea : -ringArea;
  });
  return area;
}

/** A GSHHG id without the -E/-W of a polygon split at the antimeridian. */
QString wholePolygonId(const QString& id)
{
  return (id.endsWith(QLatin1String("-E")) || id.endsWith(QLatin1String("-W")))
             ? id.left(id.size() - 2)
             : id;
}

/**
 * @p geometry with closeHalfAtPole() applied if it is a one-ring polygon
 * reaching the South Pole; otherwise null, to keep @p geometry.
 */
OGRGeometryH closedAtPole(OGRGeometryH geometry)
{
  if (wkbFlatten(OGR_G_GetGeometryType(geometry)) != wkbPolygon ||
      OGR_G_GetGeometryCount(geometry) != 1) {
    return nullptr;
  }
  OGREnvelope envelope{};
  OGR_G_GetEnvelope(geometry, &envelope);
  if (envelope.MinY > -90.0 + 1e-9) {
    return nullptr;
  }
  OGRGeometryH ring = OGR_G_GetGeometryRef(geometry, 0);
  const int count = OGR_G_GetPointCount(ring);
  std::vector<double> lonLat;
  lonLat.reserve(static_cast<std::size_t>(count) * 2);
  for (int i = 0; i < count; ++i) {
    lonLat.push_back(OGR_G_GetX(ring, i));
    lonLat.push_back(OGR_G_GetY(ring, i));
  }
  const std::vector<double> closed = closeHalfAtPole(lonLat);
  OGRGeometryH newRing = OGR_G_CreateGeometry(wkbLinearRing);
  for (std::size_t i = 0; i + 1 < closed.size(); i += 2) {
    OGR_G_AddPoint_2D(newRing, closed[i], closed[i + 1]);
  }
  OGR_G_AddPoint_2D(newRing, closed[0], closed[1]);
  OGRGeometryH polygon = OGR_G_CreateGeometry(wkbPolygon);
  OGR_G_AddGeometryDirectly(polygon, newRing);
  return polygon;
}

bool readLevel(const Geos& geos, const QString& path, int level,
               const ShorelineOptions& options, std::vector<Piece>* polygons,
               ShorelineLodStats* stats, QString* errorString)
{
  const GdalDataset dataset(path);
  if (!dataset.isOpen() || dataset.layerCount() < 1) {
    *errorString =
        QStringLiteral("Cannot read %1: %2").arg(path, dataset.errorString());
    return false;
  }
  OGRLayerH layer =
      GDALDatasetGetLayer(static_cast<GDALDatasetH>(dataset.handle()), 0);
  OGR_L_ResetReading(layer);
  const int areaField = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), "area");
  const int idField = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), "id");
  // GSHHG gives both halves of a polygon split at the antimeridian
  // (Eurasia, Antarctica) the whole polygon's area: count it once.
  std::set<QString> counted;
  // Land counts positive, water negative.
  const double sign = (level == 2 || level == 4) ? -1.0 : 1.0;
  for (OGRFeatureH feature = OGR_L_GetNextFeature(layer); feature != nullptr;
       feature = OGR_L_GetNextFeature(layer)) {
    const double areaKm2 =
        areaField >= 0 ? std::abs(OGR_F_GetFieldAsDouble(feature, areaField))
                       : 0.0;
    const QString id =
        idField >= 0
            ? QString::fromUtf8(OGR_F_GetFieldAsString(feature, idField))
            : QString::number(OGR_F_GetFID(feature));
    OGRGeometryH ogrGeometry = OGR_F_GetGeometryRef(feature);
    if (ogrGeometry == nullptr || areaKm2 < options.minimumAreaKm2) {
      OGR_F_Destroy(feature);
      continue;
    }
    // Halves of a polygon split at the antimeridian that reach the pole
    // (Antarctica) get their closure rebuilt; see closeHalfAtPole().
    OGRGeometryH rebuilt =
        wholePolygonId(id) != id ? closedAtPole(ogrGeometry) : nullptr;
    if (rebuilt != nullptr) {
      ogrGeometry = rebuilt;
    }
    std::vector<unsigned char> wkb(
        static_cast<std::size_t>(OGR_G_WkbSize(ogrGeometry)));
    OGR_G_ExportToIsoWkb(ogrGeometry, wkbNDR, wkb.data());
    if (rebuilt != nullptr) {
      OGR_G_DestroyGeometry(rebuilt);
    }
    OGR_F_Destroy(feature);
    GeomPtr geometry =
        geos.repair(geos.keep(geos.fromWkb(wkb)), &stats->repairedPolygons);
    if (geometry == nullptr) {
      continue;
    }
    stats->sourcePolygons += 1;
    if (counted.insert(wholePolygonId(id)).second) {
      stats->gshhgAreaKm2 += sign * areaKm2;
    }
    stats->sourceAreaKm2 += sign * polygonAreaKm2(geos, geometry.get());
    const Box box = geos.box(geometry.get());
    polygons->push_back(Piece{std::move(geometry), level, box});
  }
  return true;
}

/** Writes the land of one resolution while descending the quadtree. */
class TileCutter
{
public:
  TileCutter(const Geos& geos, const ShorelineLod& lod,
             GeoPackageFeatureWriter* writer, ShorelineLodStats* stats)
      : m_geos(&geos), m_lod(lod), m_writer(writer), m_stats(stats),
        m_table(QStringLiteral("land_") + lod.resolution)
  {
  }

  /** Clips @p pieces to @p key and cuts further, down to storage. */
  bool cut(TileKey key, const std::vector<Piece>& pieces)
  {
    const Box box = boxOf(key);
    std::vector<Piece> inside;
    for (const Piece& piece : pieces) {
      if (!piece.box.overlaps(box)) {
        continue;
      }
      GeomPtr clipped = m_geos->clip(piece, box);
      if (clipped != nullptr) {
        const Box clippedBox = m_geos->box(clipped.get());
        inside.push_back(Piece{std::move(clipped), piece.level, clippedBox});
      }
    }
    if (inside.empty()) {
      return true;
    }
    if (key.level == m_lod.storageLevel) {
      return writeTile(key, inside);
    }
    for (int i = 0; i < 4; ++i) {
      if (!cut(key.child(i), inside)) {
        return false;
      }
    }
    return true;
  }

private:
  /** Land = levels 1 and 5, minus lakes (2), plus islands (3), minus ponds. */
  bool writeTile(TileKey key, const std::vector<Piece>& pieces)
  {
    std::array<std::vector<const GEOSGeometry*>, 4> byRole;
    for (const Piece& piece : pieces) {
      const int role = piece.level == 5 ? 0 : piece.level - 1;
      if (role >= 0 && role < 4) {
        byRole.at(static_cast<std::size_t>(role))
            .push_back(piece.geometry.get());
      }
    }
    GEOSContextHandle_t context = m_geos->context();
    GeomPtr land = m_geos->unionOf(byRole.at(0));
    for (std::size_t role = 1; role < 4; ++role) {
      const GeomPtr other = m_geos->unionOf(byRole.at(role));
      if (other == nullptr) {
        continue;
      }
      if (role == 2) {
        land = land == nullptr
                   ? m_geos->wrap(GEOSGeom_clone_r(context, other.get()))
                   : m_geos->keep(m_geos->wrap(
                         GEOSUnion_r(context, land.get(), other.get())));
      } else if (land != nullptr) {
        land = m_geos->keep(
            m_geos->wrap(GEOSDifference_r(context, land.get(), other.get())));
      }
    }
    land = m_geos->repair(std::move(land), &m_stats->repairedPolygons);
    if (land == nullptr) {
      return true;
    }
    m_stats->landTiles += 1;
    m_stats->vertices += GEOSGetNumCoordinates_r(context, land.get());
    m_stats->landAreaKm2 += polygonAreaKm2(*m_geos, land.get());
    return m_writer->putFeature(m_table, key, m_geos->toWkb(land.get()));
  }

  const Geos* m_geos;
  ShorelineLod m_lod;
  GeoPackageFeatureWriter* m_writer;
  ShorelineLodStats* m_stats;
  QString m_table;
};

} // namespace

namespace
{

/** A vertex, degrees. */
struct LonLat {
  double lon;
  double lat;
};

/**
 * The boundary meridian (−180°, 0° or 180°) within 0.01° (about 380 m at
 * the equator, 130 m at 70°S) of @p lon, or NaN.
 */
double boundaryMeridian(double lon)
{
  for (const double boundary : {-180.0, 0.0, 180.0}) {
    if (std::abs(lon - boundary) < 0.01) {
      return boundary;
    }
  }
  return std::nan("");
}

/**
 * The index of the one vertex of the cyclic @p points whose edge to the
 * next goes from one boundary meridian to the other, or points.size() if
 * there is none or more than one.
 */
std::size_t crossingEdge(const std::vector<LonLat>& points)
{
  std::size_t found = points.size();
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double from = boundaryMeridian(points[i].lon);
    const double to = boundaryMeridian(points[(i + 1) % points.size()].lon);
    if (std::isnan(from) || std::isnan(to) || from == to) {
      continue;
    }
    if (found != points.size()) {
      return points.size();
    }
    found = i;
  }
  return found;
}

} // namespace

std::vector<double> closeHalfAtPole(const std::vector<double>& lonLatDeg)
{
  // Vertices off the pole, without the closing one and without repeats.
  std::vector<LonLat> points;
  bool reachesPole = false;
  for (std::size_t i = 0; i + 1 < lonLatDeg.size(); i += 2) {
    const LonLat point{lonLatDeg[i], lonLatDeg[i + 1]};
    if (point.lat <= -90.0 + 1e-9) {
      reachesPole = true;
    } else if (points.empty() || points.back().lon != point.lon ||
               points.back().lat != point.lat) {
      points.push_back(point);
    }
  }
  while (points.size() > 1 && points.front().lon == points.back().lon &&
         points.front().lat == points.back().lat) {
    points.pop_back();
  }
  if (!reachesPole || points.size() < 3) {
    return lonLatDeg;
  }
  const std::size_t last = crossingEdge(points);
  if (last == points.size()) {
    return lonLatDeg;
  }
  // The coastline from the vertex after that edge round to the one before
  // it, its ends on their meridians, then down, along the pole and up.
  const std::size_t first = (last + 1) % points.size();
  const double startLon = boundaryMeridian(points[first].lon);
  const double endLon = boundaryMeridian(points[last].lon);
  std::vector<double> ring;
  ring.reserve(points.size() * 2 + 4);
  for (std::size_t k = 0; k < points.size(); ++k) {
    const std::size_t i = (first + k) % points.size();
    double lon = points[i].lon;
    if (i == first) {
      lon = startLon;
    } else if (i == last) {
      lon = endLon;
    }
    ring.push_back(lon);
    ring.push_back(points[i].lat);
  }
  ring.insert(ring.end(), {endLon, -90.0, startLon, -90.0});
  return ring;
}

double rhumbRingAreaKm2(const std::vector<double>& lonLatDeg)
{
  GeographicLib::PolygonAreaRhumb polygon(GeographicLib::Rhumb::WGS84());
  std::size_t points = lonLatDeg.size() / 2;
  // The closing point repeats the first; the ring is closed below.
  if (points > 1 && lonLatDeg[2 * points - 2] == lonLatDeg[0] &&
      lonLatDeg[2 * points - 1] == lonLatDeg[1]) {
    points -= 1;
  }
  for (std::size_t i = 0; i < points; ++i) {
    const double lon = lonLatDeg[2 * i];
    const double lat = lonLatDeg[2 * i + 1];
    polygon.AddPoint(lat, lon);
    // GeographicLib takes each edge's longitude step modulo 360° into
    // (−180°, 180°], so an edge from −180° to 180° (Antarctica's edge
    // along the pole, or a full parallel) would count as nothing. Split
    // long edges into steps under 90°, straight in longitude and latitude
    // (exact for parallels, the only long edges in practice).
    const std::size_t next = (i + 1) % points;
    const double lonStep = lonLatDeg[2 * next] - lon;
    const double latStep = lonLatDeg[2 * next + 1] - lat;
    const int steps = static_cast<int>(std::ceil(std::abs(lonStep) / 90.0));
    for (int step = 1; step < steps; ++step) {
      const double t = static_cast<double>(step) / steps;
      polygon.AddPoint(lat + t * latStep, lon + t * lonStep);
    }
  }
  double perimeterM = 0.0;
  double areaM2 = 0.0;
  polygon.Compute(false, true, perimeterM, areaM2);
  return std::abs(areaM2) * 1e-6;
}

bool buildShoreline(const ShorelineOptions& options,
                    GeoPackageFeatureWriter* writer,
                    std::vector<ShorelineLodStats>* stats, QString* errorString)
{
  const std::function<void(const QString&)> progress =
      options.progress ? options.progress : [](const QString&) {};
  for (const ShorelineLod& lod : options.lods) {
    FeatureTableSpec land;
    land.name = QStringLiteral("land_") + lod.resolution;
    land.geometry = FeatureTableSpec::Geometry::Polygons;
    land.storageLevel = lod.storageLevel;
    land.minLevel = lod.minLevel;
    land.maxLevel = lod.maxLevel;
    if (!writer->addTable(land)) {
      *errorString = writer->errorString();
      return false;
    }
  }

  const Geos geos;
  for (const ShorelineLod& lod : options.lods) {
    ShorelineLodStats lodStats;
    lodStats.resolution = lod.resolution;
    std::vector<Piece> polygons;
    for (int level = 1; level <= 5; ++level) {
      const QString path =
          options.shapefileDirectory +
          QStringLiteral("/%1/GSHHS_%1_L%2.shp").arg(lod.resolution).arg(level);
      if (!gdalPathExists(path)) {
        continue; // Not every resolution has every level (c has no L4).
      }
      progress(QStringLiteral("Reading %1").arg(baseName(path)));
      if (!readLevel(geos, path, level, options, &polygons, &lodStats,
                     errorString)) {
        return false;
      }
    }
    if (polygons.empty()) {
      *errorString = QStringLiteral("No GSHHG polygons for resolution %1 in %2")
                         .arg(lod.resolution, options.shapefileDirectory);
      return false;
    }
    progress(QStringLiteral("Cutting resolution %1 to level %2")
                 .arg(lod.resolution)
                 .arg(lod.storageLevel));
    TileCutter cutter(geos, lod, writer, &lodStats);
    for (const TileKey root : TileKey::roots()) {
      if (!cutter.cut(root, polygons)) {
        *errorString = writer->errorString();
        return false;
      }
    }
    stats->push_back(lodStats);
  }
  return true;
}

} // namespace ts::packager
