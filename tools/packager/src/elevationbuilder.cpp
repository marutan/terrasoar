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

#include "elevationbuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QTemporaryFile>

#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>

#include "ts/deps/dependencies.h"
#include "ts/deps/gdaldataset.h"
#include "ts/elevation/heightgrid.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/tiling/tilekey.h"

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

/**
 * Grids larger than this are kept in a memory-mapped temporary file rather
 * than in memory: GEBCO's 15″ for a quarter of the world is 3.7 GB, more
 * than a machine building packs may have free. The grid is read and
 * written row by row, so the system pages it well.
 */
constexpr std::size_t mappedGridBytes = std::size_t{1} << 30;

/**
 * A grid of point samples, row 0 at 90° N, column 0 at the region's west
 * edge (180° W for the world).
 */
class PointGrid
{
public:
  // Plain dimensions, read everywhere; the samples behind them are private.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  int32_t width = 0;
  int32_t height = 0;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

  /** Makes it @p columns by @p rows, every sample 0. False on failure. */
  // Columns, then rows, as width and height.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool allocate(int32_t columns, int32_t rows)
  {
    width = columns;
    height = rows;
    const std::size_t count = size();
    m_owned.clear();
    m_file.reset();
    m_data = nullptr;
    if (count * sizeof(float) <= mappedGridBytes) {
      m_owned.assign(count, 0.0F);
      m_data = m_owned.data();
      return true;
    }
    m_file = std::make_unique<QTemporaryFile>();
    const qint64 bytes =
        static_cast<qint64>(count) * static_cast<qint64>(sizeof(float));
    if (!m_file->open() || !m_file->resize(bytes)) {
      return false;
    }
    // A new file reads as zeros.
    m_data = reinterpret_cast<float*>(m_file->map(0, bytes));
    return m_data != nullptr;
  }

  std::size_t size() const
  {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  }
  const float* begin() const { return m_data; }
  const float* end() const { return m_data + size(); }

  float& at(int32_t row, int32_t column)
  {
    return m_data[static_cast<std::size_t>(row) *
                      static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(column)];
  }
  float at(int32_t row, int32_t column) const
  {
    return m_data[static_cast<std::size_t>(row) *
                      static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(column)];
  }

private:
  std::vector<float> m_owned;
  std::unique_ptr<QTemporaryFile> m_file;
  float* m_data = nullptr;
};

/** Overlap of the source cells with an interval, as (cell, weight) pairs. */
using Weights = std::vector<std::pair<int32_t, double>>;

/**
 * The cells of [@p from, @p to) (in cell units) and how much of each lies
 * in it; cells outside [0, @p count) wrap round when @p wrap, else are
 * left out.
 */
// An interval's two ends.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Weights cellWeights(double from, double to, int32_t count, bool wrap)
{
  Weights weights;
  for (int32_t cell = static_cast<int32_t>(std::floor(from));
       cell < static_cast<int32_t>(std::ceil(to)); ++cell) {
    const double overlap =
        std::min(to, cell + 1.0) - std::max(from, static_cast<double>(cell));
    if (overlap <= 0.0) {
      continue;
    }
    int32_t index = cell;
    if (wrap) {
      index = ((cell % count) + count) % count;
    } else if (cell < 0 || cell >= count) {
      continue;
    }
    weights.emplace_back(index, overlap);
  }
  return weights;
}

/** Reads source rows on demand, keeping the ones still needed. */
class RowReader
{
public:
  /**
   * Reads the columns from @p first (which may be negative, or reach past
   * the east edge: they wrap around the world) to @p first + @p count of
   * rows @p width columns wide.
   */
  RowReader(GDALRasterBandH band, int32_t width, int32_t first, int32_t count)
      : m_band(band), m_width(width), m_column(first), m_count(count)
  {
    int32_t hasNoData = 0;
    m_noData = GDALGetRasterNoDataValue(band, &hasNoData);
    m_hasNoData = hasNoData != 0;
  }

