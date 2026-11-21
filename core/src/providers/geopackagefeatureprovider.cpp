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

#include "ts/providers/geopackagefeatureprovider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

#include <QFileInfo>
#include <QRegularExpression>
#include <QVariant>

#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <sqlite3.h>

#include "sqlitestatementpool.h"
#include "ts/deps/dependencies.h"
#include "ts/deps/sqliteconnection.h"
#include "ts/tiling/tilekey.h"
#include "ts/vector/featurepick.h"

namespace ts
{

namespace
{

QString tablesTableName()
{
  return QStringLiteral("ts_feature_tables"); // ts-notr: table name
}

bool isPlainIdentifier(const QString& name)
{
  static const QRegularExpression pattern(
      QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,62}$"));
  return pattern.match(name).hasMatch();
}

QString geometryName(FeatureTableSpec::Geometry geometry)
{
  switch (geometry) {
  case FeatureTableSpec::Geometry::Lines:
    return QStringLiteral("lines"); // ts-notr: identifier
  case FeatureTableSpec::Geometry::Points:
    return QStringLiteral("points"); // ts-notr: identifier
  case FeatureTableSpec::Geometry::Polygons:
    break;
  }
  return QStringLiteral("polygons"); // ts-notr: identifier
}

QString lastGdalError()
{
  return QString::fromUtf8(CPLGetLastErrorMsg());
}

// --- WKB decoding
// -------------------------------------------------------------

/** Reads WKB with bounds checks; any overrun makes ok false. */
class WkbReader
{
public:
  WkbReader(const unsigned char* data, std::size_t size)
      : m_data(data), m_size(size)
  {
  }

  bool ok() const { return m_ok; }

  std::uint8_t byte()
  {
    if (!need(1)) {
      return 0;
    }
    const std::uint8_t value = m_data[m_pos];
    m_pos += 1;
    return value;
  }

  std::uint32_t uint32(bool littleEndian)
  {
    if (!need(4)) {
      return 0;
    }
    std::uint32_t value = 0;
    for (int32_t i = 0; i < 4; ++i) {
      const std::size_t index =
          m_pos + static_cast<std::size_t>(littleEndian ? 3 - i : i);
      value = (value << 8U) | m_data[index];
    }
    m_pos += 4;
    return value;
  }

  double float64(bool littleEndian)
  {
    if (!need(8)) {
      return 0.0;
    }
    std::uint64_t bits = 0;
    for (int32_t i = 0; i < 8; ++i) {
      const std::size_t index =
          m_pos + static_cast<std::size_t>(littleEndian ? 7 - i : i);
      bits = (bits << 8U) | m_data[index];
    }
    m_pos += 8;
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
  }

  void skip(std::size_t bytes)
  {
    if (need(bytes)) {
      m_pos += bytes;
    }
  }

  /** Checks that @p count items of @p itemBytes each can still follow. */
  bool canHold(std::uint32_t count, std::size_t itemBytes)
  {
    m_ok = m_ok && static_cast<std::uint64_t>(count) * itemBytes <=
                       static_cast<std::uint64_t>(m_size - m_pos);
    return m_ok;
  }

private:
  bool need(std::size_t bytes)
  {
    m_ok = m_ok && bytes <= m_size - m_pos;
    return m_ok;
  }

