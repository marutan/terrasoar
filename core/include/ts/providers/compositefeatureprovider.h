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
 * Several feature sources read as one: the regional packs of one dataset.
 */
#ifndef TS_PROVIDERS_COMPOSITEFEATUREPROVIDER_H
#define TS_PROVIDERS_COMPOSITEFEATUREPROVIDER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QString>

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * Reads several feature sources stored the same way (the same tables and
 * levels, as VMAP0's regional packs of one theme are) as one: a tile's
 * features are those of every source for it.
 *
 * @threadsafe
 */
class CompositeFeatureProvider : public FeatureProvider
{
public:
  /** Combines @p sources (at least one), all stored alike. */
  explicit CompositeFeatureProvider(
      std::vector<std::shared_ptr<FeatureProvider>> sources);

  QString id() const override;
  QString name() const override;
  int32_t maximumLevel() const override;
  int32_t preferredLoaderThreads() const override;

  /** The features of every source for @p key, in the sources' order. */
  std::optional<FeatureTile>
  loadFeatures(TileKey key, const std::atomic<bool>& cancelled) override;

  /** The first source's stored key (they are stored alike). */
  TileKey storedKey(TileKey key) const override;

  std::vector<Feature> featuresAt(GeoPoint point,
                                  double toleranceDeg) const override;

  std::vector<Feature>
  labelPoints(const QString& attribute,
              const std::vector<TileKey>& tiles) const override;

  /** Returns the sources. */
  const std::vector<std::shared_ptr<FeatureProvider>>& sources() const
  {
    return m_sources;
  }

private:
  std::vector<std::shared_ptr<FeatureProvider>> m_sources;
};

} // namespace ts

#endif // TS_PROVIDERS_COMPOSITEFEATUREPROVIDER_H
