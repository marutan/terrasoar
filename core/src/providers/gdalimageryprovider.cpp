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
#include "ts/providers/gdalimageryprovider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStringList>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "ts/deps/dependencies.h"

namespace ts
{

// Not anonymous: Part, declared in the header, holds Readers.
namespace gdalimagery
{

/** A GDAL affine geotransform. */
using GeoTransform = std::array<double, 6>;

/** Metres in a degree of latitude, near enough to size pixels. */
constexpr double metresPerDegree = 111320.0;

/** The largest window read for one tile, pixels along a side. */
constexpr int32_t maximumReadPx = 1024;

/** Samples along each edge when finding a part's extent. */
constexpr int32_t edgeSamples = 32;

/** WGS 84 latitude/longitude, longitude first (as the transforms take). */
OGRSpatialReferenceH newWgs84()
{
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  OSRImportFromEPSG(srs, 4326);
  OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
  return srs;
}

/** A premultiplied RGBA colour, components 0 to 1. */
struct Rgba {
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
  float a = 0.0F;
};

/** Premultiplies 8-bit components. */
// Red, green, blue, alpha, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Rgba premultiplied(int32_t r, int32_t g, int32_t b, int32_t a)
{
  const float alpha = static_cast<float>(a) / 255.0F;
  return Rgba{static_cast<float>(r) / 255.0F * alpha,
              static_cast<float>(g) / 255.0F * alpha,
              static_cast<float>(b) / 255.0F * alpha, alpha};
}

/** Rounds a 0-1 component to 8 bits. */
uchar toByte(float value)
{
  return static_cast<uchar>(
      std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

/**
 * Between four corners: @p topLeft to @p topRight by @p fu, and the top to
 * the bottom by @p fv.
 */
float bilinear(float topLeft, float topRight, float bottomLeft,
               float bottomRight, float fu, float fv)
{
  return (topLeft * (1.0F - fu) + topRight * fu) * (1.0F - fv) +
         (bottomLeft * (1.0F - fu) + bottomRight * fu) * fv;
}

/** The pixels of a window read from a part, expanded to RGBA. */
struct Window {
  /** The window's first column and row in the raster, and its size. */
  double x0 = 0.0;
  double y0 = 0.0;
  double widthPx = 0.0;
  double heightPx = 0.0;
  /** The buffer's size: smaller than the window when decimated. */
  int32_t bufferWidth = 0;
  int32_t bufferHeight = 0;
  /** The buffer's pixels, row by row. */
  std::vector<Rgba> pixels;

  /** The pixel at (@p column, @p row) of the buffer, clamped. */
  // Column then row, as x then y.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const Rgba& at(int32_t column, int32_t row) const
  {
    const int32_t c = std::clamp(column, 0, bufferWidth - 1);
    const int32_t r = std::clamp(row, 0, bufferHeight - 1);
    return pixels.at(static_cast<std::size_t>(r) *
                         static_cast<std::size_t>(bufferWidth) +
                     static_cast<std::size_t>(c));
  }

  /**
   * The colour at raster pixel coordinates (@p x, @p y) (pixel corners at
   * integers), bilinear between the buffer's pixel centres.
   */
  // x then y.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Rgba sample(double x, double y) const
  {
    const double u = (x - x0) / widthPx * bufferWidth - 0.5;
    const double v = (y - y0) / heightPx * bufferHeight - 0.5;
    const double floorU = std::floor(u);
    const double floorV = std::floor(v);
    const float fu = static_cast<float>(u - floorU);
    const float fv = static_cast<float>(v - floorV);
    const int32_t c = static_cast<int32_t>(floorU);
    const int32_t r = static_cast<int32_t>(floorV);
    const Rgba& p00 = at(c, r);
    const Rgba& p10 = at(c + 1, r);
    const Rgba& p01 = at(c, r + 1);
    const Rgba& p11 = at(c + 1, r + 1);
    return Rgba{bilinear(p00.r, p10.r, p01.r, p11.r, fu, fv),
                bilinear(p00.g, p10.g, p01.g, p11.g, fu, fv),
                bilinear(p00.b, p10.b, p01.b, p11.b, fu, fv),
                bilinear(p00.a, p10.a, p01.a, p11.a, fu, fv)};
  }
};

/** One open dataset of a part and its transform, for one thread at a time. */
struct Reader {
  GDALDatasetH dataset = nullptr;
  OGRCoordinateTransformationH toRaster = nullptr;
  /** Geographic raster: longitudes may need ±360° to land inside. */
  bool geographic = false;
  GeoTransform transform{};
  GeoTransform inverse{};
  int32_t width = 0;
  int32_t height = 0;
  /** The bands read as red, green, blue and alpha (0: none). */
  std::array<int32_t, 4> bands{};
  /** A paletted raster's colours, premultiplied; empty if not paletted. */
  std::vector<Rgba> palette;
  /** The value of pixels with no data, if the first band has one. */
  bool hasNoData = false;
  double noDataValue = 0.0;

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

  /** Opens @p name, or returns null and @p error. */
  static std::unique_ptr<Reader> open(const QString& name, QString* error);

  /** Chooses the bands and reads the palette. */
  void readBands();

  /**
   * Turns longitudes @p x and latitudes @p y into raster pixel
   * coordinates (continuous, pixel corners at integers), NaN where they
   * can't be or fall outside.
   */
  void toPixels(std::vector<double>& x, std::vector<double>& y) const;

  /** Reads the window [x0, x1) × [y0, y1), at most @p bufferPx across. */
  bool read(Window& window, int32_t bufferWidth, int32_t bufferHeight) const;
};

std::unique_ptr<Reader> Reader::open(const QString& name, QString* error)
{
  initialiseGdal();
  std::unique_ptr<Reader> reader = std::make_unique<Reader>();
  const QByteArray encoded = name.toUtf8();
  CPLErrorReset();
  reader->dataset =
      GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr);
  if (reader->dataset == nullptr) {
    *error = QString::fromUtf8(CPLGetLastErrorMsg());
    return nullptr;
  }
  if (GDALGetRasterCount(reader->dataset) < 1) {
    *error = QStringLiteral("The file has no raster band.");
    return nullptr;
  }
  reader->width = GDALGetRasterXSize(reader->dataset);
  reader->height = GDALGetRasterYSize(reader->dataset);
  OGRSpatialReferenceH crs = GDALGetSpatialRef(reader->dataset);
  if (GDALGetGeoTransform(reader->dataset, reader->transform.data()) !=
          CE_None ||
      crs == nullptr ||
      GDALInvGeoTransform(reader->transform.data(), reader->inverse.data()) ==
          0) {
    *error = QStringLiteral("The file has no georeferencing (coordinate "
                            "reference system and position).");
    return nullptr;
  }
  OGRSpatialReferenceH horizontal = OSRClone(crs);
  OSRSetAxisMappingStrategy(horizontal, OAMS_TRADITIONAL_GIS_ORDER);
  reader->geographic = OSRIsGeographic(horizontal) != 0;
  OGRSpatialReferenceH wgs84 = newWgs84();
  reader->toRaster = OCTNewCoordinateTransformation(wgs84, horizontal);
  OSRDestroySpatialReference(wgs84);
  OSRDestroySpatialReference(horizontal);
  if (reader->toRaster == nullptr) {
    *error = QStringLiteral("The file's coordinate reference system can't "
                            "be converted to latitude and longitude.");
    return nullptr;
  }
  reader->readBands();
  return reader;
}

void Reader::readBands()
{
  const int32_t count = GDALGetRasterCount(dataset);
  GDALRasterBandH first = GDALGetRasterBand(dataset, 1);
  int32_t has = 0;
  noDataValue = GDALGetRasterNoDataValue(first, &has);
  hasNoData = has != 0;
  GDALColorTableH table = GDALGetRasterColorTable(first);
  if (table != nullptr &&
      GDALGetRasterColorInterpretation(first) == GCI_PaletteIndex) {
    const int32_t entries = GDALGetColorEntryCount(table);
    for (int32_t i = 0; i < entries; ++i) {
      GDALColorEntry entry{};
      GDALGetColorEntryAsRGB(table, i, &entry);
      palette.push_back(premultiplied(entry.c1, entry.c2, entry.c3, entry.c4));
    }
    bands = {1, 0, 0, 0};
    return;
  }
  // Grey (and alpha), or red, green, blue (and alpha) by their colour
  // interpretation, else in order.
  bands = count >= 3 ? std::array<int32_t, 4>{1, 2, 3, 0}
                     : std::array<int32_t, 4>{1, 1, 1, 0};
  for (int32_t band = 1; band <= count; ++band) {
    switch (
        GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, band))) {
    case GCI_RedBand:
      bands[0] = band;
      break;
    case GCI_GreenBand:
      bands[1] = band;
      break;
    case GCI_BlueBand:
      bands[2] = band;
      break;
    case GCI_AlphaBand:
      bands[3] = band;
      break;
    default:
      break;
    }
  }
}

void Reader::toPixels(std::vector<double>& x, std::vector<double>& y) const
{
  const std::size_t count = x.size();
  std::vector<int32_t> ok(count, 0);
  // Points beyond a projection's domain are expected (they're outside).
  CPLPushErrorHandler(CPLQuietErrorHandler);
  OCTTransformEx(toRaster, static_cast<int32_t>(count), x.data(), y.data(),
                 nullptr, ok.data());
  CPLPopErrorHandler();
  for (std::size_t i = 0; i < count; ++i) {
    double px = inverse[0] + inverse[1] * x[i] + inverse[2] * y[i];
    double py = inverse[3] + inverse[4] * x[i] + inverse[5] * y[i];
    if (geographic && (px < 0.0 || px > width)) {
      // A raster in 0°…360° (or beyond 180°): try the longitude a turn on.
      for (const double turn : {360.0, -360.0}) {
        const double shifted = px + inverse[1] * turn;
        if (shifted >= 0.0 && shifted <= width) {
          py += inverse[4] * turn;
          px = shifted;
          break;
        }
      }
    }
    const bool inside = ok[i] != 0 && std::isfinite(px) && std::isfinite(py) &&
                        px >= 0.0 && px <= width && py >= 0.0 && py <= height;
    x[i] = inside ? px : std::numeric_limits<double>::quiet_NaN();
    y[i] = inside ? py : std::numeric_limits<double>::quiet_NaN();
  }
}

bool Reader::read(Window& window, int32_t bufferWidth,
                  int32_t bufferHeight) const
{
  const std::size_t size = static_cast<std::size_t>(bufferWidth) *
                           static_cast<std::size_t>(bufferHeight);
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  // Fractional windows, and averaged where decimated (overviews used).
  extra.bFloatingPointWindowValidity = TRUE;
  extra.dfXOff = window.x0;
  extra.dfYOff = window.y0;
  extra.dfXSize = window.widthPx;
  extra.dfYSize = window.heightPx;
  extra.eResampleAlg =
      palette.empty() ? GRIORA_Average : GRIORA_NearestNeighbour;
  const int32_t xOff = static_cast<int32_t>(std::floor(window.x0));
  const int32_t yOff = static_cast<int32_t>(std::floor(window.y0));
  const int32_t xSize = std::max(
      1, std::min(width,
                  static_cast<int32_t>(std::ceil(window.x0 + window.widthPx))) -
             xOff);
  const int32_t ySize =
      std::max(1, std::min(height, static_cast<int32_t>(std::ceil(
                                       window.y0 + window.heightPx))) -
                      yOff);
  std::array<std::vector<double>, 4> values;
  for (std::size_t channel = 0; channel < 4; ++channel) {
    const int32_t band = bands.at(channel);
    if (band == 0) {
      continue;
    }
    values.at(channel).resize(size);
    if (GDALRasterIOEx(GDALGetRasterBand(dataset, band), GF_Read, xOff, yOff,
                       xSize, ySize, values.at(channel).data(), bufferWidth,
                       bufferHeight, GDT_Float64, 0, 0, &extra) != CE_None) {
      return false;
    }
  }
  window.bufferWidth = bufferWidth;
  window.bufferHeight = bufferHeight;
  window.pixels.assign(size, Rgba{});
  for (std::size_t i = 0; i < size; ++i) {
    const double first = values[0][i];
    if (hasNoData && first == noDataValue) {
      continue;
    }
    if (!palette.empty()) {
      const std::size_t index = static_cast<std::size_t>(first);
      window.pixels[i] = index < palette.size() ? palette[index] : Rgba{};
      continue;
    }
    const int32_t alpha =
        values[3].empty() ? 255 : static_cast<int32_t>(values[3][i]);
    window.pixels[i] = premultiplied(static_cast<int32_t>(first),
                                     static_cast<int32_t>(values[1][i]),
                                     static_cast<int32_t>(values[2][i]), alpha);
  }
  return true;
}

} // namespace gdalimagery

