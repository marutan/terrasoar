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
 * The National Grid's test data: the Ordnance Survey's OSTN15 test points,
 * where to find the real OSTN15 grid, and a synthetic shift grid.
 */
#ifndef TS_TEST_NATIONALGRIDDATA_H
#define TS_TEST_NATIONALGRIDDATA_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <gdal.h>

#include "ts/deps/dependencies.h"

namespace ts::test
{

/** One of the Ordnance Survey's OSTN15 test points. */
struct Ostn15TestPoint {
  /** Its name in the test data, "TP01" to "TP40". */
  const char* name;
  /** ETRS89 latitude and longitude, degrees. */
  double latDeg;
  double lonDeg;
  /** The OSGB36 easting and northing OSTN15 gives, metres. */
  double eastingM;
  double northingM;
};

/**
 * The Ordnance Survey's OSTN15 test points (OSTN15_TestInput_ETRStoOSGB
 * and its expected output, from the OSTN15 developer pack): ETRS89
 * positions across Great Britain, the Isle of Man and the outer isles, and
 * their National Grid coordinates.
 */
inline const std::vector<Ostn15TestPoint>& ostn15TestPoints()
{
  static const std::vector<Ostn15TestPoint> points = {
      {"TP01", 49.92226393730, -6.29977752014, 91492.146, 11318.804},
      {"TP02", 49.96006137820, -5.20304609998, 170370.718, 11572.405},
      {"TP03", 50.43885825610, -4.10864563561, 250359.811, 62016.569},
      {"TP04", 50.57563665000, -1.29782277240, 449816.371, 75335.861},
      {"TP05", 50.93127937910, -1.45051433700, 438710.920, 114792.250},
      {"TP06", 51.40078220140, -3.55128349240, 292184.870, 168003.465},
      {"TP07", 51.37447025550, 1.44454730409, 639821.835, 169565.858},
      {"TP08", 51.42754743020, -2.54407618349, 362269.991, 169978.690},
      {"TP09", 51.48936564950, -0.11992557180, 530624.974, 178388.464},
      {"TP10", 51.85890896400, -4.30852476960, 241124.584, 220332.641},
      {"TP11", 51.89436637350, 0.89724327012, 599445.590, 225722.826},
      {"TP12", 52.25529381630, -2.15458614387, 389544.190, 261912.153},
      {"TP13", 52.25160951230, -0.91248956970, 474335.969, 262047.755},
      {"TP14", 52.75136687170, 0.40153547065, 562180.547, 319784.995},
      {"TP15", 52.96219109410, -1.19747655922, 454002.834, 340834.943},
      {"TP16", 53.34480280190, -2.64049320810, 357455.843, 383290.436},
      {"TP17", 53.41628516040, -4.28918069756, 247958.971, 393492.909},
      {"TP18", 53.41630925420, -4.28917792869, 247959.241, 393495.583},
      {"TP19", 53.77911025760, -3.04045490691, 331534.564, 431920.794},
      {"TP20", 53.80021519630, -1.66379168242, 422242.186, 433818.701},
      {"TP21", 54.08666318080, -4.63452168212, 227778.330, 468847.388},
      {"TP22", 54.11685144290, -0.07773133187, 525745.670, 470703.214},
      {"TP23", 54.32919541010, -4.38849118133, 244780.636, 495254.887},
      {"TP24", 54.89542340420, -2.93827741149, 339921.145, 556034.761},
      {"TP25", 54.97912273660, -1.61657685184, 424639.355, 565012.703},
      {"TP26", 55.85399952950, -4.29649016251, 256340.925, 664697.269},
      {"TP27", 55.92478265510, -3.29479219337, 319188.434, 670947.534},
      {"TP28", 57.00606696050, -5.82836691850, 167634.202, 797067.144},
      {"TP29", 57.13902518960, -2.04856030746, 397160.491, 805349.736},
      {"TP30", 57.48625000720, -4.21926398555, 267056.768, 846176.972},
      {"TP31", 57.81351838410, -8.57854456076, 9587.909, 899448.996},
      {"TP32", 58.21262247180, -7.59255560556, 71713.132, 938516.404},
      {"TP33", 58.51560361300, -6.26091455533, 151968.652, 966483.780},
      {"TP34", 58.58120461280, -3.72631022121, 299721.891, 967202.992},
      {"TP35", 59.03743871190, -3.21454001115, 330398.323, 1017347.016},
      {"TP36", 59.09335035320, -4.41757674598, 261596.778, 1025447.602},
      {"TP37", 59.09671617400, -5.82799339844, 180862.461, 1029604.114},
      {"TP38", 59.53470794490, -1.62516966058, 421300.525, 1072147.239},
      {"TP39", 59.85409913890, -1.27486910356, 440725.073, 1107878.448},
      {"TP40", 60.13308091660, -2.07382822798, 395999.668, 1138728.951},
  };
  return points;
}

/** The file name of the OSTN15 grid, as PROJ distributes it. */
inline QString ostn15GridFileName()
{
  return QStringLiteral("uk_os_OSTN15_NTv2_OSGBtoETRS.tif");
}

/**
 * Returns the path of the real OSTN15 grid, or empty: the file
 * TS_OSTN15_GRID names, else the grid in PROJ's search paths (PROJ_DATA,
 * or a PROJ data package that has it).
 */
inline QString ostn15GridPath()
{
  const QString named = qEnvironmentVariable("TS_OSTN15_GRID");
  if (!named.isEmpty()) {
    return QFileInfo::exists(named) ? named : QString();
  }
  const QStringList directories = projSearchPaths();
  for (const QString& directory : directories) {
    const QString path = QDir(directory).filePath(ostn15GridFileName());
    if (QFileInfo::exists(path)) {
      return path;
    }
  }
  return QString();
}

/** The synthetic grid's latitude offset at a node, arc-seconds. */
// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline double fixtureLatOffsetSec(double latDeg, double lonDeg)
{
  return -2.0 + 0.3 * std::sin(0.5 * latDeg) + 0.05 * lonDeg;
}

/** The synthetic grid's longitude offset at a node, arc-seconds east. */
// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline double fixtureLonOffsetSec(double latDeg, double lonDeg)
{
  return -5.0 + 0.4 * std::cos(0.7 * lonDeg) - 0.02 * latDeg;
}

/**
 * Writes a horizontal offset grid in PROJ's GeoTIFF form to @p path:
 * nodes every @p stepDeg from 9° W to 2° E and 49° N to 61° N (OSTN15's
 * extent), offsets from fixtureLatOffsetSec() and fixtureLonOffsetSec(),
 * longitude offsets positive east, or west if @p positiveWest (and then
 * negated, so the grid means the same).
 */
inline bool writeFixtureShiftGrid(const QString& path, double stepDeg = 0.25,
                                  bool positiveWest = false)
{
  initialiseGdal();
  const double west = -9.0;
  const double north = 61.0;
  const int32_t width = static_cast<int32_t>(std::lround(11.0 / stepDeg)) + 1;
  const int32_t height = static_cast<int32_t>(std::lround(12.0 / stepDeg)) + 1;
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH dataset =
      GDALCreate(GDALGetDriverByName("GTiff"), encoded.constData(), width,
                 height, 2, GDT_Float32, nullptr);
  if (dataset == nullptr) {
    return false;
  }
  // GDAL wants the corners of pixels about the nodes.
  std::array<double, 6> transform{west - 0.5 * stepDeg,  stepDeg, 0.0,
                                  north + 0.5 * stepDeg, 0.0,     -stepDeg};
  GDALSetGeoTransform(dataset, transform.data());
  GDALSetMetadataItem(dataset, "AREA_OR_POINT", "Point", nullptr);
  GDALSetMetadataItem(dataset, "TYPE", "HORIZONTAL_OFFSET", nullptr);
  GDALRasterBandH latBand = GDALGetRasterBand(dataset, 1);
  GDALRasterBandH lonBand = GDALGetRasterBand(dataset, 2);
  GDALSetDescription(latBand, "latitude_offset");
  GDALSetDescription(lonBand, "longitude_offset");
  GDALSetMetadataItem(lonBand, "positive_value", positiveWest ? "west" : "east",
                      nullptr);
  std::vector<float> lat(static_cast<std::size_t>(width) *
                         static_cast<std::size_t>(height));
  std::vector<float> lon(lat.size());
  for (int32_t row = 0; row < height; ++row) {
    for (int32_t column = 0; column < width; ++column) {
      const double latDeg = north - row * stepDeg;
      const double lonDeg = west + column * stepDeg;
      const std::size_t i =
          static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(column);
      lat[i] = static_cast<float>(fixtureLatOffsetSec(latDeg, lonDeg));
      const double east = fixtureLonOffsetSec(latDeg, lonDeg);
      lon[i] = static_cast<float>(positiveWest ? -east : east);
    }
  }
  const bool ok =
      GDALRasterIO(latBand, GF_Write, 0, 0, width, height, lat.data(), width,
                   height, GDT_Float32, 0, 0) == CE_None &&
      GDALRasterIO(lonBand, GF_Write, 0, 0, width, height, lon.data(), width,
                   height, GDT_Float32, 0, 0) == CE_None;
  GDALClose(dataset);
  return ok;
}

} // namespace ts::test

#endif // TS_TEST_NATIONALGRIDDATA_H
