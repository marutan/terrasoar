// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/gdalelevationprovider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "ts/deps/dependencies.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/heightgrid.h"

namespace ts
{

namespace
{

constexpr double noData = std::numeric_limits<double>::quiet_NaN();

/** Source pixels to a sample above which a file without overviews is skipped.
 */
constexpr double maximumDecimationWithoutOverviews = 64.0;

/** A GDAL affine geotransform. */
using GeoTransform = std::array<double, 6>;

/** Index of (@p row, @p column) in a grid @p width wide. */
// Row then column, as everywhere in a grid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::size_t gridIndex(int row, int column, int width)
{
  return static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(column);
}

/** What GDAL's progress callback is given: the load's cancel flag. */
struct CancelFlag {
  const std::atomic<bool>* cancelled = nullptr;
};

/** GDAL progress callback that stops a read once the load is cancelled. */
int CPL_STDCALL keepReading(double /*complete*/, const char* /*message*/,
                            void* data)
{
  return static_cast<const CancelFlag*>(data)->cancelled->load() ? FALSE : TRUE;
}

/** Metres in one of the band's vertical units ("ft", "metre", …). */
double metresPerUnit(const char* unit)
{
  const QString name = QString::fromUtf8(unit).trimmed().toLower();
  if (name == QLatin1String("ft") || name == QLatin1String("foot") ||
      name == QLatin1String("feet")) {
    return 0.3048;
  }
  if (name.contains(QLatin1String("us survey foot")) ||
      name == QLatin1String("us-ft")) {
    return 1200.0 / 3937.0;
  }
  return 1.0;
}

/** WGS 84 latitude/longitude, longitude first (as the transforms take). */
OGRSpatialReferenceH newWgs84()
{
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  OSRImportFromEPSG(srs, 4326);
  OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  return srs;
}

/**
 * The horizontal part of @p dataset's CRS, longitude first; the caller
 * destroys it.
 */
OGRSpatialReferenceH horizontalCrs(GDALDatasetH dataset)
{
  OGRSpatialReferenceH srs = OSRClone(GDALGetSpatialRef(dataset));
  if (OSRIsCompound(srs) != 0) {
    OSRStripVertical(srs);
  }
  OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  return srs;
}

/** The pixels of a tile's samples in a raster. */
struct SamplePixels {
  /** The samples' latitudes and longitudes, row by row. */
  std::vector<GeoPoint> points;
  /** Their pixel coordinates; x is NaN for samples outside the raster. */
  std::vector<double> x;
  std::vector<double> y;
  /** The extent of those inside, pixels. */
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  /** True if any sample is inside. */
  bool any = false;
};

/** Raster values averaged over whole factor² blocks of a fixed grid. */
struct Blocks {
  /** Pixels along a block's side (a power of two). */
  int factor = 1;
  /** The first block's column and row in the raster's block grid. */
  int x0 = 0;
  int y0 = 0;
  /** Blocks across and down. */
  int width = 0;
  int height = 0;
  /** The averages, row by row. */
  std::vector<double> values;
};

} // namespace

QString demVerticalDatumId(DemVerticalDatum datum)
{
  switch (datum) {
  case DemVerticalDatum::Ellipsoid:
    return QStringLiteral("ellipsoid");
  case DemVerticalDatum::Egm2008:
    return QStringLiteral("egm2008");
  case DemVerticalDatum::Egm96:
    return QStringLiteral("egm96");
  case DemVerticalDatum::Unknown:
    break;
  }
  return QString();
}

DemVerticalDatum detectDemVerticalDatum(const QString& path)
{
  initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr);
  if (dataset == nullptr) {
    return DemVerticalDatum::Unknown;
  }
  DemVerticalDatum datum = DemVerticalDatum::Unknown;
  OGRSpatialReferenceH srs = GDALGetSpatialRef(dataset);
  if (srs != nullptr) {
    char* wkt = nullptr;
    const std::array<const char*, 2> options = {"FORMAT=WKT2_2019", nullptr};
    if (OSRExportToWktEx(srs, &wkt, options.data()) == OGRERR_NONE &&
        wkt != nullptr) {
      const QString text = QString::fromUtf8(wkt).toUpper();
      if (text.contains(QLatin1String("EGM2008"))) {
        datum = DemVerticalDatum::Egm2008;
      } else if (text.contains(QLatin1String("EGM96"))) {
        datum = DemVerticalDatum::Egm96;
      } else if (OSRGetAxesCount(srs) == 3 && OSRIsGeographic(srs) != 0 &&
                 OSRIsCompound(srs) == 0) {
        // A 3D geographic CRS such as EPSG:4979: ellipsoidal heights.
        datum = DemVerticalDatum::Ellipsoid;
      }
    }
    CPLFree(wkt);
  }
  GDALClose(dataset);
  return datum;
}