using gdalimagery::Reader;
using gdalimagery::Rgba;
using gdalimagery::toByte;

/** One raster of the file: the file itself, or one of its subdatasets. */
struct GdalImageryProvider::Part {
  QString name;
  GeoRect extent;
  double pixelSizeDeg = 0.0;
  /** Readers not in use; one is opened for each thread at once. */
  std::mutex mutex;
  std::vector<std::unique_ptr<Reader>> idle;

  /** Takes an idle reader, or opens one; null if it can't. */
  std::unique_ptr<Reader> acquire()
  {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (!idle.empty()) {
        std::unique_ptr<Reader> reader = std::move(idle.back());
        idle.pop_back();
        return reader;
      }
    }
    QString error;
    return Reader::open(name, &error);
  }

  /** Gives @p reader back. */
  void release(std::unique_ptr<Reader> reader)
  {
    const std::lock_guard<std::mutex> lock(mutex);
    idle.push_back(std::move(reader));
  }
};

namespace
{

/** The latitudes and longitudes of a reader's raster, by its edges. */
GeoRect extentOf(GDALDatasetH dataset,
                 const gdalimagery::GeoTransform& transform, bool* containsPole)
{
  OGRSpatialReferenceH crs = OSRClone(GDALGetSpatialRef(dataset));
  OSRSetAxisMappingStrategy(crs, OAMS_TRADITIONAL_GIS_ORDER);
  OGRSpatialReferenceH wgs84 = gdalimagery::newWgs84();
  OGRCoordinateTransformationH toWgs84 =
      OCTNewCoordinateTransformation(crs, wgs84);
  OGRCoordinateTransformationH fromWgs84 =
      OCTNewCoordinateTransformation(wgs84, crs);
  const double width = GDALGetRasterXSize(dataset);
  const double height = GDALGetRasterYSize(dataset);
  std::vector<double> x;
  std::vector<double> y;
  for (int32_t i = 0; i <= gdalimagery::edgeSamples; ++i) {
    const double t = static_cast<double>(i) / gdalimagery::edgeSamples;
    for (const std::array<double, 2>& pixel :
         {std::array<double, 2>{t * width, 0.0},
          std::array<double, 2>{t * width, height},
          std::array<double, 2>{0.0, t * height},
          std::array<double, 2>{width, t * height}}) {
      x.push_back(transform[0] + transform[1] * pixel[0] +
                  transform[2] * pixel[1]);
      y.push_back(transform[3] + transform[4] * pixel[0] +
                  transform[5] * pixel[1]);
    }
  }
  std::vector<int32_t> ok(x.size(), 0);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  OCTTransformEx(toWgs84, static_cast<int32_t>(x.size()), x.data(), y.data(),
                 nullptr, ok.data());
  GeoRect extent{180.0, 90.0, -180.0, -90.0};
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (ok[i] != 0 && std::isfinite(x[i]) && std::isfinite(y[i])) {
      extent.westDeg = std::min(extent.westDeg, x[i]);
      extent.eastDeg = std::max(extent.eastDeg, x[i]);
      extent.southDeg = std::min(extent.southDeg, y[i]);
      extent.northDeg = std::max(extent.northDeg, y[i]);
    }
  }
  // A pole inside the raster: all longitudes, up to the pole.
  *containsPole = false;
  for (const double poleLat : {90.0, -90.0}) {
    double px = 0.0;
    double py = poleLat;
    int32_t poleOk = 0;
    OCTTransformEx(fromWgs84, 1, &px, &py, nullptr, &poleOk);
    gdalimagery::GeoTransform forward = transform;
    gdalimagery::GeoTransform inverse{};
    if (GDALInvGeoTransform(forward.data(), inverse.data()) == 0) {
      continue; // Reader::open() has inverted it already: never.
    }
    const double column = inverse[0] + inverse[1] * px + inverse[2] * py;
    const double row = inverse[3] + inverse[4] * px + inverse[5] * py;
    if (poleOk != 0 && column >= 0.0 && column <= width && row >= 0.0 &&
        row <= height) {
      *containsPole = true;
      extent.westDeg = -180.0;
      extent.eastDeg = 180.0;
      (poleLat > 0.0 ? extent.northDeg : extent.southDeg) = poleLat;
    }
  }
  CPLPopErrorHandler();
  OCTDestroyCoordinateTransformation(toWgs84);
  OCTDestroyCoordinateTransformation(fromWgs84);
  OSRDestroySpatialReference(wgs84);
  OSRDestroySpatialReference(crs);
  extent.westDeg = std::max(extent.westDeg, -180.0);
  extent.eastDeg = std::min(extent.eastDeg, 180.0);
  extent.southDeg = std::max(extent.southDeg, -90.0);
  extent.northDeg = std::min(extent.northDeg, 90.0);
  return extent;
}