  const unsigned char* m_data;
  std::size_t m_size;
  std::size_t m_pos = 0;
  bool m_ok = true;
};

/** Geometry type and coordinate dimension from a WKB type word. */
struct WkbType {
  std::uint32_t base = 0;
  std::size_t dimensions = 2;
};

WkbType splitType(std::uint32_t type)
{
  WkbType result;
  // EWKB flags in the high bits, ISO Z/M/ZM as +1000/+2000/+3000.
  const bool ewkbZ = (type & 0x80000000U) != 0;
  const bool ewkbM = (type & 0x40000000U) != 0;
  type &= 0x0fffffffU;
  const std::uint32_t iso = type / 1000U;
  result.base = type % 1000U;
  result.dimensions = 2 + ((iso == 1 || iso == 3 || ewkbZ) ? 1U : 0U) +
                      ((iso == 2 || iso == 3 || ewkbM) ? 1U : 0U);
  return result;
}

bool readPoints(WkbReader& reader, bool little, std::size_t dimensions,
                std::vector<GeoPoint>* points)
{
  const std::uint32_t count = reader.uint32(little);
  if (!reader.canHold(count, 8 * dimensions)) {
    return false;
  }
  points->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    GeoPoint point;
    point.lonDeg = reader.float64(little);
    point.latDeg = reader.float64(little);
    reader.skip(8 * (dimensions - 2));
    points->push_back(point);
  }
  return reader.ok();
}

bool readPolygonBody(WkbReader& reader, bool little, std::size_t dimensions,
                     Feature* feature)
{
  const std::uint32_t rings = reader.uint32(little);
  if (!reader.canHold(rings, 4)) {
    return false;
  }
  feature->type = Feature::Type::Polygon;
  for (std::uint32_t i = 0; i < rings; ++i) {
    std::vector<GeoPoint> ring;
    if (!readPoints(reader, little, dimensions, &ring)) {
      return false;
    }
    feature->parts.push_back(std::move(ring));
  }
  return reader.ok();
}

bool readGeometry(WkbReader& reader, int32_t depth, std::vector<Feature>* out);

/**
 * Reads the members of a multipoint (@p type 4), multi line string (5) or
 * multipolygon (6): one feature with every point or line, or one feature
 * per polygon.
 */
bool readMulti(WkbReader& reader, bool little, WkbType type, int32_t depth,
               std::vector<Feature>* out)
{
  const std::uint32_t count = reader.uint32(little);
  if (!reader.canHold(count, 9)) {
    return false;
  }
  const std::uint32_t expected = type.base - 3U;
  std::vector<Feature> members;
  for (std::uint32_t i = 0; i < count; ++i) {
    // Peek at the member's type through a copy of the reader.
    WkbReader peek = reader;
    const bool memberLittle = peek.byte() == 1;
    if (splitType(peek.uint32(memberLittle)).base != expected ||
        !readGeometry(reader, depth + 1, &members)) {
      return false;
    }
  }
  if (type.base == 6) {
    for (Feature& member : members) {
      out->push_back(std::move(member));
    }
    return true;
  }
  Feature merged;
  merged.type =
      type.base == 4 ? Feature::Type::Points : Feature::Type::LineStrings;
  for (Feature& member : members) {
    for (std::vector<GeoPoint>& part : member.parts) {
      merged.parts.push_back(std::move(part));
    }
  }
  out->push_back(std::move(merged));
  return true;
}

/** Reads one WKB geometry (its byte order and type first). */
bool readGeometry(WkbReader& reader, int32_t depth, std::vector<Feature>* out)
{
  const bool little = reader.byte() == 1;
  const WkbType type = splitType(reader.uint32(little));
  if (!reader.ok() || depth > 1) {
    return false;
  }
  Feature feature;
  switch (type.base) {
  case 1: { // Point
    feature.type = Feature::Type::Points;
    if (!reader.canHold(1, 8 * type.dimensions)) {
      return false;
    }
    GeoPoint point;
    point.lonDeg = reader.float64(little);
    point.latDeg = reader.float64(little);
    reader.skip(8 * (type.dimensions - 2));
    if (!reader.ok()) {
      return false;
    }
    // An empty point is written as NaN coordinates.
    if (!std::isnan(point.lonDeg) && !std::isnan(point.latDeg)) {
      feature.parts.push_back({point});
    }
    out->push_back(std::move(feature));
    return true;
  }
  case 2: { // LineString
    feature.type = Feature::Type::LineStrings;
    std::vector<GeoPoint> line;
    if (!readPoints(reader, little, type.dimensions, &line)) {
      return false;
    }
    feature.parts.push_back(std::move(line));
    out->push_back(std::move(feature));
    return true;
  }
  case 3: // Polygon
    if (!readPolygonBody(reader, little, type.dimensions, &feature)) {
      return false;
    }
    out->push_back(std::move(feature));
    return true;
  case 4: // MultiPoint
  case 5: // MultiLineString
  case 6: // MultiPolygon
    return readMulti(reader, little, type, depth, out);
  default:
    return false;
  }
}

} // namespace

bool decodeGeoPackageGeometry(const unsigned char* data, std::size_t sizeBytes,
                              std::vector<Feature>* features)
{
  if (data == nullptr || sizeBytes < 8 || data[0] != 'G' || data[1] != 'P') {
    return false;
  }
  const std::uint8_t flags = data[3];
  if ((flags & 0x10U) != 0) {
    return true; // An empty geometry: nothing to add.
  }
  static constexpr std::array<std::size_t, 5> envelopeBytes = {0, 32, 48, 48,
                                                               64};
  const std::size_t envelope = (flags >> 1U) & 0x07U;
  if (envelope >= envelopeBytes.size()) {
    return false;
  }
  const std::size_t headerBytes = 8 + envelopeBytes.at(envelope);
  if (sizeBytes <= headerBytes) {
    return false;
  }
  WkbReader reader(data + headerBytes, sizeBytes - headerBytes);
  std::vector<Feature> decoded;
  if (!readGeometry(reader, 0, &decoded) || !reader.ok()) {
    return false;
  }
  for (Feature& feature : decoded) {
    features->push_back(std::move(feature));
  }
  return true;
}