  /** Row @p row; rows before @p keepFrom are forgotten. Null on error. */
  // The row wanted, then the first still needed.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const std::vector<float>* row(int32_t row, int32_t keepFrom)
  {
    while (!m_rows.empty() && m_first < keepFrom) {
      m_rows.pop_front();
      ++m_first;
    }
    if (m_rows.empty()) {
      m_first = row;
    }
    while (m_first + static_cast<int32_t>(m_rows.size()) <= row) {
      std::vector<float> values(static_cast<std::size_t>(m_count));
      const int32_t next = m_first + static_cast<int32_t>(m_rows.size());
      // One piece, or two where the window wraps around the world.
      int32_t done = 0;
      while (done < m_count) {
        const int32_t column =
            ((m_column + done) % m_width + m_width) % m_width;
        const int32_t length = std::min(m_count - done, m_width - column);
        if (GDALRasterIO(m_band, GF_Read, column, next, length, 1,
                         values.data() + done, length, 1, GDT_Float32, 0,
                         0) != CE_None) {
          return nullptr;
        }
        done += length;
      }
      m_rows.push_back(std::move(values));
    }
    return &m_rows[static_cast<std::size_t>(row - m_first)];
  }

  bool isNoData(float value) const
  {
    return !std::isfinite(value) ||
           (m_hasNoData && static_cast<double>(value) == m_noData);
  }

private:
  GDALRasterBandH m_band;
  int32_t m_width;
  int32_t m_column;
  int32_t m_count;
  double m_noData = 0.0;
  bool m_hasNoData = false;
  int32_t m_first = 0;
  std::deque<std::vector<float>> m_rows;
};

/** Sets each pole row to its mean: the pole is one point. */
void flattenPoles(PointGrid& grid)
{
  for (const int32_t row : {0, grid.height - 1}) {
    double sum = 0.0;
    for (int32_t column = 0; column < grid.width - 1; ++column) {
      sum += static_cast<double>(grid.at(row, column));
    }
    const float mean = static_cast<float>(sum / (grid.width - 1));
    for (int32_t column = 0; column < grid.width; ++column) {
      grid.at(row, column) = mean;
    }
  }
}

/**
 * Sums the source rows of @p rowWeights, weighted, into @p sums, and the
 * weights of the cells with data into @p totals. False on a read error.
 */
bool accumulateRows(RowReader& reader, const Weights& rowWeights,
                    std::vector<double>* sums, std::vector<double>* totals)
{
  std::fill(sums->begin(), sums->end(), 0.0);
  std::fill(totals->begin(), totals->end(), 0.0);
  for (const std::pair<int32_t, double>& weight : rowWeights) {
    const std::vector<float>* values =
        reader.row(weight.first, rowWeights.front().first);
    if (values == nullptr) {
      return false;
    }
    for (std::size_t x = 0; x < values->size(); ++x) {
      if (!reader.isNoData((*values)[x])) {
        (*sums)[x] += weight.second * static_cast<double>((*values)[x]);
        (*totals)[x] += weight.second;
      }
    }
  }
  return true;
}

/** The mean of the accumulated cells of @p weights; 0 without data. */
// The sums, then their weights, as accumulateRows() fills them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float weightedMean(const Weights& weights, const std::vector<double>& sums,
                   const std::vector<double>& totals)
{
  double sum = 0.0;
  double total = 0.0;
  for (const std::pair<int32_t, double>& weight : weights) {
    sum += weight.second * sums[static_cast<std::size_t>(weight.first)];
    total += weight.second * totals[static_cast<std::size_t>(weight.first)];
  }
  return total > 0.0 ? static_cast<float>(sum / total) : 0.0F;
}

/**
 * The deepest level's samples from @p westDeg eastwards, @p columns of
 * them, box-filtered from the source.
 */