/** Draws @p reader's raster over @p composite, a @p size-pixel @p tile. */
bool drawPart(const Reader& reader, const GeoRect& tile, int32_t size,
              std::vector<Rgba>& composite)
{
  // Where each of the tile's pixel centres is in the raster.
  const std::size_t count =
      static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
  std::vector<double> x(count);
  std::vector<double> y(count);
  for (int32_t row = 0; row < size; ++row) {
    const double lat = tile.northDeg - (row + 0.5) * tile.heightDeg() / size;
    for (int32_t column = 0; column < size; ++column) {
      const std::size_t i =
          static_cast<std::size_t>(row) * static_cast<std::size_t>(size) +
          static_cast<std::size_t>(column);
      x[i] = tile.westDeg + (column + 0.5) * tile.widthDeg() / size;
      y[i] = lat;
    }
  }
  reader.toPixels(x, y);
  gdalimagery::Window window;
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for (std::size_t i = 0; i < count; ++i) {
    if (std::isfinite(x[i])) {
      minX = std::min(minX, x[i]);
      maxX = std::max(maxX, x[i]);
      minY = std::min(minY, y[i]);
      maxY = std::max(maxY, y[i]);
    }
  }
  if (minX > maxX) {
    return false;
  }
  // A pixel's margin for the bilinear samples at the edges.
  window.x0 = std::max(0.0, std::floor(minX) - 1.0);
  window.y0 = std::max(0.0, std::floor(minY) - 1.0);
  window.widthPx =
      std::min<double>(reader.width, std::ceil(maxX) + 1.0) - window.x0;
  window.heightPx =
      std::min<double>(reader.height, std::ceil(maxY) + 1.0) - window.y0;
  // Decimated to about the tile's own resolution (twice, for the average).
  const double limit = std::min(gdalimagery::maximumReadPx, 2 * size);
  const double factor =
      std::max({1.0, window.widthPx / limit, window.heightPx / limit});
  const int32_t bufferWidth =
      std::max(1, static_cast<int32_t>(std::ceil(window.widthPx / factor)));
  const int32_t bufferHeight =
      std::max(1, static_cast<int32_t>(std::ceil(window.heightPx / factor)));
  if (!reader.read(window, bufferWidth, bufferHeight)) {
    return false;
  }
  bool drawn = false;
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(x[i])) {
      continue;
    }
    const Rgba over = window.sample(x[i], y[i]);
    if (over.a <= 0.0F) {
      continue;
    }
    Rgba& under = composite[i];
    const float keep = 1.0F - over.a;
    under = Rgba{over.r + under.r * keep, over.g + under.g * keep,
                 over.b + under.b * keep, over.a + under.a * keep};
    drawn = true;
  }
  return drawn;
}