/** One open dataset and its transform, used by one thread at a time. */
struct GdalElevationProvider::Reader {
  GDALDatasetH dataset = nullptr;
  GDALRasterBandH band = nullptr;
  OGRCoordinateTransformationH toRaster = nullptr;
  /** Geographic raster: longitudes may need ±360° to land inside. */
  bool geographic = false;
  GeoTransform transform{};
  GeoTransform inverse{};
  int width = 0;
  int height = 0;
  double noDataValue = 0.0;
  bool hasNoData = false;
  double scale = 1.0;
  double offset = 0.0;

  Reader() = default;
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;
  ~Reader()
  {
    if (toRaster != nullptr) {
      OCTDestroyCoordinateTransformation(toRaster);
    }
    if (dataset != nullptr) {
      GDALClose(dataset);
    }
  }

  /** Opens @p path, or returns null and @p error. */
  static std::unique_ptr<Reader> open(const QString& path, QString* error)
  {
    initialiseGdal();
    std::unique_ptr<Reader> reader = std::make_unique<Reader>();
    const QByteArray encoded = path.toUtf8();
    CPLErrorReset();
    reader->dataset =
        GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr);
    if (reader->dataset == nullptr) {
      *error = QString::fromUtf8(CPLGetLastErrorMsg());
      return nullptr;
    }
    if (GDALGetRasterCount(reader->dataset) < 1) {
      *error = QCoreApplication::translate("ts::GdalElevationProvider",
                                           "The file has no raster band.");
      return nullptr;
    }
    reader->band = GDALGetRasterBand(reader->dataset, 1);
    reader->width = GDALGetRasterXSize(reader->dataset);
    reader->height = GDALGetRasterYSize(reader->dataset);
    if (GDALGetGeoTransform(reader->dataset, reader->transform.data()) !=
            CE_None ||
        GDALGetSpatialRef(reader->dataset) == nullptr ||
        GDALInvGeoTransform(reader->transform.data(), reader->inverse.data()) ==
            0) {
      *error = QCoreApplication::translate(
          "ts::GdalElevationProvider",
          "The file has no georeferencing (coordinate reference system "
          "and position).");
      return nullptr;
    }
    // Only the horizontal part: the heights are converted separately.
    OGRSpatialReferenceH horizontal = horizontalCrs(reader->dataset);
    reader->geographic = OSRIsGeographic(horizontal) != 0;
    OGRSpatialReferenceH wgs84 = newWgs84();
    reader->toRaster = OCTNewCoordinateTransformation(wgs84, horizontal);
    OSRDestroySpatialReference(wgs84);
    OSRDestroySpatialReference(horizontal);
    if (reader->toRaster == nullptr) {
      *error = QCoreApplication::translate(
          "ts::GdalElevationProvider",
          "The file's coordinate reference system can't be converted to "
          "latitude and longitude.");
      return nullptr;
    }
    int has = 0;
    reader->noDataValue = GDALGetRasterNoDataValue(reader->band, &has);
    reader->hasNoData = has != 0;
    const double unit = metresPerUnit(GDALGetRasterUnitType(reader->band));
    int hasScale = 0;
    const double scale = GDALGetRasterScale(reader->band, &hasScale);
    int hasOffset = 0;
    const double offset = GDALGetRasterOffset(reader->band, &hasOffset);
    reader->scale = (hasScale != 0 ? scale : 1.0) * unit;
    reader->offset = (hasOffset != 0 ? offset : 0.0) * unit;
    return reader;
  }

  /**
   * Turns longitudes @p x and latitudes @p y into raster pixel
   * coordinates (continuous, pixel corners at integers), NaN where they
   * can't be.
   */
  void toPixels(std::vector<double>& x, std::vector<double>& y) const
  {
    const std::size_t count = x.size();
    std::vector<int> ok(count, 0);
    OCTTransformEx(toRaster, static_cast<int>(count), x.data(), y.data(),
                   nullptr, ok.data());
    for (std::size_t i = 0; i < count; ++i) {
      if (ok[i] == 0 || !std::isfinite(x[i]) || !std::isfinite(y[i])) {
        x[i] = noData;
        y[i] = noData;
        continue;
      }
      const double px = inverse[0] + inverse[1] * x[i] + inverse[2] * y[i];
      const double py = inverse[3] + inverse[4] * x[i] + inverse[5] * y[i];
      x[i] = px;
      y[i] = py;
      if (geographic && (px < 0.0 || px > width)) {
        // A raster in 0°…360° (or beyond 180°): try the longitude a turn on.
        for (const double turn : {360.0, -360.0}) {
          const double shifted = px + inverse[1] * turn;
          if (shifted >= 0.0 && shifted <= width) {
            x[i] = shifted;
            y[i] = py + inverse[4] * turn;
            break;
          }
        }
      }
    }
  }

  /** The pixels of @p key's samples. */
  SamplePixels samplePixels(TileKey key) const
  {
    SamplePixels pixels;
    const int n = GdalElevationProvider::samples;
    const std::size_t count = gridIndex(n, 0, n);
    pixels.points.resize(count);
    pixels.x.resize(count);
    pixels.y.resize(count);
    for (int row = 0; row < n; ++row) {
      for (int column = 0; column < n; ++column) {
        const std::size_t i = gridIndex(row, column, n);
        pixels.points[i] = heightSamplePoint(key, n, row, column);
        pixels.x[i] = pixels.points[i].lonDeg;
        pixels.y[i] = pixels.points[i].latDeg;
      }
    }
    toPixels(pixels.x, pixels.y);
    for (std::size_t i = 0; i < count; ++i) {
      const double px = pixels.x[i];
      const double py = pixels.y[i];
      if (std::isnan(px) || px < 0.0 || py < 0.0 || px > width || py > height) {
        pixels.x[i] = noData;
        continue;
      }
      pixels.any = true;
      pixels.minX = std::min(pixels.minX, px);
      pixels.maxX = std::max(pixels.maxX, px);
      pixels.minY = std::min(pixels.minY, py);
      pixels.maxY = std::max(pixels.maxY, py);
    }
    return pixels;
  }

  /** Raster pixels between neighbouring samples of a tile @p bounds. */
  double pixelsPerSample(const GeoRect& bounds,
                         const SamplePixels& pixels) const
  {
    // At the tile's middle, one sample spacing east and north.
    const double step =
        bounds.widthDeg() / (GdalElevationProvider::samples - 1);
    const double lat = bounds.centreLatDeg();
    const double lon = bounds.centreLonDeg();
    std::vector<double> x = {lon, lon + step, lon};
    std::vector<double> y = {lat, lat, lat + step};
    toPixels(x, y);
    if (!std::isnan(x[0]) && !std::isnan(x[1]) && !std::isnan(x[2])) {
      return std::min(std::hypot(x[1] - x[0], y[1] - y[0]),
                      std::hypot(x[2] - x[0], y[2] - y[0]));
    }
    return std::min(pixels.maxX - pixels.minX, pixels.maxY - pixels.minY) /
           (GdalElevationProvider::samples - 1);
  }

  /**
   * Reads the blocks of @p factor² pixels around @p pixels, or nothing if
   * the raster has fewer pixels than a block or the read failed or was
   * cancelled.
   */
  std::optional<Blocks> readBlocks(const SamplePixels& pixels, int factor,
                                   const std::atomic<bool>& cancelled) const
  {
    Blocks blocks;
    blocks.factor = factor;
    const int columns = width / factor;
    const int rows = height / factor;
    if (columns < 1 || rows < 1) {
      return std::nullopt;
    }
    blocks.x0 = std::clamp(
        static_cast<int>(std::floor(pixels.minX / factor)) - 1, 0, columns - 1);
    blocks.y0 = std::clamp(
        static_cast<int>(std::floor(pixels.minY / factor)) - 1, 0, rows - 1);
    const int x1 =
        std::clamp(static_cast<int>(std::ceil(pixels.maxX / factor)) + 1,
                   blocks.x0 + 1, columns);
    const int y1 =
        std::clamp(static_cast<int>(std::ceil(pixels.maxY / factor)) + 1,
                   blocks.y0 + 1, rows);
    blocks.width = x1 - blocks.x0;
    blocks.height = y1 - blocks.y0;
    blocks.values.resize(gridIndex(blocks.height, 0, blocks.width));
    CancelFlag flag{&cancelled};
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = factor > 1 ? GRIORA_Average : GRIORA_NearestNeighbour;
    extra.pfnProgress = keepReading;
    extra.pProgressData = &flag;
    CPLErrorReset();
    if (GDALRasterIOEx(band, GF_Read, blocks.x0 * factor, blocks.y0 * factor,
                       blocks.width * factor, blocks.height * factor,
                       blocks.values.data(), blocks.width, blocks.height,
                       GDT_Float64, 0, 0, &extra) != CE_None ||
        cancelled.load()) {
      return std::nullopt;
    }
    return blocks;
  }

  /** Returns true if @p value is a height (not no-data). */
  bool isHeight(double value) const
  {
    return std::isfinite(value) && (!hasNoData || value != noDataValue);
  }

  /**
   * The height at pixel (@p px, @p py), bilinear between the centres of
   * the valid ones of @p blocks, or NaN if none near it is valid.
   */
  double interpolate(const Blocks& blocks, double px, double py) const
  {
    const double u = std::clamp(px / blocks.factor - blocks.x0 - 0.5, 0.0,
                                static_cast<double>(blocks.width - 1));
    const double v = std::clamp(py / blocks.factor - blocks.y0 - 0.5, 0.0,
                                static_cast<double>(blocks.height - 1));
    const int column = std::min(static_cast<int>(u), blocks.width - 1);
    const int row = std::min(static_cast<int>(v), blocks.height - 1);
    const double fu = u - column;
    const double fv = v - row;
    double sum = 0.0;
    double weights = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
      for (int dx = 0; dx <= 1; ++dx) {
        const double weight =
            (dx == 1 ? fu : 1.0 - fu) * (dy == 1 ? fv : 1.0 - fv);
        const double value = blocks.values[gridIndex(
            std::min(row + dy, blocks.height - 1),
            std::min(column + dx, blocks.width - 1), blocks.width)];
        if (weight > 0.0 && isHeight(value)) {
          sum += weight * value;
          weights += weight;
        }
      }
    }
    return weights > 1e-9 ? sum / weights * scale + offset : noData;
  }
};