// The intervals of 180°, then the columns wanted.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool sampleSource(const GdalDataset& source, int32_t intervals, double westDeg,
                  int32_t columns, PointGrid* grid,
                  const std::function<void(const QString&)>& progress,
                  QString* errorString)
{
  GDALDatasetH dataset = static_cast<GDALDatasetH>(source.handle());
  std::array<double, 6> transform{};
  if (GDALGetGeoTransform(dataset, transform.data()) != CE_None ||
      transform[2] != 0.0 || transform[4] != 0.0 || transform[1] <= 0.0 ||
      transform[5] >= 0.0) {
    return fail(errorString,
                QStringLiteral("The DEM is not a north-up geographic grid"));
  }
  const int32_t sourceWidth = GDALGetRasterXSize(dataset);
  const int32_t sourceHeight = GDALGetRasterYSize(dataset);
  // The source must go the whole way round for wrapping to be right.
  if (std::abs(transform[1] * sourceWidth - 360.0) > 1e-6) {
    return fail(errorString, QStringLiteral("The DEM does not span 360°"));
  }
  const double step = 180.0 / intervals;
  // The source columns the region's samples reach (all for the world):
  // read only those.
  int32_t first = static_cast<int32_t>(
      std::floor((westDeg - step / 2.0 - transform[0]) / transform[1]));
  int32_t count =
      static_cast<int32_t>(std::ceil(
          (westDeg + (columns - 1) * step + step / 2.0 - transform[0]) /
          transform[1])) -
      first;
  if (count >= sourceWidth) {
    first = 0;
    count = sourceWidth;
  }
  RowReader reader(GDALGetRasterBand(dataset, 1), sourceWidth, first, count);
  if (!grid->allocate(columns, intervals + 1)) {
    return fail(errorString, QStringLiteral("No room for a %1 × %2 grid")
                                 .arg(columns)
                                 .arg(intervals + 1));
  }
  // Horizontal weights are the same for every row.
  std::vector<Weights> columnWeights;
  for (int32_t column = 0; column < grid->width; ++column) {
    const double lon = westDeg + column * step;
    Weights weights = cellWeights(
        (lon - step / 2.0 - transform[0]) / transform[1],
        (lon + step / 2.0 - transform[0]) / transform[1], sourceWidth, true);
    // Source columns to the window's.
    for (std::pair<int32_t, double>& weight : weights) {
      weight.first =
          ((weight.first - first) % sourceWidth + sourceWidth) % sourceWidth;
    }
    columnWeights.push_back(std::move(weights));
  }
  std::vector<double> sums(static_cast<std::size_t>(count));
  std::vector<double> totals(static_cast<std::size_t>(count));
  for (int32_t row = 0; row < grid->height; ++row) {
    if (row % 512 == 0) {
      progress(QStringLiteral("Sampling the DEM, row %1 of %2")
                   .arg(row)
                   .arg(grid->height));
    }
    const double lat = 90.0 - row * step;
    const Weights rowWeights = cellWeights(
        (std::min(90.0, lat + step / 2.0) - transform[3]) / transform[5],
        (std::max(-90.0, lat - step / 2.0) - transform[3]) / transform[5],
        sourceHeight, false);
    if (rowWeights.empty()) {
      return fail(errorString,
                  QStringLiteral("The DEM has no rows at %1°").arg(lat));
    }
    if (!accumulateRows(reader, rowWeights, &sums, &totals)) {
      return fail(errorString,
                  QStringLiteral("Cannot read the DEM: %1")
                      .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
    }
    for (int32_t column = 0; column < grid->width; ++column) {
      grid->at(row, column) = weightedMean(
          columnWeights[static_cast<std::size_t>(column)], sums, totals);
    }
  }
  if (westDeg <= -180.0 && grid->width == 2 * intervals + 1) {
    flattenPoles(*grid);
  }
  return true;
}

/**
 * The next coarser level: [1 2 1] / 4 each way, every other sample. Around
 * the world (@p wrap) the filter wraps east to west; for a region it stops
 * at the region's edges.
 */
std::optional<PointGrid> coarser(const PointGrid& fine, bool wrap)
{
  PointGrid grid;
  if (!grid.allocate((fine.width - 1) / 2 + 1, (fine.height - 1) / 2 + 1)) {
    return std::nullopt;
  }
  const int32_t period = fine.width - 1; // the last column repeats the first
  constexpr std::array<double, 3> taps{0.25, 0.5, 0.25};
  for (int32_t row = 0; row < grid.height; ++row) {
    for (int32_t column = 0; column < grid.width; ++column) {
      double sum = 0.0;
      for (std::size_t i = 0; i < taps.size(); ++i) {
        const int32_t y = std::clamp(2 * row + static_cast<int32_t>(i) - 1, 0,
                                     fine.height - 1);
        for (std::size_t j = 0; j < taps.size(); ++j) {
          const int32_t at = 2 * column + static_cast<int32_t>(j) - 1;
          const int32_t x = wrap ? (at % period + period) % period
                                 : std::clamp(at, 0, fine.width - 1);
          sum += taps.at(i) * taps.at(j) * static_cast<double>(fine.at(y, x));
        }
      }
      grid.at(row, column) = static_cast<float>(sum);
    }
  }
  if (wrap) {
    flattenPoles(grid);
  }
  return std::optional<PointGrid>(std::move(grid));
}

/** Writes every tile of @p level from its grid, which starts at tile
 *  column @p firstX. */
// The level, then its first column, then the tile size.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool writeLevel(const PointGrid& grid, int32_t level, int32_t firstX,
                int32_t samples, SqliteTileStoreWriter* writer,
                ElevationStats* stats, QString* errorString)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const int32_t intervals = samples - 1;
  const std::size_t n = static_cast<std::size_t>(samples);
  std::vector<float> heights(n * n);
  const int32_t tiles = (grid.width - 1) / intervals;
  for (int32_t y = 0; y < static_cast<int32_t>(TileKey::rows(level)); ++y) {
    for (int32_t x = firstX; x < firstX + tiles; ++x) {
      for (int32_t row = 0; row < samples; ++row) {
        for (int32_t column = 0; column < samples; ++column) {
          const float value =
              grid.at(y * intervals + row, (x - firstX) * intervals + column);
          heights[static_cast<std::size_t>(row) * n +
                  static_cast<std::size_t>(column)] = value;
          const double metres = static_cast<double>(value);
          stats->maxQuantizationErrorM =
              std::max(stats->maxQuantizationErrorM,
                       std::abs(quantizedHeightM(metres) - metres));
        }
      }
      if (!writer->putTile(TileKey{level, x, y},
                           encodeHeightGrid(heights, samples))) {
        return fail(errorString, writer->errorString());
      }
      stats->tiles += 1;
    }
  }
  return true;
}

} // namespace

