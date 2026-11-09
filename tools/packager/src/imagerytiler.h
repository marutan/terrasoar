// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Cuts an equirectangular world image into quadtree tiles.
 */
#ifndef TS_PACKAGER_IMAGERYTILER_H
#define TS_PACKAGER_IMAGERYTILER_H

#include <functional>

#include <QImage>
#include <QString>

namespace ts
{
class SqliteTileStoreWriter;
} // namespace ts

namespace ts::packager
{

/** What to build. */
struct ImageryTilerOptions {
  /** Deepest level written; levels 0 to it are complete. */
  int maxLevel = 4;
  /** Tile width and height, pixels. */
  int tileSizePx = 256;
  /** JPEG quality, 0 to 100. */
  int jpegQuality = 85;
  /** Called with progress messages (English); may be empty. */
  std::function<void(const QString&)> progress;
};

/** Results of a run. */
struct ImageryTilerStats {
  /** Tiles written. */
  int tiles = 0;
  /** Encoded bytes written. */
  long long bytes = 0;
};

/**
 * Reads the equirectangular image at @p sourcePath (any GDAL raster with
 * three 8-bit bands covering −180° to 180°, 90° to −90°, pixel corners on
 * the edges) into the mosaic of the deepest level with area averaging.
 */
QImage readEquirectangular(const QString& sourcePath, int widthPx, int heightPx,
                           QString* errorString);

/**
 * Cuts @p mosaic (2:1, the whole world, at least the deepest level's size)
 * into tiles of every level from options.maxLevel up to 0, each level
 * halving the one below with a 2 × 2 box filter, and writes them as JPEG
 * to @p writer.
 */
bool tileMosaic(const QImage& mosaic, const ImageryTilerOptions& options,
                SqliteTileStoreWriter* writer, ImageryTilerStats* stats,
                QString* errorString);

} // namespace ts::packager

#endif // TS_PACKAGER_IMAGERYTILER_H
