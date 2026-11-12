// SPDX-License-Identifier: GPL-2.0-or-later
#include "countrybuilder.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <QByteArray>

#define GEOS_USE_ONLY_R_API
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <geos_c.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include "ts/deps/dependencies.h"
#include "ts/deps/gdaldataset.h"
#include "ts/deps/geoscontext.h"

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

QString lastGdalError()
{
  return QString::fromUtf8(CPLGetLastErrorMsg());
}

/** GEOS geometries and the context they belong to. */
class Geos
{
public:
  struct Deleter {
    GEOSContextHandle_t context = nullptr;
    void operator()(GEOSGeometry* geometry) const
    {
      if (geometry != nullptr) {
        GEOSGeom_destroy_r(context, geometry);
      }
    }
  };
  using Ptr = std::unique_ptr<GEOSGeometry, Deleter>;

  Geos()
      : m_handle(static_cast<GEOSContextHandle_t>(m_context.get())),
        m_reader(GEOSWKBReader_create_r(m_handle)),
        m_writer(GEOSWKBWriter_create_r(m_handle))
  {
  }
  ~Geos()
  {
    GEOSWKBReader_destroy_r(m_handle, m_reader);
    GEOSWKBWriter_destroy_r(m_handle, m_writer);
  }
  Geos(const Geos&) = delete;
  Geos& operator=(const Geos&) = delete;
  Geos(Geos&&) = delete;
  Geos& operator=(Geos&&) = delete;

  GEOSContextHandle_t handle() const { return m_handle; }
  QString lastError() const { return m_context.lastError(); }

  Ptr wrap(GEOSGeometry* geometry) const
  {
    return Ptr(geometry, Deleter{m_handle});
  }

  Ptr fromOgr(OGRGeometryH geometry) const
  {
    std::vector<unsigned char> wkb(
        static_cast<std::size_t>(OGR_G_WkbSize(geometry)));
    OGR_G_ExportToIsoWkb(geometry, wkbNDR, wkb.data());
    return wrap(
        GEOSWKBReader_read_r(m_handle, m_reader, wkb.data(), wkb.size()));
  }

  /** A new OGR multipolygon of @p geometry, or null. */
  OGRGeometryH toOgr(const GEOSGeometry* geometry) const
  {
    std::size_t size = 0;
    unsigned char* data =
        GEOSWKBWriter_write_r(m_handle, m_writer, geometry, &size);
    if (data == nullptr) {
      return nullptr;
    }
    OGRGeometryH result = nullptr;
    const OGRErr error =
        OGR_G_CreateFromWkb(data, nullptr, &result, static_cast<int>(size));
    GEOSFree_r(m_handle, data);
    if (error != OGRERR_NONE || result == nullptr) {
      return nullptr;
    }
    return OGR_G_ForceToMultiPolygon(result);
  }

private:
  GeosContext m_context;
  GEOSContextHandle_t m_handle;
  GEOSWKBReader* m_reader;
  GEOSWKBWriter* m_writer;
};

/**
 * Moves the western parts of a country that reaches both sides of the
 * antimeridian (Russia, Fiji, the United States) by 360°, so that its
 * parts on either side join up.
 */
void joinAcrossAntimeridian(OGRGeometryH geometry)
{
  OGREnvelope envelope{};
  OGR_G_GetEnvelope(geometry, &envelope);
  if (envelope.MinX > -179.99 || envelope.MaxX < 179.99) {
    return;
  }
  const bool multi =
      wkbFlatten(OGR_G_GetGeometryType(geometry)) == wkbMultiPolygon;
  const int parts = multi ? OGR_G_GetGeometryCount(geometry) : 1;
  for (int p = 0; p < parts; ++p) {
    OGRGeometryH part = multi ? OGR_G_GetGeometryRef(geometry, p) : geometry;
    OGREnvelope partEnvelope{};
    OGR_G_GetEnvelope(part, &partEnvelope);
    if (partEnvelope.MinX + partEnvelope.MaxX >= 0.0) {
      continue;
    }
    for (int r = 0; r < OGR_G_GetGeometryCount(part); ++r) {
      OGRGeometryH ring = OGR_G_GetGeometryRef(part, r);
      for (int i = 0; i < OGR_G_GetPointCount(ring); ++i) {
        OGR_G_SetPoint_2D(ring, i, OGR_G_GetX(ring, i) + 360.0,
                          OGR_G_GetY(ring, i));
      }
    }
  }
}