// --- Writer
// -------------------------------------------------------------------

/** GDAL handles of the writer. */
struct GeoPackageFeatureWriter::Private {
  GDALDatasetH dataset = nullptr;
  OGRSpatialReferenceH srs = nullptr;
  bool inTransaction = false;
  bool spatialIndex = true;
  std::vector<std::pair<FeatureTableSpec, OGRLayerH>> tables;

  Private() = default;
  ~Private()
  {
    if (dataset != nullptr) {
      if (inTransaction) {
        GDALDatasetRollbackTransaction(dataset);
      }
      GDALClose(dataset);
    }
    if (srs != nullptr) {
      OSRDestroySpatialReference(srs);
    }
  }
  Private(const Private&) = delete;
  Private& operator=(const Private&) = delete;
  Private(Private&&) = delete;
  Private& operator=(Private&&) = delete;

  OGRLayerH layer(const QString& name) const
  {
    for (const std::pair<FeatureTableSpec, OGRLayerH>& entry : tables) {
      if (entry.first.name == name) {
        return entry.second;
      }
    }
    return nullptr;
  }
};

GeoPackageFeatureWriter::GeoPackageFeatureWriter(const QString& path,
                                                 bool spatialIndex)
    : d(std::make_unique<Private>())
{
  d->spatialIndex = spatialIndex;
  initialiseGdal();
  if (QFileInfo::exists(path)) {
    m_error = QStringLiteral("%1 already exists").arg(path);
    return;
  }
  GDALDriverH driver = GDALGetDriverByName("GPKG");
  if (driver == nullptr) {
    m_error = QStringLiteral("GDAL has no GPKG driver");
    return;
  }
  CPLErrorReset();
  const QByteArray encoded = path.toUtf8();
  d->dataset =
      GDALCreate(driver, encoded.constData(), 0, 0, 0, GDT_Unknown, nullptr);
  if (d->dataset == nullptr) {
    m_error = lastGdalError();
    return;
  }
  // WGS 84 longitude/latitude, built without PROJ's database; the GPKG
  // driver recognises it as EPSG:4326.
  d->srs = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(d->srs, "WGS84");
  OSRSetAxisMappingStrategy(d->srs, OAMS_TRADITIONAL_GIS_ORDER);
}

GeoPackageFeatureWriter::~GeoPackageFeatureWriter() = default;

bool GeoPackageFeatureWriter::addTable(const FeatureTableSpec& spec)
{
  if (!m_error.isEmpty() || d->dataset == nullptr) {
    return false;
  }
  if (!isPlainIdentifier(spec.name) || d->layer(spec.name) != nullptr ||
      spec.storageLevel < 0 || spec.minLevel < spec.storageLevel ||
      (spec.maxLevel >= 0 && spec.maxLevel < spec.minLevel)) {
    m_error = QStringLiteral("Bad feature table %1").arg(spec.name);
    return false;
  }
  if (d->inTransaction) {
    m_error = QStringLiteral("Add every table before the first feature");
    return false;
  }
  char** options = nullptr;
  options = CSLSetNameValue(options, "GEOMETRY_NAME", "geom");
  options = CSLSetNameValue(options, "FID", "fid");
  options =
      CSLSetNameValue(options, "SPATIAL_INDEX", d->spatialIndex ? "YES" : "NO");
  const QByteArray name = spec.name.toUtf8();
  CPLErrorReset();
  OGRwkbGeometryType type = wkbMultiPolygon;
  if (spec.geometry == FeatureTableSpec::Geometry::Lines) {
    type = wkbMultiLineString;
  } else if (spec.geometry == FeatureTableSpec::Geometry::Points) {
    type = wkbMultiPoint;
  }
  OGRLayerH layer = GDALDatasetCreateLayer(d->dataset, name.constData(), d->srs,
                                           type, options);
  CSLDestroy(options);
  if (layer == nullptr) {
    m_error = lastGdalError();
    return false;
  }
  std::vector<QString> columns = {QStringLiteral("tile_level"),
                                  QStringLiteral("tile_x"),
                                  QStringLiteral("tile_y")};
  for (const QString& attribute : spec.attributes) {
    if (!isPlainIdentifier(attribute)) {
      m_error = QStringLiteral("Bad attribute name %1").arg(attribute);
      return false;
    }
    columns.push_back(attribute);
  }
  const std::size_t integerColumns = columns.size();
  for (const QString& attribute : spec.textAttributes) {
    if (!isPlainIdentifier(attribute)) {
      m_error = QStringLiteral("Bad attribute name %1").arg(attribute);
      return false;
    }
    columns.push_back(attribute);
  }
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const QByteArray columnName = columns[i].toUtf8();
    OGRFieldDefnH field = OGR_Fld_Create(
        columnName.constData(), i < integerColumns ? OFTInteger64 : OFTString);
    const OGRErr result = OGR_L_CreateField(layer, field, TRUE);
    OGR_Fld_Destroy(field);
    if (result != OGRERR_NONE) {
      m_error = lastGdalError();
      return false;
    }
  }
  d->tables.emplace_back(spec, layer);
  return true;
}

