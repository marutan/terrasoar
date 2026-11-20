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
/*
 * Writes the synthetic RPF test charts in tests/data/rpf: a checker of
 * eight colours, a square every 0.25° of latitude and, near the equator,
 * of longitude (every 2.5° near the pole, else the squares would be thin
 * slivers there), as CADRG (GNC, 1:5M) through
 * GDAL's CADRG writer, which needs GDAL 3.13 or later (reading them
 * needs any GDAL). See tests/data/rpf/README.md.
 *
 *   make_rpf_fixtures <output directory>
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

namespace
{

/** The checker's colours, as tests/providers expects them. */
const GDALColorEntry colours[8] = {{230, 25, 75, 255},  {60, 180, 75, 255},
                                   {255, 225, 25, 255}, {0, 130, 200, 255},
                                   {245, 130, 48, 255}, {145, 30, 180, 255},
                                   {70, 240, 240, 255}, {250, 250, 250, 255}};

/** Squares per degree of latitude. */
constexpr int32_t squaresPerDeg = 4;

/** Source pixels per square. */
constexpr int32_t pixelsPerSquare = 40;

/**
 * Writes a chart of the checker over [west, east] × [south, north],
 * @p lonSquaresPerDeg squares per degree of longitude.
 */
bool writeChart(const std::string& directory, double west, double south,
                double east, double north, double lonSquaresPerDeg)
{
  const int32_t width =
      static_cast<int32_t>((east - west) * squaresPerDeg * pixelsPerSquare);
  const int32_t height =
      static_cast<int32_t>((north - south) * squaresPerDeg * pixelsPerSquare);
  GDALDriverH mem = GDALGetDriverByName("MEM");
  GDALDatasetH source =
      GDALCreate(mem, "", width, height, 1, GDT_Byte, nullptr);
  const double pixelDeg = 1.0 / (squaresPerDeg * pixelsPerSquare);
  double transform[6] = {west, pixelDeg, 0.0, north, 0.0, -pixelDeg};
  GDALSetGeoTransform(source, transform);
  OGRSpatialReferenceH wgs84 = OSRNewSpatialReference(nullptr);
  OSRImportFromEPSG(wgs84, 4326);
  // Longitude first, as the geotransform is.
  OSRSetAxisMappingStrategy(wgs84, OAMS_TRADITIONAL_GIS_ORDER);
  GDALSetSpatialRef(source, wgs84);
  OSRDestroySpatialReference(wgs84);
  GDALRasterBandH band = GDALGetRasterBand(source, 1);
  GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
  for (int32_t i = 0; i < 8; ++i) {
    GDALSetColorEntry(table, i, &colours[i]);
  }
  GDALSetRasterColorTable(band, table);
  GDALDestroyColorTable(table);
  std::vector<uint8_t> row(static_cast<std::size_t>(width));
  for (int32_t y = 0; y < height; ++y) {
    // A square's colour from its latitude and longitude indexes, the
    // same anywhere the checker is drawn (tests/providers).
    const int32_t j = static_cast<int32_t>(
        (north - (y + 0.5) * pixelDeg + 90.0) * squaresPerDeg);
    for (int32_t x = 0; x < width; ++x) {
      const int32_t i = static_cast<int32_t>(
          (west + (x + 0.5) * pixelDeg + 180.0) * lonSquaresPerDeg);
      row[static_cast<std::size_t>(x)] = static_cast<uint8_t>((i + 3 * j) % 8);
    }
    if (GDALRasterIO(band, GF_Write, 0, y, width, 1, row.data(), width, 1,
                     GDT_Byte, 0, 0) != CE_None) {
      return false;
    }
  }
  char** options = nullptr;
  options = CSLSetNameValue(options, "PRODUCT_TYPE", "CADRG");
  options = CSLSetNameValue(options, "SERIES_CODE", "GN");
  options = CSLSetNameValue(options, "SCALE", "5000000");
  options = CSLSetNameValue(options, "RESAMPLING", "NEAREST");
  GDALDatasetH chart =
      GDALCreateCopy(GDALGetDriverByName("NITF"), directory.c_str(), source,
                     FALSE, options, nullptr, nullptr);
  CSLDestroy(options);
  GDALClose(source);
  if (chart == nullptr) {
    return false;
  }
  GDALClose(chart);
  return true;
}

} // namespace

int32_t main(int32_t argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: make_rpf_fixtures <output directory>\n");
    return 2;
  }
  GDALAllRegister();
  const std::string out = argv[1];
  // Zone 2 (32° to 48° N), a degree and a half square.
  if (!writeChart(out + "/zone2", 2.0, 34.5, 3.5, 36.0, 4.0)) {
    return 1;
  }
  // Zone 9, the north polar zone: 85° to 86.5° N, 0° to 10° E, squares
  // 2.5° of longitude wide (about 20 km).
  if (!writeChart(out + "/zone9", 0.0, 85.0, 10.0, 86.5, 0.4)) {
    return 1;
  }
  return 0;
}
