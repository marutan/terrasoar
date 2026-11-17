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

#include "imagerytiler.h"

#include <array>
#include <cstdint>

#include <QBuffer>
#include <QByteArray>
#include <QImageWriter>

#include <cpl_error.h>
#include <gdal.h>

#include "ts/deps/gdaldataset.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts::packager
{

namespace
{

/** Halves @p image with a 2 × 2 box filter (exact, unlike QImage::scaled). */
QImage halve(const QImage& image)
{
  const int32_t width = image.width() / 2;
  const int32_t height = image.height() / 2;
  QImage result(width, height, QImage::Format_RGB888);
  for (int32_t y = 0; y < height; ++y) {
    const std::uint8_t* row0 = image.constScanLine(2 * y);
    const std::uint8_t* row1 = image.constScanLine(2 * y + 1);
    std::uint8_t* out = result.scanLine(y);
    for (int32_t x = 0; x < width; ++x) {
      for (int32_t channel = 0; channel < 3; ++channel) {
        const int32_t left = 6 * x + channel;
        const int32_t sum =
            row0[left] + row0[left + 3] + row1[left] + row1[left + 3];
        out[3 * x + channel] = static_cast<std::uint8_t>((sum + 2) / 4);
      }
    }
  }
  return result;
}

QByteArray encodeJpeg(const QImage& tile, int32_t quality)
{
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  QImageWriter writer(&buffer, "jpeg");
  writer.setQuality(quality);
  writer.setOptimizedWrite(true);
  writer.setProgressiveScanWrite(false);
  if (!writer.write(tile)) {
    return QByteArray();
  }
  return bytes;
}

} // namespace

QImage readEquirectangular(const QString& sourcePath, int32_t widthPx,
                           int32_t heightPx, QString* errorString)
{
  const GdalDataset dataset(sourcePath);
  if (!dataset.isOpen() || dataset.rasterBandCount() < 3) {
    *errorString = QStringLiteral("Cannot read %1 as a three-band image: %2")
                       .arg(sourcePath, dataset.errorString());
    return QImage();
  }
  GDALDatasetH handle = static_cast<GDALDatasetH>(dataset.handle());
  const int32_t sourceWidth = GDALGetRasterXSize(handle);
  const int32_t sourceHeight = GDALGetRasterYSize(handle);
  if (sourceWidth != 2 * sourceHeight) {
    *errorString = QStringLiteral("%1 is %2 × %3 pixels, not 2:1")
                       .arg(sourcePath)
                       .arg(sourceWidth)
                       .arg(sourceHeight);
    return QImage();
  }
  QImage mosaic(widthPx, heightPx, QImage::Format_RGB888);
  if (mosaic.isNull()) {
    *errorString = QStringLiteral("Out of memory for a %1 × %2 mosaic")
                       .arg(widthPx)
                       .arg(heightPx);
    return QImage();
  }
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  extra.eResampleAlg = GRIORA_Average;
  std::array<int32_t, 3> bands = {1, 2, 3};
  CPLErrorReset();
  // Interleaved RGB straight into the QImage rows.
  const CPLErr result = GDALDatasetRasterIOEx(
      handle, GF_Read, 0, 0, sourceWidth, sourceHeight, mosaic.bits(), widthPx,
      heightPx, GDT_Byte, 3, bands.data(), 3, mosaic.bytesPerLine(), 1, &extra);
  if (result != CE_None) {
    *errorString =
        QStringLiteral("Cannot read %1: %2")
            .arg(sourcePath, QString::fromUtf8(CPLGetLastErrorMsg()));
    return QImage();
  }
  return mosaic;
}

bool tileMosaic(const QImage& mosaic, const ImageryTilerOptions& options,
                SqliteTileStoreWriter* writer, ImageryTilerStats* stats,
                QString* errorString)
{
  const std::function<void(const QString&)> progress =
      options.progress ? options.progress : [](const QString&) {};
  const int32_t size = options.tileSizePx;
  const int64_t columns = TileKey::columns(options.maxLevel);
  const int64_t rows = TileKey::rows(options.maxLevel);
  if (mosaic.width() != size * columns || mosaic.height() != size * rows) {
    *errorString = QStringLiteral("The mosaic is %1 × %2, expected %3 × %4")
                       .arg(mosaic.width())
                       .arg(mosaic.height())
                       .arg(size * columns)
                       .arg(size * rows);
    return false;
  }
  QImage level = mosaic.convertToFormat(QImage::Format_RGB888);
  for (int32_t depth = options.maxLevel; depth >= 0; --depth) {
    progress(QStringLiteral("Writing imagery level %1").arg(depth));
    const int32_t levelColumns = static_cast<int32_t>(TileKey::columns(depth));
    const int32_t levelRows = static_cast<int32_t>(TileKey::rows(depth));
    for (int32_t y = 0; y < levelRows; ++y) {
      for (int32_t x = 0; x < levelColumns; ++x) {
        const QImage tile = level.copy(x * size, y * size, size, size);
        const QByteArray bytes = encodeJpeg(tile, options.jpegQuality);
        if (bytes.isEmpty() || !writer->putTile(TileKey{depth, x, y}, bytes)) {
          *errorString = bytes.isEmpty()
                             ? QStringLiteral("JPEG encoding failed (is the "
                                              "Qt JPEG plugin installed?)")
                             : writer->errorString();
          return false;
        }
        stats->tiles += 1;
        stats->bytes += bytes.size();
      }
    }
    if (depth > 0) {
      level = halve(level);
    }
  }
  return true;
}

} // namespace ts::packager