bool GeoPackageFeatureWriter::putFeature(const QString& table, TileKey tile,
                                         const QByteArray& wkb,
                                         const std::vector<int64_t>& attributes)
{
  std::vector<std::optional<int64_t>> integers(attributes.begin(),
                                               attributes.end());
  std::vector<QString> texts;
  for (const std::pair<FeatureTableSpec, OGRLayerH>& entry : d->tables) {
    if (entry.first.name == table) {
      texts.resize(entry.first.textAttributes.size());
    }
  }
  return putFeature(table, tile, wkb, integers, texts);
}

bool GeoPackageFeatureWriter::putFeature(
    const QString& table, TileKey tile, const QByteArray& wkb,
    const std::vector<std::optional<int64_t>>& integers,
    const std::vector<QString>& texts)
{
  if (!m_error.isEmpty() || d->dataset == nullptr) {
    return false;
  }
  OGRLayerH layer = d->layer(table);
  const FeatureTableSpec* spec = nullptr;
  for (const std::pair<FeatureTableSpec, OGRLayerH>& entry : d->tables) {
    spec = entry.first.name == table ? &entry.first : spec;
  }
  if (layer == nullptr || spec == nullptr ||
      integers.size() != spec->attributes.size() ||
      texts.size() != spec->textAttributes.size()) {
    m_error = QStringLiteral("No table %1 or wrong attributes").arg(table);
    return false;
  }
  if (!d->inTransaction) {
    if (GDALDatasetStartTransaction(d->dataset, FALSE) != OGRERR_NONE) {
      m_error = lastGdalError();
      return false;
    }
    d->inTransaction = true;
  }
  OGRGeometryH geometry = nullptr;
  if (OGR_G_CreateFromWkb(wkb.constData(), nullptr, &geometry,
                          static_cast<int32_t>(wkb.size())) != OGRERR_NONE) {
    m_error = QStringLiteral("Bad WKB for table %1").arg(table);
    return false;
  }
  switch (spec->geometry) {
  case FeatureTableSpec::Geometry::Lines:
    geometry = OGR_G_ForceToMultiLineString(geometry);
    break;
  case FeatureTableSpec::Geometry::Points:
    geometry = OGR_G_ForceTo(geometry, wkbMultiPoint, nullptr);
    break;
  case FeatureTableSpec::Geometry::Polygons:
    geometry = OGR_G_ForceToMultiPolygon(geometry);
    break;
  }
  OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(layer));
  OGR_F_SetGeometryDirectly(feature, geometry);
  OGR_F_SetFieldInteger64(feature, 0, tile.level);
  OGR_F_SetFieldInteger64(feature, 1, tile.x);
  OGR_F_SetFieldInteger64(feature, 2, tile.y);
  for (std::size_t i = 0; i < integers.size(); ++i) {
    const int32_t field = static_cast<int32_t>(i) + 3;
    if (integers[i]) {
      OGR_F_SetFieldInteger64(feature, field, *integers[i]);
    } else {
      OGR_F_SetFieldNull(feature, field);
    }
  }
  for (std::size_t i = 0; i < texts.size(); ++i) {
    const int32_t field = static_cast<int32_t>(i + integers.size()) + 3;
    if (texts[i].isNull()) {
      OGR_F_SetFieldNull(feature, field);
    } else {
      const QByteArray value = texts[i].toUtf8();
      OGR_F_SetFieldString(feature, field, value.constData());
    }
  }
  const OGRErr result = OGR_L_CreateFeature(layer, feature);
  OGR_F_Destroy(feature);
  if (result != OGRERR_NONE) {
    m_error = lastGdalError();
    return false;
  }
  return true;
}