/** The rasters of the file at @p path: its subdatasets, or itself. */
QStringList rasterNames(const QString& path, QString* error)
{
  initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  CPLErrorReset();
  GDALDatasetH dataset =
      GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr);
  if (dataset == nullptr) {
    *error = QString::fromUtf8(CPLGetLastErrorMsg());
    if (error->isEmpty()) {
      *error = QStringLiteral("GDAL cannot read the file.");
    }
    return {};
  }
  QStringList names;
  CSLConstList subdatasets = GDALGetMetadata(dataset, "SUBDATASETS");
  for (CSLConstList item = subdatasets; item != nullptr && *item != nullptr;
       ++item) {
    const QString entry = QString::fromUtf8(*item);
    const int32_t equals =
        static_cast<int32_t>(entry.indexOf(QLatin1Char('=')));
    if (equals > 0 && entry.left(equals).endsWith(QLatin1String("_NAME"))) {
      names << entry.mid(equals + 1);
    }
  }
  if (names.isEmpty()) {
    names << path;
  }
  GDALClose(dataset);
  return names;
}

/**
 * @p name, or for an RPF table of contents' entry in a polar zone, its
 * frames: GDAL before 3.13 places a polar entry wrongly (its origin in
 * degrees, its pixels in metres), but each frame rightly.
 */