GdalElevationProvider::GdalElevationProvider(
    QString path, QString datumId, std::shared_ptr<const GeoidModel> datumGeoid,
    std::shared_ptr<const GeoidModel> seaLevelGeoid)
    : m_path(std::move(path)), m_datumId(std::move(datumId)),
      m_datumGeoid(std::move(datumGeoid)),
      m_seaLevelGeoid(std::move(seaLevelGeoid))
{
  std::unique_ptr<Reader> reader = Reader::open(m_path, &m_error);
  if (!reader) {
    if (m_error.isEmpty()) {
      m_error = QCoreApplication::translate("ts::GdalElevationProvider",
                                            "The file can't be read.");
    }
    return;
  }
  m_hasOverviews = GDALGetOverviewCount(reader->band) > 0;
  const std::optional<GeoRect> bounds = extentOf(*reader);
  if (!bounds) {
    m_error = QCoreApplication::translate(
        "ts::GdalElevationProvider",
        "The file's extent can't be turned into latitude and longitude.");
    return;
  }
  m_bounds = *bounds;
  // The level whose samples are as far apart as the raster's pixels.
  const double pixelDeg =
      std::max(m_bounds.heightDeg(), 1e-9) / std::max(reader->height, 1);
  const double level = std::log2(TileKey::sizeDeg(0) /
                                 (static_cast<double>(samples - 1) * pixelDeg));
  m_maximumLevel = std::clamp(static_cast<int>(std::ceil(level - 1e-6)), 0,
                              TileKey::maximumLevel);
  m_readers.push_back(std::move(reader));
}