bool GeoPackageFeatureWriter::finish()
{
  if (!m_error.isEmpty() || d->dataset == nullptr) {
    return false;
  }
  if (!d->inTransaction) {
    if (GDALDatasetStartTransaction(d->dataset, FALSE) != OGRERR_NONE) {
      m_error = lastGdalError();
      return false;
    }
    d->inTransaction = true;
  }
  const QByteArray listName = tablesTableName().toUtf8();
  OGRLayerH list = GDALDatasetCreateLayer(d->dataset, listName.constData(),
                                          nullptr, wkbNone, nullptr);
  if (list == nullptr) {
    m_error = lastGdalError();
    return false;
  }
  const std::array<std::pair<const char*, OGRFieldType>, 5> fields = {
      {{"table_name", OFTString},
       {"geometry", OFTString},
       {"storage_level", OFTInteger},
       {"min_level", OFTInteger},
       {"max_level", OFTInteger}}};
  for (const std::pair<const char*, OGRFieldType>& field : fields) {
    OGRFieldDefnH definition = OGR_Fld_Create(field.first, field.second);
    OGR_L_CreateField(list, definition, TRUE);
    OGR_Fld_Destroy(definition);
  }
  for (const std::pair<FeatureTableSpec, OGRLayerH>& entry : d->tables) {
    const FeatureTableSpec& spec = entry.first;
    const QByteArray name = spec.name.toUtf8();
    const QByteArray geometry = geometryName(spec.geometry).toUtf8();
    OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(list));
    OGR_F_SetFieldString(feature, 0, name.constData());
    OGR_F_SetFieldString(feature, 1, geometry.constData());
    OGR_F_SetFieldInteger(feature, 2, spec.storageLevel);
    OGR_F_SetFieldInteger(feature, 3, spec.minLevel);
    OGR_F_SetFieldInteger(feature, 4, spec.maxLevel);
    const OGRErr result = OGR_L_CreateFeature(list, feature);
    OGR_F_Destroy(feature);
    // The names were checked by addTable(), so they can go into SQL.
    const QByteArray index =
        QStringLiteral("CREATE INDEX \"%1_tile\" ON \"%1\" "
                       "(tile_level, tile_x, tile_y)")
            .arg(spec.name)
            .toUtf8();
    OGRLayerH none =
        GDALDatasetExecuteSQL(d->dataset, index.constData(), nullptr, nullptr);
    if (none != nullptr) {
      GDALDatasetReleaseResultSet(d->dataset, none);
    }
    if (result != OGRERR_NONE) {
      m_error = lastGdalError();
      return false;
    }
  }
  if (GDALDatasetCommitTransaction(d->dataset) != OGRERR_NONE) {
    m_error = lastGdalError();
    return false;
  }
  d->inTransaction = false;
  // Compact the file: the R-tree and index builds leave free pages.
  OGRLayerH vacuum =
      GDALDatasetExecuteSQL(d->dataset, "VACUUM", nullptr, nullptr);
  if (vacuum != nullptr) {
    GDALDatasetReleaseResultSet(d->dataset, vacuum);
  }
  CPLErrorReset();
  GDALClose(d->dataset);
  d->dataset = nullptr;
  if (CPLGetLastErrorType() >= CE_Failure) {
    m_error = lastGdalError();
    return false;
  }
  return true;
}

// --- Provider
// -----------------------------------------------------------------

/** One table with its reader pool. */
struct GeoPackageFeatureProvider::Table {
  FeatureTableSpec spec;
  std::unique_ptr<SqliteStatementPool> pool;
};