bool buildElevation(const ElevationOptions& options, ElevationStats* stats,
                    QString* errorString)
{
  ts::initialiseGdal();
  const std::function<void(const QString&)> progress =
      [&options](const QString& message) {
        if (options.progress) {
          options.progress(message);
        }
      };
  // The region must be whole tiles of the shallowest level.
  const double tileDeg = 180.0 / std::pow(2.0, options.minLevel);
  const double westTiles = (options.westDeg + 180.0) / tileDeg;
  const double eastTiles = (options.eastDeg + 180.0) / tileDeg;
  if (options.samples < 3 || options.maxLevel < 0 || options.maxLevel > 10 ||
      options.minLevel < 0 || options.minLevel > options.maxLevel ||
      options.westDeg < -180.0 || options.eastDeg > 180.0 ||
      options.eastDeg <= options.westDeg ||
      std::abs(westTiles - std::round(westTiles)) > 1e-9 ||
      std::abs(eastTiles - std::round(eastTiles)) > 1e-9) {
    return fail(errorString, QStringLiteral("Bad elevation options"));
  }
  const bool world = options.westDeg <= -180.0 && options.eastDeg >= 180.0;
  const GdalDataset source(options.sourcePath);
  if (!source.isOpen()) {
    return fail(errorString,
                QStringLiteral("Cannot read %1: %2")
                    .arg(options.sourcePath, source.errorString()));
  }
  const int32_t intervals = (options.samples - 1) << options.maxLevel;
  const int32_t columns =
      static_cast<int32_t>(std::lround((options.eastDeg - options.westDeg) /
                                       180.0 * intervals)) +
      1;
  PointGrid grid;
  if (!sampleSource(source, intervals, options.westDeg, columns, &grid,
                    progress, errorString)) {
    return false;
  }
  const std::pair<const float*, const float*> range =
      std::minmax_element(grid.begin(), grid.end());
  stats->minHeightM = static_cast<double>(*range.first);
  stats->maxHeightM = static_cast<double>(*range.second);

  SqliteTileStoreInfo info;
  info.format = QStringLiteral("heights");
  info.tileSizePx = options.samples;
  info.minLevel = options.minLevel;
  info.maxLevel = options.maxLevel;
  info.name = options.name;
  info.verticalDatum = options.verticalDatum;
  SqliteTileStoreWriter writer(options.outputPath, info);
  if (!writer.errorString().isEmpty()) {
    return fail(errorString, writer.errorString());
  }
  for (int32_t level = options.maxLevel; level >= options.minLevel; --level) {
    progress(QStringLiteral("Writing elevation level %1").arg(level));
    const int32_t firstX = static_cast<int32_t>(
        std::lround((options.westDeg + 180.0) / ts::TileKey::sizeDeg(level)));
    if (!writeLevel(grid, level, firstX, options.samples, &writer, stats,
                    errorString)) {
      return false;
    }
    if (level > options.minLevel) {
      std::optional<PointGrid> next = coarser(grid, world);
      if (!next) {
        return fail(
            errorString,
            QStringLiteral("No room for level %1's grid").arg(level - 1));
      }
      grid = std::move(*next);
    }
  }
  if (!writer.finish()) {
    return fail(errorString, writer.errorString());
  }
  return true;
}

