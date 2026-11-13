// SPDX-License-Identifier: GPL-2.0-or-later
#include "elevationbuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

#include <QByteArray>

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

/** A grid of point samples, row 0 at 90° N, column 0 at 180° W. */
struct PointGrid {
  int width = 0;
  int height = 0;
  std::vector<float> values;

  float& at(int row, int column)
  {
    return values[static_cast<std::size_t>(row) *
                      static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(column)];
  }
  float at(int row, int column) const
  {
    return values[static_cast<std::size_t>(row) *
                      static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(column)];
  }
};

/** Overlap of the source cells with an interval, as (cell, weight) pairs. */
using Weights = std::vector<std::pair<int, double>>;

/**
 * The cells of [@p from, @p to) (in cell units) and how much of each lies
 * in it; cells outside [0, @p count) wrap round when @p wrap, else are
 * left out.
 */
// An interval's two ends.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Weights cellWeights(double from, double to, int count, bool wrap)
{
  Weights weights;
  for (int cell = static_cast<int>(std::floor(from));
       cell < static_cast<int>(std::ceil(to)); ++cell) {
    const double overlap =
        std::min(to, cell + 1.0) - std::max(from, static_cast<double>(cell));
    if (overlap <= 0.0) {
      continue;
    }
    int index = cell;
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
  RowReader(GDALRasterBandH band, int width) : m_band(band), m_width(width)
  {
    int hasNoData = 0;
    m_noData = GDALGetRasterNoDataValue(band, &hasNoData);
    m_hasNoData = hasNoData != 0;
  }

  /** Row @p row; rows before @p keepFrom are forgotten. Null on error. */
  // The row wanted, then the first still needed.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const std::vector<float>* row(int row, int keepFrom)
  {
    while (!m_rows.empty() && m_first < keepFrom) {
      m_rows.pop_front();
      ++m_first;
    }
    if (m_rows.empty()) {
      m_first = row;
    }
    while (m_first + static_cast<int>(m_rows.size()) <= row) {
      std::vector<float> values(static_cast<std::size_t>(m_width));
      const int next = m_first + static_cast<int>(m_rows.size());
      if (GDALRasterIO(m_band, GF_Read, 0, next, m_width, 1, values.data(),
                       m_width, 1, GDT_Float32, 0, 0) != CE_None) {
        return nullptr;
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
  int m_width;
  double m_noData = 0.0;
  bool m_hasNoData = false;
  int m_first = 0;
  std::deque<std::vector<float>> m_rows;
};

/** Sets each pole row to its mean: the pole is one point. */
void flattenPoles(PointGrid& grid)
{
  for (const int row : {0, grid.height - 1}) {
    double sum = 0.0;
    for (int column = 0; column < grid.width - 1; ++column) {
      sum += static_cast<double>(grid.at(row, column));
    }
    const float mean = static_cast<float>(sum / (grid.width - 1));
    for (int column = 0; column < grid.width; ++column) {
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
  for (const std::pair<int, double>& weight : rowWeights) {
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
  for (const std::pair<int, double>& weight : weights) {
    sum += weight.second * sums[static_cast<std::size_t>(weight.first)];
    total += weight.second * totals[static_cast<std::size_t>(weight.first)];
  }
  return total > 0.0 ? static_cast<float>(sum / total) : 0.0F;
}

/** The deepest level's samples, box-filtered from the source. */
bool sampleSource(const GdalDataset& source, int intervals, PointGrid* grid,
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
  const int sourceWidth = GDALGetRasterXSize(dataset);
  const int sourceHeight = GDALGetRasterYSize(dataset);
  // The source must go the whole way round for wrapping to be right.
  if (std::abs(transform[1] * sourceWidth - 360.0) > 1e-6) {
    return fail(errorString, QStringLiteral("The DEM does not span 360°"));
  }
  RowReader reader(GDALGetRasterBand(dataset, 1), sourceWidth);
  const double step = 180.0 / intervals;
  grid->width = 2 * intervals + 1;
  grid->height = intervals + 1;
  grid->values.assign(static_cast<std::size_t>(grid->width) *
                          static_cast<std::size_t>(grid->height),
                      0.0F);
  // Horizontal weights are the same for every row.
  std::vector<Weights> columnWeights;
  for (int column = 0; column < grid->width; ++column) {
    const double lon = -180.0 + column * step;
    columnWeights.push_back(cellWeights(
        (lon - step / 2.0 - transform[0]) / transform[1],
        (lon + step / 2.0 - transform[0]) / transform[1], sourceWidth, true));
  }
  std::vector<double> sums(static_cast<std::size_t>(sourceWidth));
  std::vector<double> totals(static_cast<std::size_t>(sourceWidth));
  for (int row = 0; row < grid->height; ++row) {
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
    for (int column = 0; column < grid->width; ++column) {
      grid->at(row, column) = weightedMean(
          columnWeights[static_cast<std::size_t>(column)], sums, totals);
    }
  }
  flattenPoles(*grid);
  return true;
}

/** The next coarser level: [1 2 1] / 4 each way, every other sample. */
PointGrid coarser(const PointGrid& fine)
{
  PointGrid grid;
  grid.width = (fine.width - 1) / 2 + 1;
  grid.height = (fine.height - 1) / 2 + 1;
  grid.values.resize(static_cast<std::size_t>(grid.width) *
                     static_cast<std::size_t>(grid.height));
  const int period = fine.width - 1; // the last column repeats the first
  constexpr std::array<double, 3> taps{0.25, 0.5, 0.25};
  for (int row = 0; row < grid.height; ++row) {
    for (int column = 0; column < grid.width; ++column) {
      double sum = 0.0;
      for (std::size_t i = 0; i < taps.size(); ++i) {
        const int y =
            std::clamp(2 * row + static_cast<int>(i) - 1, 0, fine.height - 1);
        for (std::size_t j = 0; j < taps.size(); ++j) {
          const int x =
              ((2 * column + static_cast<int>(j) - 1) % period + period) %
              period;
          sum += taps.at(i) * taps.at(j) * static_cast<double>(fine.at(y, x));
        }
      }
      grid.at(row, column) = static_cast<float>(sum);
    }
  }
  flattenPoles(grid);
  return grid;
}

/** Writes every tile of @p level from its grid. */
bool writeLevel(const PointGrid& grid, int level, int samples,
                SqliteTileStoreWriter* writer, ElevationStats* stats,
                QString* errorString)
{
  const int intervals = samples - 1;
  const std::size_t n = static_cast<std::size_t>(samples);
  std::vector<float> heights(n * n);
  for (int y = 0; y < static_cast<int>(TileKey::rows(level)); ++y) {
    for (int x = 0; x < static_cast<int>(TileKey::columns(level)); ++x) {
      for (int row = 0; row < samples; ++row) {
        for (int column = 0; column < samples; ++column) {
          const float value =
              grid.at(y * intervals + row, x * intervals + column);
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
  if (options.samples < 3 || options.maxLevel < 0 || options.maxLevel > 10) {
    return fail(errorString, QStringLiteral("Bad elevation options"));
  }
  const GdalDataset source(options.sourcePath);
  if (!source.isOpen()) {
    return fail(errorString,
                QStringLiteral("Cannot read %1: %2")
                    .arg(options.sourcePath, source.errorString()));
  }
  const int intervals = (options.samples - 1) << options.maxLevel;
  PointGrid grid;
  if (!sampleSource(source, intervals, &grid, progress, errorString)) {
    return false;
  }
  const std::pair<std::vector<float>::const_iterator,
                  std::vector<float>::const_iterator>
      range = std::minmax_element(grid.values.begin(), grid.values.end());
  stats->minHeightM = static_cast<double>(*range.first);
  stats->maxHeightM = static_cast<double>(*range.second);

  SqliteTileStoreInfo info;
  info.format = QStringLiteral("heights");
  info.tileSizePx = options.samples;
  info.minLevel = 0;
  info.maxLevel = options.maxLevel;
  info.name = options.name;
  info.verticalDatum = options.verticalDatum;
  SqliteTileStoreWriter writer(options.outputPath, info);
  if (!writer.errorString().isEmpty()) {
    return fail(errorString, writer.errorString());
  }
  for (int level = options.maxLevel; level >= 0; --level) {
    progress(QStringLiteral("Writing elevation level %1").arg(level));
    if (!writeLevel(grid, level, options.samples, &writer, stats,
                    errorString)) {
      return false;
    }
    if (level > 0) {
      grid = coarser(grid);
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
  const int width = GDALGetRasterXSize(dataset);
  const int height = GDALGetRasterYSize(dataset);
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
  for (int row = 0; row < stats->height; ++row) {
    for (int column = 0; column < stats->width; ++column) {
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