namespace
{

QString columnText(sqlite3_stmt* statement, int32_t column)
{
  const unsigned char* text = sqlite3_column_text(statement, column);
  return text ? QString::fromUtf8(reinterpret_cast<const char*>(text))
              : QString();
}

/** Returns the geometry column of @p table, or an empty string. */
QString geometryColumn(const SqliteConnection& connection, const QString& table)
{
  sqlite3_stmt* statement = nullptr;
  QString column;
  if (sqlite3_prepare_v2(connection.get(),
                         "SELECT column_name FROM gpkg_geometry_columns "
                         "WHERE table_name = ?",
                         -1, &statement, nullptr) == SQLITE_OK) {
    const QByteArray name = table.toUtf8();
    sqlite3_bind_text(statement, 1, name.constData(),
                      static_cast<int32_t>(name.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW) {
      column = columnText(statement, 0);
    }
  }
  sqlite3_finalize(statement);
  return isPlainIdentifier(column) ? column : QString();
}

/**
 * Returns the integer columns (or, with @p text, the text columns) of
 * @p table other than fid and tile_*: the attributes (with the geometry
 * column, if its type matches).
 */
std::vector<QString> attributeColumns(const SqliteConnection& connection,
                                      const QString& table, bool text)
{
  std::vector<QString> columns;
  sqlite3_stmt* statement = nullptr;
  const QByteArray sql =
      QStringLiteral("PRAGMA table_info(\"%1\")").arg(table).toUtf8();
  if (sqlite3_prepare_v2(connection.get(), sql.constData(), -1, &statement,
                         nullptr) == SQLITE_OK) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const QString name = columnText(statement, 1);
      const QString type = columnText(statement, 2).toUpper();
      const bool matches = text ? (type.contains(QLatin1String("TEXT")) ||
                                   type.contains(QLatin1String("CHAR")))
                                : type.contains(QLatin1String("INT"));
      if (name != QLatin1String("fid") &&
          !name.startsWith(QLatin1String("tile_")) && matches &&
          isPlainIdentifier(name)) {
        columns.push_back(name);
      }
    }
  }
  sqlite3_finalize(statement);
  return columns;
}

/**
 * Steps @p statement (bound to one tile) and appends its features to
 * @p features with the table's attributes. False if cancelled.
 */
bool readRows(sqlite3_stmt* statement, const FeatureTableSpec& spec,
              const std::atomic<bool>& cancelled,
              std::vector<Feature>* features)
{
  while (sqlite3_step(statement) == SQLITE_ROW) {
    if (cancelled.load()) {
      return false;
    }
    std::vector<Feature> decoded;
    const unsigned char* blob =
        static_cast<const unsigned char*>(sqlite3_column_blob(statement, 0));
    const int32_t bytes = sqlite3_column_bytes(statement, 0);
    if (!decodeGeoPackageGeometry(blob, static_cast<std::size_t>(bytes),
                                  &decoded)) {
      continue; // Damaged geometry: skip the feature.
    }
    QVariantMap attributes;
    attributes.insert(QStringLiteral("table"), spec.name);
    QString id;
    for (std::size_t i = 0; i < spec.attributes.size(); ++i) {
      const int32_t column = static_cast<int32_t>(i) + 1;
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        continue;
      }
      const qlonglong value =
          static_cast<qlonglong>(sqlite3_column_int64(statement, column));
      attributes.insert(spec.attributes[i], value);
      if (spec.attributes[i] == QLatin1String("feature_id")) {
        id = QString::number(value);
      }
    }
    for (std::size_t i = 0; i < spec.textAttributes.size(); ++i) {
      const int32_t column =
          static_cast<int32_t>(i + spec.attributes.size()) + 1;
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        continue;
      }
      attributes.insert(spec.textAttributes[i], columnText(statement, column));
    }
    for (Feature& feature : decoded) {
      feature.attributes = attributes;
      feature.id = id;
      features->push_back(std::move(feature));
    }
  }
  return true;
}

} // namespace

GeoPackageFeatureProvider::GeoPackageFeatureProvider(const QString& path)
    : GeoPackageFeatureProvider(path, Options())
{
}

namespace
{

/** Returns true if table @p name is selected by @p wanted (see Options). */
bool tableWanted(const QString& name, const std::vector<QString>& wanted)
{
  if (wanted.empty()) {
    return true;
  }
  static const QRegularExpression suffix(QStringLiteral("_[0-9]+$"));
  QString base = name;
  base.remove(suffix);
  return std::find(wanted.begin(), wanted.end(), name) != wanted.end() ||
         std::find(wanted.begin(), wanted.end(), base) != wanted.end();
}

} // namespace