GdalElevationProvider::~GdalElevationProvider() = default;

std::optional<GeoRect> GdalElevationProvider::extentOf(const Reader& reader)
{
  // The raster's edges turned into latitude and longitude.
  OGRSpatialReferenceH raster = horizontalCrs(reader.dataset);
  OGRSpatialReferenceH wgs84 = newWgs84();
  OGRCoordinateTransformationH toWgs84 =
      OCTNewCoordinateTransformation(raster, wgs84);
  OSRDestroySpatialReference(wgs84);
  OSRDestroySpatialReference(raster);
  if (toWgs84 == nullptr) {
    return std::nullopt;
  }
  constexpr int steps = 32;
  const GeoTransform& t = reader.transform;
  std::vector<double> x;
  std::vector<double> y;
  for (int i = 0; i <= steps; ++i) {
    const double f = static_cast<double>(i) / steps;
    const std::array<std::pair<double, double>, 4> edges = {
        {{f * reader.width, 0.0},
         {f * reader.width, static_cast<double>(reader.height)},
         {0.0, f * reader.height},
         {static_cast<double>(reader.width), f * reader.height}}};
    for (const std::pair<double, double>& pixel : edges) {
      x.push_back(t[0] + t[1] * pixel.first + t[2] * pixel.second);
      y.push_back(t[3] + t[4] * pixel.first + t[5] * pixel.second);
    }
  }
  std::vector<int> ok(x.size(), 0);
  OCTTransformEx(toWgs84, static_cast<int>(x.size()), x.data(), y.data(),
                 nullptr, ok.data());
  OCTDestroyCoordinateTransformation(toWgs84);
  GeoRect bounds{180.0, 90.0, -180.0, -90.0};
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (ok[i] != 0 && std::isfinite(x[i]) && std::isfinite(y[i])) {
      bounds.westDeg = std::min(bounds.westDeg, x[i]);
      bounds.eastDeg = std::max(bounds.eastDeg, x[i]);
      bounds.southDeg = std::min(bounds.southDeg, y[i]);
      bounds.northDeg = std::max(bounds.northDeg, y[i]);
    }
  }
  if (bounds.westDeg > bounds.eastDeg || bounds.southDeg > bounds.northDeg) {
    return std::nullopt;
  }
  bounds.southDeg = std::max(bounds.southDeg, -90.0);
  bounds.northDeg = std::min(bounds.northDeg, 90.0);
  if (bounds.westDeg < -180.0 || bounds.eastDeg > 180.0) {
    // Over the antimeridian or in 0°…360°: take every longitude.
    bounds.westDeg = -180.0;
    bounds.eastDeg = 180.0;
  }
  return bounds;
}

