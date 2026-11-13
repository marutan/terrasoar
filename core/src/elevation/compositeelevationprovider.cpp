// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/compositeelevationprovider.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QStringList>

#include "ts/elevation/heightgrid.h"

namespace ts
{

namespace
{

/** Index of (@p row, @p column) in a grid @p width wide. */
// Row then column, as everywhere in a grid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::size_t gridIndex(int row, int column, int width)
{
  return static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(column);
}

/** Returns @p tile with @p samples² samples on its key's grid. */
HeightTile withSamples(const HeightTile& tile, int samples)
{
  if (tile.samples == samples) {
    return tile;
  }
  HeightTile resampled;
  resampled.key = tile.key;
  resampled.samples = samples;
  const std::size_t count = gridIndex(samples, 0, samples);
  resampled.heightsM.resize(count);
  if (!tile.seaLevelM.empty()) {
    resampled.seaLevelM.resize(count);
  }
  for (int row = 0; row < samples; ++row) {
    for (int column = 0; column < samples; ++column) {
      const std::size_t i = gridIndex(row, column, samples);
      const GeoPoint point = heightSamplePoint(tile.key, samples, row, column);
      // NaN (no data) in any of the four neighbours makes the sample NaN.
      resampled.heightsM[i] =
          static_cast<float>(sampleHeightM(tile, point.latDeg, point.lonDeg)
                                 .value_or(std::nan("")));
      if (!tile.seaLevelM.empty()) {
        resampled.seaLevelM[i] = static_cast<float>(
            sampleSeaLevelM(tile, point.latDeg, point.lonDeg).value_or(0.0));
      }
    }
  }
  return resampled;
}

/**
 * Fills the samples of @p tile without data from @p below (same grid),
 * and its sea level if it has none; returns how many were filled.
 */
std::size_t fillFrom(HeightTile& tile, const HeightTile& below)
{
  std::size_t filled = 0;
  for (std::size_t i = 0; i < tile.heightsM.size(); ++i) {
    if (std::isnan(tile.heightsM[i]) && !std::isnan(below.heightsM[i])) {
      tile.heightsM[i] = below.heightsM[i];
      ++filled;
    }
  }
  if (tile.seaLevelM.empty() && !below.seaLevelM.empty()) {
    tile.seaLevelM = below.seaLevelM;
  }
  return filled;
}

} // namespace

CompositeElevationProvider::CompositeElevationProvider(
    std::vector<std::shared_ptr<ElevationProvider>> sources)
    : m_sources(std::move(sources))
{
}

QString CompositeElevationProvider::id() const
{
  QStringList ids;
  for (const std::shared_ptr<ElevationProvider>& source : m_sources) {
    ids << source->id();
  }
  // ts-notr: identifier
  return QStringLiteral("composite:[%1]").arg(ids.join(QLatin1Char(';')));
}

QString CompositeElevationProvider::name() const
{
  return m_sources.empty() ? QString() : m_sources.front()->name();
}

int CompositeElevationProvider::maximumLevel() const
{
  int level = 0;
  for (const std::shared_ptr<ElevationProvider>& source : m_sources) {
    level = std::max(level, source->maximumLevel());
  }
  return level;
}

int CompositeElevationProvider::preferredLoaderThreads() const
{
  int threads = 0;
  for (const std::shared_ptr<ElevationProvider>& source : m_sources) {
    threads = std::max(threads, source->preferredLoaderThreads());
  }
  return threads;
}

std::optional<HeightTile>
CompositeElevationProvider::loadHeights(TileKey key,
                                        const std::atomic<bool>& cancelled)
{
  std::optional<HeightTile> result;
  std::size_t missing = 0;
  for (const std::shared_ptr<ElevationProvider>& source : m_sources) {
    if (cancelled.load()) {
      return std::nullopt;
    }
    std::optional<HeightTile> loaded = source->loadHeights(key, cancelled);
    if (!loaded || loaded->samples < 2 ||
        loaded->heightsM.size() !=
            gridIndex(loaded->samples, 0, loaded->samples)) {
      continue;
    }
    if (!result) {
      // The first source with data sets the grid.
      result = std::move(loaded);
      missing = static_cast<std::size_t>(
          std::count_if(result->heightsM.begin(), result->heightsM.end(),
                        [](float h) { return std::isnan(h); }));
    } else {
      missing -= fillFrom(*result, withSamples(*loaded, result->samples));
    }
    if (missing == 0 && !result->seaLevelM.empty()) {
      break;
    }
  }
  if (!result) {
    return std::nullopt;
  }
  result->key = key;
  // What no source has is sea level.
  for (std::size_t i = 0; i < result->heightsM.size(); ++i) {
    if (std::isnan(result->heightsM[i])) {
      result->heightsM[i] =
          result->seaLevelM.empty() ? 0.0F : result->seaLevelM[i];
    }
  }
  return result;
}

} // namespace ts