GeoPackageFeatureProvider::GeoPackageFeatureProvider(const QString& path,
                                                     const Options& options)
    : m_path(QFileInfo(path).absoluteFilePath()), m_options(options),
      m_rules(styleRulesFromJson(options.styleRules))
{
  if (!QFileInfo(m_path).isFile()) {
    m_error = QStringLiteral("%1 does not exist").arg(m_path);
    return;
  }
  const SqliteConnection connection(m_path,
                                    SqliteConnection::OpenMode::ReadOnly);
  if (!connection.isOpen()) {
    m_error = connection.errorString();
    return;
  }
  sqlite3_stmt* statement = nullptr;
  const QByteArray listSql =
      QStringLiteral("SELECT table_name, geometry, storage_level, min_level, "
                     "max_level FROM %1")
          .arg(tablesTableName())
          .toUtf8();
  if (sqlite3_prepare_v2(connection.get(), listSql.constData(), -1, &statement,
                         nullptr) != SQLITE_OK) {
    m_error = QStringLiteral("No feature table list: %1")
                  .arg(QString::fromUtf8(sqlite3_errmsg(connection.get())));
    sqlite3_finalize(statement);
    return;
  }
  while (sqlite3_step(statement) == SQLITE_ROW) {
    FeatureTableSpec spec;
    spec.name = columnText(statement, 0);
    const QString geometry = columnText(statement, 1);
    if (geometry == QLatin1String("lines")) {
      spec.geometry = FeatureTableSpec::Geometry::Lines;
    } else if (geometry == QLatin1String("points")) {
      spec.geometry = FeatureTableSpec::Geometry::Points;
    } else {
      spec.geometry = FeatureTableSpec::Geometry::Polygons;
    }
    spec.storageLevel = sqlite3_column_int(statement, 2);
    spec.minLevel = sqlite3_column_int(statement, 3);
    spec.maxLevel = sqlite3_column_int(statement, 4);
    const QString column = isPlainIdentifier(spec.name)
                               ? geometryColumn(connection, spec.name)
                               : QString();
    if (column.isEmpty() || spec.storageLevel < 0 ||
        spec.storageLevel > TileKey::maximumLevel ||
        spec.minLevel < spec.storageLevel ||
        (geometry != QLatin1String("lines") &&
         geometry != QLatin1String("polygons") &&
         geometry != QLatin1String("points")) ||
        !tableWanted(spec.name, options.tables)) {
      continue; // Not a table we can read (or want); skip it.
    }
    spec.attributes = attributeColumns(connection, spec.name, false);
    spec.attributes.erase(
        std::remove(spec.attributes.begin(), spec.attributes.end(), column),
        spec.attributes.end());
    spec.textAttributes = attributeColumns(connection, spec.name, true);
    spec.textAttributes.erase(std::remove(spec.textAttributes.begin(),
                                          spec.textAttributes.end(), column),
                              spec.textAttributes.end());
    m_hasAttributes = m_hasAttributes || !spec.attributes.empty() ||
                      !spec.textAttributes.empty();
    QString select = QStringLiteral("SELECT \"%1\"").arg(column);
    for (const QString& attribute : spec.attributes) {
      select += QStringLiteral(", \"%1\"").arg(attribute);
    }
    for (const QString& attribute : spec.textAttributes) {
      select += QStringLiteral(", \"%1\"").arg(attribute);
    }
    select += QStringLiteral(" FROM \"%1\" WHERE tile_level = ? AND "
                             "tile_x = ? AND tile_y = ?")
                  .arg(spec.name);
    std::unique_ptr<Table> table = std::make_unique<Table>();
    table->spec = spec;
    table->pool =
        std::make_unique<SqliteStatementPool>(m_path, select.toUtf8());
    m_maximumLevel = std::max(m_maximumLevel, spec.storageLevel);
    m_tables.push_back(std::move(table));
  }
  sqlite3_finalize(statement);
  if (m_tables.empty()) {
    m_error = QStringLiteral("No readable feature tables in %1").arg(m_path);
    return;
  }
  m_valid = true;
}

GeoPackageFeatureProvider::~GeoPackageFeatureProvider() = default;

std::vector<FeatureTableSpec> GeoPackageFeatureProvider::tables() const
{
  std::vector<FeatureTableSpec> specs;
  specs.reserve(m_tables.size());
  for (const std::unique_ptr<Table>& table : m_tables) {
    specs.push_back(table->spec);
  }
  return specs;
}

QString GeoPackageFeatureProvider::name() const
{
  return QFileInfo(m_path).completeBaseName();
}

QString GeoPackageFeatureProvider::id() const
{
  return QStringLiteral("geopackage:") + m_path; // ts-notr: identifier
}

TileKey GeoPackageFeatureProvider::storedKey(TileKey key) const
{
  for (const std::unique_ptr<Table>& table : m_tables) {
    const FeatureTableSpec& spec = table->spec;
    if (key.level >= spec.minLevel &&
        (spec.maxLevel < 0 || key.level <= spec.maxLevel)) {
      TileKey stored = key;
      while (stored.level > spec.storageLevel) {
        stored = stored.parent();
      }
      return stored;
    }
  }
  return key;
}