QString GdalElevationProvider::id() const
{
  // ts-notr: identifier
  return QStringLiteral("gdalheights:%1|%2").arg(m_path, m_datumId);
}

QString GdalElevationProvider::name() const
{
  return QFileInfo(m_path).fileName();
}

std::unique_ptr<GdalElevationProvider::Reader>
GdalElevationProvider::takeReader()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_readers.empty()) {
      std::unique_ptr<Reader> reader = std::move(m_readers.back());
      m_readers.pop_back();
      return reader;
    }
  }
  QString error;
  return Reader::open(m_path, &error);
}

void GdalElevationProvider::giveBack(std::unique_ptr<Reader> reader)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_readers.push_back(std::move(reader));
}

std::optional<HeightTile>
GdalElevationProvider::loadHeights(TileKey key,
                                   const std::atomic<bool>& cancelled)
{
  if (!isValid() || !key.isValid()) {
    return std::nullopt;
  }
  const GeoRect tileBounds = key.bounds();
  if (tileBounds.eastDeg < m_bounds.westDeg ||
      tileBounds.westDeg > m_bounds.eastDeg ||
      tileBounds.northDeg < m_bounds.southDeg ||
      tileBounds.southDeg > m_bounds.northDeg) {
    return std::nullopt;
  }
  std::unique_ptr<Reader> reader = takeReader();
  if (!reader) {
    return std::nullopt;
  }
  std::optional<HeightTile> tile = read(*reader, key, cancelled);
  giveBack(std::move(reader));
  return tile;
}

std::optional<HeightTile>
GdalElevationProvider::read(const Reader& reader, TileKey key,
                            const std::atomic<bool>& cancelled) const
{
  const SamplePixels pixels = reader.samplePixels(key);
  if (!pixels.any) {
    return std::nullopt;
  }
  const double spacing = reader.pixelsPerSample(key.bounds(), pixels);
  if (!m_hasOverviews && spacing > maximumDecimationWithoutOverviews) {
    return std::nullopt;
  }
  // Whole blocks of factor² pixels from a grid fixed to the raster, so
  // that every tile averages the same blocks.
  int factor = 1;
  while (factor * 2 <= spacing && factor < (1 << 20)) {
    factor *= 2;
  }
  const std::optional<Blocks> blocks =
      reader.readBlocks(pixels, factor, cancelled);
  if (!blocks) {
    return std::nullopt;
  }
  const Blocks& read = *blocks;
  HeightTile tile;
  tile.key = key;
  tile.samples = samples;
  tile.heightsM.assign(pixels.points.size(), static_cast<float>(noData));
  bool found = false;
  for (std::size_t i = 0; i < pixels.points.size(); ++i) {
    if (!std::isnan(pixels.x[i])) {
      const double height = reader.interpolate(read, pixels.x[i], pixels.y[i]);
      tile.heightsM[i] = static_cast<float>(height);
      found = found || !std::isnan(height);
    }
  }
  if (!found) {
    return std::nullopt;
  }
  applyVerticalDatum(tile, pixels.points, m_datumGeoid.get(),
                     m_seaLevelGeoid.get());
  return tile;
}

} // namespace ts