QStringList framesIfPolar(const QString& name)
{
  if (!name.startsWith(QLatin1String("NITF_TOC_ENTRY:"))) {
    return {name};
  }
  const QByteArray encoded = name.toUtf8();
  GDALDatasetH dataset =
      GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr);
  if (dataset == nullptr) {
    return {name};
  }
  QStringList frames;
  OGRSpatialReferenceH crs = GDALGetSpatialRef(dataset);
  if (crs != nullptr && OSRIsProjected(crs) != 0) {
    char** files = GDALGetFileList(dataset);
    for (char** file = files; file != nullptr && *file != nullptr; ++file) {
      const QString frame = QString::fromUtf8(*file);
      if (QFileInfo(frame).fileName().compare(QLatin1String("A.TOC"),
                                              Qt::CaseInsensitive) != 0) {
        frames << frame;
      }
    }
    CSLDestroy(files);
  }
  GDALClose(dataset);
  return frames.isEmpty() ? QStringList{name} : frames;
}

} // namespace

GdalImageryProvider::GdalImageryProvider(const QString& path)
    : m_path(QDir::cleanPath(path))
{
  QStringList names;
  for (const QString& raster : rasterNames(m_path, &m_error)) {
    names << framesIfPolar(raster);
  }
  double finestDeg = std::numeric_limits<double>::max();
  for (const QString& raster : names) {
    std::unique_ptr<Reader> reader = Reader::open(raster, &m_error);
    if (!reader) {
      m_parts.clear();
      return;
    }
    std::unique_ptr<Part> part = std::make_unique<Part>();
    part->name = raster;
    bool containsPole = false;
    part->extent = extentOf(reader->dataset, reader->transform, &containsPole);
    const double pixel =
        std::min(std::hypot(reader->transform[1], reader->transform[4]),
                 std::hypot(reader->transform[2], reader->transform[5]));
    part->pixelSizeDeg =
        reader->geographic ? pixel : pixel / gdalimagery::metresPerDegree;
    finestDeg = std::min(finestDeg, part->pixelSizeDeg);
    part->release(std::move(reader));
    m_parts.push_back(std::move(part));
  }
  if (m_parts.empty()) {
    return;
  }
  m_error.clear();
  // The level whose texels are no larger than the finest pixels.
  const double texelsAtLevel0 = TileKey::sizeDeg(0) / tileSizePxFixed;
  m_maximumLevel =
      std::clamp(static_cast<int32_t>(
                     std::ceil(std::log2(texelsAtLevel0 / finestDeg) - 1e-9)),
                 0, TileKey::maximumLevel);
}