std::optional<FeatureTile>
GeoPackageFeatureProvider::loadFeatures(TileKey key,
                                        const std::atomic<bool>& cancelled)
{
  if (!m_valid || !key.isValid()) {
    return std::nullopt;
  }
  std::optional<FeatureTile> tile;
  for (const std::unique_ptr<Table>& table : m_tables) {
    const FeatureTableSpec& spec = table->spec;
    if (key.level < spec.minLevel ||
        (spec.maxLevel >= 0 && key.level > spec.maxLevel)) {
      continue;
    }
    TileKey stored = key;
    while (stored.level > spec.storageLevel) {
      stored = stored.parent();
    }
    if (tile && tile->key != stored) {
      continue; // Tables of one level share a storage level.
    }
    const SqliteStatementPool::Lease lease = table->pool->acquire();
    sqlite3_stmt* statement = lease.statement();
    if (statement == nullptr) {
      continue;
    }
    if (!tile) {
      tile = FeatureTile{stored, {}};
    }
    sqlite3_bind_int(statement, 1, stored.level);
    sqlite3_bind_int(statement, 2, stored.x);
    sqlite3_bind_int(statement, 3, stored.y);
    if (!readRows(statement, spec, cancelled, &tile->features)) {
      return std::nullopt;
    }
  }
  if (tile) {
    if (!m_rules.empty()) {
      applyStyleRules(tile->features, m_rules);
    }
    keep(*tile);
  }
  return tile;
}

void GeoPackageFeatureProvider::keep(const FeatureTile& tile)
{
  if (!m_hasAttributes || m_options.keptTiles <= 0) {
    return;
  }
  const std::lock_guard<std::mutex> lock(m_keptMutex);
  for (std::deque<FeatureTile>::iterator it = m_kept.begin();
       it != m_kept.end(); ++it) {
    if (it->key == tile.key) {
      m_kept.erase(it);
      break;
    }
  }
  m_kept.push_back(tile);
  while (static_cast<int32_t>(m_kept.size()) > m_options.keptTiles) {
    m_kept.pop_front();
  }
}

std::vector<Feature>
GeoPackageFeatureProvider::featuresAt(GeoPoint point, double toleranceDeg) const
{
  const std::lock_guard<std::mutex> lock(m_keptMutex);
  std::vector<const FeatureTile*> tiles;
  for (const FeatureTile& tile : m_kept) {
    const GeoRect rect = tile.key.bounds();
    if (point.lonDeg >= rect.westDeg - toleranceDeg &&
        point.lonDeg <= rect.eastDeg + toleranceDeg &&
        point.latDeg >= rect.southDeg - toleranceDeg &&
        point.latDeg <= rect.northDeg + toleranceDeg) {
      tiles.push_back(&tile);
    }
  }
  // The finest tiles first: their features are the most detailed.
  std::stable_sort(tiles.begin(), tiles.end(),
                   [](const FeatureTile* a, const FeatureTile* b) {
                     return a->key.level > b->key.level;
                   });
  std::vector<Feature> found;
  std::set<QString> seen;
  for (const FeatureTile* tile : tiles) {
    for (const Feature& feature : tile->features) {
      if (!feature.id.isEmpty() && seen.count(feature.id) != 0) {
        continue;
      }
      if (featureUnder(feature, point, toleranceDeg)) {
        found.push_back(feature);
        if (!feature.id.isEmpty()) {
          seen.insert(feature.id);
        }
      }
    }
  }
  return found;
}

std::vector<Feature>
GeoPackageFeatureProvider::labelPoints(const QString& attribute,
                                       const std::vector<TileKey>& tiles) const
{
  const std::lock_guard<std::mutex> lock(m_keptMutex);
  std::vector<Feature> points;
  for (const FeatureTile& tile : m_kept) {
    if (std::find(tiles.begin(), tiles.end(), tile.key) == tiles.end()) {
      continue;
    }
    for (const Feature& feature : tile.features) {
      if (feature.type != Feature::Type::Points ||
          !feature.attributes.contains(attribute)) {
        continue;
      }
      for (const std::vector<GeoPoint>& part : feature.parts) {
        Feature point = feature;
        point.parts = {part};
        points.push_back(std::move(point));
      }
    }
  }
  return points;
}

} // namespace ts
