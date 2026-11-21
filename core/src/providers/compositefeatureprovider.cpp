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

#include "ts/providers/compositefeatureprovider.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

namespace ts
{

CompositeFeatureProvider::CompositeFeatureProvider(
    std::vector<std::shared_ptr<FeatureProvider>> sources)
    : m_sources(std::move(sources))
{
}

QString CompositeFeatureProvider::id() const
{
  QStringList ids;
  for (const std::shared_ptr<FeatureProvider>& source : m_sources) {
    ids.append(source->id());
  }
  return QStringLiteral("composite:") + // ts-notr: identifier
         ids.join(QLatin1Char('|'));
}

QString CompositeFeatureProvider::name() const
{
  return m_sources.empty() ? QString() : m_sources.front()->name();
}

int32_t CompositeFeatureProvider::maximumLevel() const
{
  int32_t level = 0;
  for (const std::shared_ptr<FeatureProvider>& source : m_sources) {
    level = std::max(level, source->maximumLevel());
  }
  return level;
}

int32_t CompositeFeatureProvider::preferredLoaderThreads() const
{
  return m_sources.empty() ? 0 : m_sources.front()->preferredLoaderThreads();
}

std::optional<FeatureTile>
CompositeFeatureProvider::loadFeatures(TileKey key,
                                       const std::atomic<bool>& cancelled)
{
  std::optional<FeatureTile> tile;
  for (const std::shared_ptr<FeatureProvider>& source : m_sources) {
    if (cancelled.load()) {
      return std::nullopt;
    }
    std::optional<FeatureTile> part = source->loadFeatures(key, cancelled);
    if (!part) {
      continue;
    }
    if (!tile) {
      tile = std::move(part);
      continue;
    }
    if (part->key != tile->key) {
      continue; // Not stored alike: left out rather than misplaced.
    }
    for (Feature& feature : part->features) {
      tile->features.push_back(std::move(feature));
    }
  }
  return tile;
}

TileKey CompositeFeatureProvider::storedKey(TileKey key) const
{
  return m_sources.empty() ? key : m_sources.front()->storedKey(key);
}

std::vector<Feature>
CompositeFeatureProvider::featuresAt(GeoPoint point, double toleranceDeg) const
{
  std::vector<Feature> found;
  for (const std::shared_ptr<FeatureProvider>& source : m_sources) {
    std::vector<Feature> part = source->featuresAt(point, toleranceDeg);
    for (Feature& feature : part) {
      found.push_back(std::move(feature));
    }
  }
  return found;
}

std::vector<Feature>
CompositeFeatureProvider::labelPoints(const QString& attribute,
                                      const std::vector<TileKey>& tiles) const
{
  std::vector<Feature> points;
  for (const std::shared_ptr<FeatureProvider>& source : m_sources) {
    std::vector<Feature> part = source->labelPoints(attribute, tiles);
    for (Feature& feature : part) {
      points.push_back(std::move(feature));
    }
  }
  return points;
}

} // namespace ts