bool buildGeoidGrid(const GeoidGridOptions& options, GeoidGridStats* stats,
                    QString* errorString)
{
  ts::initialiseGdal();
  const GdalDataset source(options.sourcePath);
  if (!source.isOpen() || options.step < 1) {
    return fail(errorString,
                QStringLiteral("Cannot read %1: %2")
                    .arg(options.sourcePath, source.errorString()));
  }
  GDALDatasetH dataset = static_cast<GDALDatasetH>(source.handle());
  const int32_t width = GDALGetRasterXSize(dataset);
  const int32_t height = GDALGetRasterYSize(dataset);
  std::array<double, 6> transform{};
  GDALGetGeoTransform(dataset, transform.data());
  std::vector<float> values(static_cast<std::size_t>(width) *
                            static_cast<std::size_t>(height));
  if (GDALRasterIO(GDALGetRasterBand(dataset, 1), GF_Read, 0, 0, width, height,
                   values.data(), width, height, GDT_Float32, 0,
                   0) != CE_None) {
    return fail(errorString, QStringLiteral("Cannot read %1: %2")
                                 .arg(options.sourcePath,
                                      QString::fromUtf8(CPLGetLastErrorMsg())));
  }
  stats->width = (width - 1) / options.step + 1;
  stats->height = (height - 1) / options.step + 1;
  std::vector<std::int16_t> centimetres(
      static_cast<std::size_t>(stats->width) *
      static_cast<std::size_t>(stats->height));
  for (int32_t row = 0; row < stats->height; ++row) {
    for (int32_t column = 0; column < stats->width; ++column) {
      const std::size_t sourceRow = static_cast<std::size_t>(row) *
                                    static_cast<std::size_t>(options.step);
      const std::size_t sourceColumn = static_cast<std::size_t>(column) *
                                       static_cast<std::size_t>(options.step);
      const double metres = static_cast<double>(
          values[sourceRow * static_cast<std::size_t>(width) + sourceColumn]);
      const double value = std::round(metres * 100.0);
      if (!(std::abs(value) <= std::numeric_limits<std::int16_t>::max())) {
        return fail(errorString,
                    QStringLiteral("%1: %2 m does not fit the grid")
                        .arg(options.sourcePath)
                        .arg(metres));
      }
      centimetres[static_cast<std::size_t>(row) *
                      static_cast<std::size_t>(stats->width) +
                  static_cast<std::size_t>(column)] =
          static_cast<std::int16_t>(value);
    }
  }
  char** creation = nullptr;
  creation = CSLSetNameValue(creation, "COMPRESS", "DEFLATE");
  creation = CSLSetNameValue(creation, "PREDICTOR", "2");
  creation = CSLSetNameValue(creation, "ZLEVEL", "9");
  creation = CSLSetNameValue(creation, "TILED", "YES");
  const QByteArray output = options.outputPath.toUtf8();
  GDALDatasetH grid =
      GDALCreate(GDALGetDriverByName("GTiff"), output.constData(), stats->width,
                 stats->height, 1, GDT_Int16, creation);
  CSLDestroy(creation);
  if (grid == nullptr) {
    return fail(errorString, QStringLiteral("Cannot create %1: %2")
                                 .arg(options.outputPath,
                                      QString::fromUtf8(CPLGetLastErrorMsg())));
  }
  std::array<double, 6> gridTransform{
      transform[0], transform[1] * options.step, 0.0, transform[3],
      0.0,          transform[5] * options.step};
  GDALSetGeoTransform(grid, gridTransform.data());
  GDALSetProjection(grid, GDALGetProjectionRef(dataset));
  const char* area = GDALGetMetadataItem(dataset, "AREA_OR_POINT", nullptr);
  GDALSetMetadataItem(grid, "AREA_OR_POINT", area != nullptr ? area : "Point",
                      nullptr);
  // What PROJ's grid format calls a geoid model.
  GDALSetMetadataItem(grid, "TYPE", "VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL",
                      nullptr);
  GDALRasterBandH band = GDALGetRasterBand(grid, 1);
  GDALSetRasterScale(band, 0.01);
  GDALSetRasterOffset(band, 0.0);
  GDALSetDescription(band, "geoid_undulation");
  const bool written =
      GDALRasterIO(band, GF_Write, 0, 0, stats->width, stats->height,
                   centimetres.data(), stats->width, stats->height, GDT_Int16,
                   0, 0) == CE_None;
  GDALClose(grid);
  if (!written) {
    return fail(errorString, QStringLiteral("Cannot write %1: %2")
                                 .arg(options.outputPath,
                                      QString::fromUtf8(CPLGetLastErrorMsg())));
  }
  return true;
}

} // namespace ts::packager