long long ringCount(GEOSContextHandle_t context, const GEOSGeometry* geometry)
{
  long long rings = 0;
  const int count = GEOSGetNumGeometries_r(context, geometry);
  for (int i = 0; i < count; ++i) {
    rings += 1 + GEOSGetNumInteriorRings_r(
                     context, GEOSGetGeometryN_r(context, geometry, i));
  }
  return rings;
}

/** The text of field @p index, empty for Natural Earth's "-99". */
QString field(OGRFeatureH feature, int index)
{
  const QString value =
      QString::fromUtf8(OGR_F_GetFieldAsString(feature, index)).trimmed();
  return value == QLatin1String("-99") ? QString() : value;
}

/** The output GeoPackage. */
class Writer
{
public:
  explicit Writer(const QString& path)
  {
    GDALDriverH driver = GDALGetDriverByName("GPKG");
    if (driver == nullptr) {
      m_error = QStringLiteral("GDAL has no GeoPackage driver");
      return;
    }
    const QByteArray encoded = path.toUtf8();
    m_dataset =
        GDALCreate(driver, encoded.constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (m_dataset == nullptr) {
      m_error = lastGdalError();
      return;
    }
    // WGS 84 longitude/latitude, as GeoPackageFeatureWriter writes it.
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRSetWellKnownGeogCS(srs, "WGS84");
    OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
    char** options = nullptr;
    options = CSLSetNameValue(options, "GEOMETRY_NAME", "geom");
    options = CSLSetNameValue(options, "FID", "fid");
    options = CSLSetNameValue(options, "SPATIAL_INDEX", "NO");
    m_layer = GDALDatasetCreateLayer(m_dataset, "countries", srs,
                                     wkbMultiPolygon, options);
    CSLDestroy(options);
    OSRRelease(srs);
    if (m_layer == nullptr) {
      m_error = lastGdalError();
      return;
    }
    for (const char* name : {"id", "name", "iso_a2", "iso_a3"}) {
      OGRFieldDefnH definition = OGR_Fld_Create(name, OFTString);
      const OGRErr error = OGR_L_CreateField(m_layer, definition, TRUE);
      OGR_Fld_Destroy(definition);
      if (error != OGRERR_NONE) {
        m_error = lastGdalError();
        return;
      }
    }
    GDALDatasetStartTransaction(m_dataset, FALSE);
  }
  ~Writer()
  {
    if (m_dataset != nullptr) {
      GDALClose(m_dataset);
    }
  }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;

  const QString& errorString() const { return m_error; }

  /** Adds a row; takes @p geometry. */
  bool add(const std::array<QString, 4>& fields, OGRGeometryH geometry)
  {
    OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(m_layer));
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const QByteArray value = fields.at(i).toUtf8();
      OGR_F_SetFieldString(feature, static_cast<int>(i), value.constData());
    }
    OGR_F_SetGeometryDirectly(feature, geometry);
    const bool ok = OGR_L_CreateFeature(m_layer, feature) == OGRERR_NONE;
    OGR_F_Destroy(feature);
    if (!ok) {
      m_error = lastGdalError();
    }
    return ok;
  }

  bool finish()
  {
    if (GDALDatasetCommitTransaction(m_dataset) != OGRERR_NONE) {
      m_error = lastGdalError();
      return false;
    }
    GDALClose(m_dataset);
    m_dataset = nullptr;
    return true;
  }

private:
  GDALDatasetH m_dataset = nullptr;
  OGRLayerH m_layer = nullptr;
  QString m_error;
};

/**
 * The outline written for a country of @p geometry (joined across the
 * antimeridian, widened, simplified; checked to cover the country), as a
 * new OGR multipolygon, or null and @p errorString. Counts in @p stats.
 */
