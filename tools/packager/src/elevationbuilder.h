// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Builds the base pack's global DEM (height grid tiles) and geoid grids.
 */
#ifndef TS_PACKAGER_ELEVATIONBUILDER_H
#define TS_PACKAGER_ELEVATIONBUILDER_H

#include <functional>

#include <QString>

namespace ts::packager
{

/** What DEM to build. */
struct ElevationOptions {
  /**
   * A global geographic raster of heights, metres (ETOPO 2022), any
   * GDAL-readable form, cells covering −180°…180° and −90°…90°.
   */
  QString sourcePath;
  /** The tile store to create (format "heights"). */
  QString outputPath;
  /** Deepest quadtree level stored; levels 0 to this are stored. */
  int maxLevel = 6;
  /** Samples along a tile's side (edges shared with the neighbours). */
  int samples = 65;
  /** The heights' vertical datum, written to the store (e.g. "egm2008"). */
  QString verticalDatum;
  /** The store's name (English). */
  QString name;
  /** Called with progress messages (English); may be empty. */
  std::function<void(const QString&)> progress;
};

/** What was built. */
struct ElevationStats {
  /** Tiles written. */
  int tiles = 0;
  /** Lowest and highest height of the deepest level, metres. */
  double minHeightM = 0.0;
  double maxHeightM = 0.0;
  /** Largest difference made by the encoding's quantization, metres. */
  double maxQuantizationErrorM = 0.0;
};

/**
 * Writes the height grids of levels 0 to options.maxLevel: each sample of
 * the deepest level is the mean of the source cells within half a sample
 * spacing of it (an area-weighted box), each coarser level the deeper one
 * filtered with [1 2 1] / 4 in both directions and taken every other
 * sample, so that neighbouring tiles share their edge samples exactly.
 * Each pole row holds one value, the row's mean. Fails with
 * @p errorString if the source can't be read or the store written.
 */
bool buildElevation(const ElevationOptions& options, ElevationStats* stats,
                    QString* errorString);

/** What geoid grid to build. */
struct GeoidGridOptions {
  /** A geoid grid in PROJ's GeoTIFF format (e.g. us_nga_egm08_25.tif). */
  QString sourcePath;
  /** The GeoTIFF to create. */
  QString outputPath;
  /** Keep every step-th sample in both directions (2: 2.5′ → 5′). */
  int step = 1;
};

/** What was built. */
struct GeoidGridStats {
  /** Samples across and down. */
  int width = 0;
  int height = 0;
};

/**
 * Writes the grid every options.step-th sample as 16-bit integer
 * centimetres with a scale of 0.01 (PROJ reads the scale), DEFLATE
 * compressed: 5.4 MB for EGM2008 at 5′ instead of 80 MB at 2.5′.
 */
bool buildGeoidGrid(const GeoidGridOptions& options, GeoidGridStats* stats,
                    QString* errorString);

} // namespace ts::packager

#endif // TS_PACKAGER_ELEVATIONBUILDER_H
