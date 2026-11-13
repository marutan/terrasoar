// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Several elevation sources as one, the first taking priority.
 */
#ifndef TS_ELEVATION_COMPOSITEELEVATIONPROVIDER_H
#define TS_ELEVATION_COMPOSITEELEVATIONPROVIDER_H

#include <memory>
#include <optional>
#include <vector>

#include <QString>

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * Elevation sources in priority order (the layer list's topmost elevation
 * layer first) as one source: each sample of a tile comes from the first
 * source with data there (not NaN), so a detailed local DEM is used where
 * it has heights and the global one around it. The tile's samples all lie
 * on the same grid whatever source they come from, so there are no cracks
 * at a source's edge (heights may step there, as the sources differ).
 * Samples no source has are sea level; a tile without any data is
 * nothing. Sea level comes from the first source that gives it.
 *
 * @threadsafe
 */
class CompositeElevationProvider : public ElevationProvider
{
public:
  /** Combines @p sources, most important first (none null). */
  explicit CompositeElevationProvider(
      std::vector<std::shared_ptr<ElevationProvider>> sources);

  /** Returns the sources, most important first. */
  const std::vector<std::shared_ptr<ElevationProvider>>& sources() const
  {
    return m_sources;
  }

  /** The sources' ids, in order. */
  QString id() const override;
  /** The first source's name. */
  QString name() const override;
  /** The deepest of the sources'. */
  int maximumLevel() const override;
  int preferredLoaderThreads() const override;

  /** Heights above the WGS 84 ellipsoid, never NaN. */
  std::optional<HeightTile>
  loadHeights(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  std::vector<std::shared_ptr<ElevationProvider>> m_sources;
};

} // namespace ts

#endif // TS_ELEVATION_COMPOSITEELEVATIONPROVIDER_H