OGRGeometryH coveringOutline(const Geos& geos, OGRGeometryH geometry,
                             const CountryOptions& options, const QString& name,
                             CountryStats* stats, QString* errorString)
{
  GEOSContextHandle_t context = geos.handle();
  joinAcrossAntimeridian(geometry);
  Geos::Ptr country = geos.fromOgr(geometry);
  if (country != nullptr && GEOSisValid_r(context, country.get()) != 1) {
    country = geos.wrap(GEOSMakeValid_r(context, country.get()));
  }
  if (country == nullptr) {
    fail(errorString, QStringLiteral("%1: cannot read the outline: %2")
                          .arg(name, geos.lastError()));
    return nullptr;
  }
  stats->sourceVertices += GEOSGetNumCoordinates_r(context, country.get());
  const Geos::Ptr widened =
      geos.wrap(GEOSBuffer_r(context, country.get(), options.bufferDeg, 4));
  const Geos::Ptr outline =
      widened == nullptr ? nullptr
                         : geos.wrap(GEOSTopologyPreserveSimplify_r(
                               context, widened.get(), options.simplifyDeg));
  if (outline == nullptr || GEOSisValid_r(context, outline.get()) != 1) {
    fail(errorString, QStringLiteral("%1: the outline is invalid: %2")
                          .arg(name, geos.lastError()));
    return nullptr;
  }
  if (GEOSCovers_r(context, outline.get(), country.get()) != 1) {
    fail(errorString,
         QStringLiteral("%1: the outline doesn't cover the country").arg(name));
    return nullptr;
  }
  OGRGeometryH written = geos.toOgr(outline.get());
  if (written == nullptr) {
    fail(errorString,
         QStringLiteral("%1: cannot convert the outline").arg(name));
    return nullptr;
  }
  stats->countries += 1;
  stats->rings += ringCount(context, outline.get());
  stats->vertices += GEOSGetNumCoordinates_r(context, outline.get());
  return written;
}

} // namespace

bool buildCountries(const CountryOptions& options, CountryStats* stats,
                    QString* errorString)
{
  ts::initialiseGdal();
  if (!(options.bufferDeg > options.simplifyDeg) ||
      !(options.simplifyDeg >= 0.0)) {
    return fail(errorString, QStringLiteral("The countries' buffer must "
                                            "exceed their simplification"));
  }
  const std::function<void(const QString&)> progress =
      [&options](const QString& message) {
        if (options.progress) {
          options.progress(message);
        }
      };
  const GdalDataset source(options.sourcePath);
  if (!source.isOpen() || source.layerCount() < 1) {
    return fail(errorString,
                QStringLiteral("Cannot read %1: %2")
                    .arg(options.sourcePath, source.errorString()));
  }
  OGRLayerH layer =
      GDALDatasetGetLayer(static_cast<GDALDatasetH>(source.handle()), 0);
  OGRFeatureDefnH definition = OGR_L_GetLayerDefn(layer);
  std::array<int, 4> fields{};
  const std::array<const char*, 4> names{"ADM0_A3", "ADMIN", "ISO_A2_EH",
                                         "ISO_A3_EH"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    fields.at(i) = OGR_FD_GetFieldIndex(definition, names.at(i));
    if (fields.at(i) < 0) {
      return fail(errorString, QStringLiteral("%1 has no field %2")
                                   .arg(options.sourcePath,
                                        QString::fromLatin1(names.at(i))));
    }
  }

  Writer writer(options.outputPath);
  if (!writer.errorString().isEmpty()) {
    return fail(errorString,
                QStringLiteral("Cannot create %1: %2")
                    .arg(options.outputPath, writer.errorString()));
  }
  const Geos geos;
  progress(QStringLiteral("Widening and simplifying the countries"));
  OGR_L_ResetReading(layer);
  for (OGRFeatureH feature = OGR_L_GetNextFeature(layer); feature != nullptr;
       feature = OGR_L_GetNextFeature(layer)) {
    const std::unique_ptr<void, void (*)(void*)> owner(
        feature, [](void* f) { OGR_F_Destroy(static_cast<OGRFeatureH>(f)); });
    const std::array<QString, 4> values{
        field(feature, fields[0]), field(feature, fields[1]),
        field(feature, fields[2]), field(feature, fields[3])};
    if (std::find(options.exclude.begin(), options.exclude.end(), values[0]) !=
        options.exclude.end()) {
      stats->excluded += 1;
      continue;
    }
    OGRGeometryH ogrGeometry = OGR_F_GetGeometryRef(feature);
    if (values[0].isEmpty() || values[1].isEmpty() || ogrGeometry == nullptr) {
      return fail(errorString,
                  QStringLiteral("A country without id, name or outline "
                                 "(%1)")
                      .arg(values[1]));
    }
    OGRGeometryH written = coveringOutline(geos, ogrGeometry, options,
                                           values[1], stats, errorString);
    if (written == nullptr) {
      return false;
    }
    if (!writer.add(values, written)) {
      return fail(errorString, QStringLiteral("Cannot write %1: %2")
                                   .arg(values[1], writer.errorString()));
    }
  }
  if (!writer.finish()) {
    return fail(errorString,
                QStringLiteral("Cannot write %1: %2")
                    .arg(options.outputPath, writer.errorString()));
  }
  progress(QStringLiteral("%1 countries, %2 vertices (from %3)")
               .arg(stats->countries)
               .arg(stats->vertices)
               .arg(stats->sourceVertices));
  return true;
}

} // namespace ts::packager
