// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/sqliteelevationprovider.h"

#include <cmath>
#include <utility>
#include <vector>

#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/heightgrid.h"

namespace ts
{

SqliteElevationProvider::SqliteElevationProvider(
    const QString& path, std::shared_ptr<const GeoidModel> geoid)
    : m_store(path), m_geoid(std::move(geoid))
{
  if (!m_store.isValid()) {
    m_error = m_store.errorString();
  } else if (m_store.info().format != QLatin1String("heights") ||
             m_store.info().tileSizePx < 2) {
    m_error = QStringLiteral("%1 is not a store of height grids").arg(path);
  } else if (!m_store.info().verticalDatum.isEmpty() &&
             (!m_geoid || !m_geoid->isValid())) {
    m_error = QStringLiteral("%1: no geoid model for its vertical datum %2")
                  .arg(path, m_store.info().verticalDatum);
  }
}

SqliteElevationProvider::~SqliteElevationProvider() = default;

QString SqliteElevationProvider::id() const
{
  // ts-notr: identifier
  return QStringLiteral("sqliteheights:") + m_store.id() +
         (m_geoid ? QStringLiteral("|") + m_geoid->id() : QString());
}

std::optional<HeightTile>
SqliteElevationProvider::loadStoredHeights(TileKey key)
{
  if (!isValid()) {
    return std::nullopt;
  }
  const int samples = m_store.info().tileSizePx;
  std::optional<std::vector<float>> heights =
      decodeHeightGrid(m_store.loadEncoded(key), samples);
  if (!heights) {
    return std::nullopt;
  }
  HeightTile tile;
  tile.key = key;
  tile.samples = samples;
  tile.heightsM = std::move(*heights);
  return tile;
}

std::optional<HeightTile>
SqliteElevationProvider::loadHeights(TileKey key,
                                     const std::atomic<bool>& cancelled)
{
  if (!isValid() || !key.isValid() || key.level < m_store.info().minLevel) {
    return std::nullopt;
  }
  TileKey stored = key;
  while (stored.level > m_store.info().maxLevel) {
    stored = stored.parent();
  }
  std::optional<HeightTile> found = loadStoredHeights(stored);
  if (!found || cancelled.load()) {
    return std::nullopt;
  }
  HeightTile tile = stored.level == key.level
                        ? std::move(*found)
                        : resampleHeightTile(*found, key, found->samples);
  if (m_geoid) {
    const int samples = tile.samples;
    std::vector<GeoPoint> points;
    points.reserve(tile.heightsM.size());
    for (int row = 0; row < samples; ++row) {
      for (int column = 0; column < samples; ++column) {
        points.push_back(heightSamplePoint(key, samples, row, column));
      }
    }
    const std::vector<double> undulations = m_geoid->undulationsM(points);
    tile.seaLevelM.assign(tile.heightsM.size(), 0.0F);
    for (std::size_t i = 0; i < undulations.size(); ++i) {
      // A grid always covers the Earth; never let one bad value through.
      if (std::isfinite(undulations[i])) {
        tile.heightsM[i] += static_cast<float>(undulations[i]);
        tile.seaLevelM[i] = static_cast<float>(undulations[i]);
      }
    }
  }
  return tile;
}

} // namespace ts