GdalImageryProvider::~GdalImageryProvider() = default;

bool GdalImageryProvider::isValid() const
{
  return !m_parts.empty();
}

QString GdalImageryProvider::errorString() const
{
  return m_error;
}

QString GdalImageryProvider::id() const
{
  return QStringLiteral("file:") + m_path;
}

QString GdalImageryProvider::name() const
{
  return QFileInfo(m_path).fileName();
}

std::vector<GdalImageryProvider::Coverage> GdalImageryProvider::coverage() const
{
  std::vector<Coverage> parts;
  parts.reserve(m_parts.size());
  for (const std::unique_ptr<Part>& part : m_parts) {
    parts.push_back(Coverage{part->name, part->extent, part->pixelSizeDeg});
  }
  return parts;
}

QImage GdalImageryProvider::loadImage(TileKey key,
                                      const std::atomic<bool>& cancelled)
{
  const GeoRect tile = key.bounds();
  const int32_t size = tileSizePx();
  std::vector<Rgba> composite(static_cast<std::size_t>(size) *
                              static_cast<std::size_t>(size));
  bool anything = false;
  for (const std::unique_ptr<Part>& part : m_parts) {
    if (cancelled.load()) {
      return QImage();
    }
    const GeoRect& extent = part->extent;
    if (extent.westDeg >= tile.eastDeg || extent.eastDeg <= tile.westDeg ||
        extent.southDeg >= tile.northDeg || extent.northDeg <= tile.southDeg) {
      continue;
    }
    std::unique_ptr<Reader> reader = part->acquire();
    if (!reader) {
      continue;
    }
    anything = drawPart(*reader, tile, size, composite) || anything;
    part->release(std::move(reader));
  }
  if (!anything) {
    return QImage();
  }
  QImage image(size, size, QImage::Format_RGBA8888);
  for (int32_t row = 0; row < size; ++row) {
    uchar* line = image.scanLine(row);
    for (int32_t column = 0; column < size; ++column) {
      const Rgba& p = composite[static_cast<std::size_t>(row) *
                                    static_cast<std::size_t>(size) +
                                static_cast<std::size_t>(column)];
      const float scale = p.a > 0.0F ? 1.0F / p.a : 0.0F;
      uchar* pixel = line + static_cast<std::ptrdiff_t>(column) * 4;
      pixel[0] = toByte(p.r * scale);
      pixel[1] = toByte(p.g * scale);
      pixel[2] = toByte(p.b * scale);
      pixel[3] = toByte(p.a);
    }
  }
  return image;
}

} // namespace ts
