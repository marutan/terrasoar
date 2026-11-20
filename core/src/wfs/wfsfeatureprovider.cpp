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
#include "ts/wfs/wfsfeatureprovider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <QCoreApplication>

#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

constexpr double pi = 3.14159265358979323846;

/** Tiles kept for picking. */
constexpr std::size_t keptTiles = 64;

/** Page numbers go in the row's bits from here (rows < 2^20 to level 19). */
constexpr int32_t pageShift = 20;

QString statusErrorMessage(const HttpResponse& response)
{
  return QCoreApplication::translate("ts::WfsFeatureProvider",
                                     "The server answered %1.")
      .arg(response.errorString);
}

QString notFeaturesMessage(const HttpResponse& response)
{
  QString error;
  parseWfsFeatures(response.body,
                   QString::fromLatin1(response.header("content-type")),
                   &error);
  if (error.isEmpty()) {
    error = QCoreApplication::translate(
        "ts::WfsFeatureProvider",
        "The server sent something that isn't features.");
  }
  return error;
}

/** Distance from @p p to segment @p a–@p b in degrees, longitude scaled. */
double segmentDistance(const GeoPoint& p, const GeoPoint& a, const GeoPoint& b,
                       double lonScale)
{
  const double ax = (a.lonDeg - p.lonDeg) * lonScale;
  const double ay = a.latDeg - p.latDeg;
  const double bx = (b.lonDeg - p.lonDeg) * lonScale;
  const double by = b.latDeg - p.latDeg;
  const double dx = bx - ax;
  const double dy = by - ay;
  const double length2 = dx * dx + dy * dy;
  const double t = length2 > 0.0
                       ? std::clamp(-(ax * dx + ay * dy) / length2, 0.0, 1.0)
                       : 0.0;
  return std::hypot(ax + t * dx, ay + t * dy);
}

/** True if @p p is inside ring @p ring (even-odd). */
bool insideRing(const GeoPoint& p, const std::vector<GeoPoint>& ring)
{
  bool inside = false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const GeoPoint& a = ring[i];
    const GeoPoint& b = ring[j];
    if ((a.latDeg > p.latDeg) != (b.latDeg > p.latDeg) &&
        p.lonDeg < (b.lonDeg - a.lonDeg) * (p.latDeg - a.latDeg) /
                           (b.latDeg - a.latDeg) +
                       a.lonDeg) {
      inside = !inside;
    }
  }
  return inside;
}

bool under(const Feature& feature, const GeoPoint& p, double tolerance)
{
  const double lonScale = std::max(std::cos(p.latDeg * pi / 180.0), 0.01);
  switch (feature.type) {
  case Feature::Type::Points:
    for (const std::vector<GeoPoint>& part : feature.parts) {
      for (const GeoPoint& point : part) {
        if (segmentDistance(p, point, point, lonScale) <= tolerance) {
          return true;
        }
      }
    }
    return false;
  case Feature::Type::LineStrings:
    for (const std::vector<GeoPoint>& part : feature.parts) {
      for (std::size_t i = 0; i + 1 < part.size(); ++i) {
        if (segmentDistance(p, part[i], part[i + 1], lonScale) <= tolerance) {
          return true;
        }
      }
    }
    return false;
  case Feature::Type::Polygon: {
    if (feature.parts.empty() || feature.parts.front().size() < 3 ||
        !insideRing(p, feature.parts.front())) {
      return false;
    }
    for (std::size_t hole = 1; hole < feature.parts.size(); ++hole) {
      if (feature.parts[hole].size() >= 3 &&
          insideRing(p, feature.parts[hole])) {
        return false;
      }
    }
    return true;
  }
  }
  return false;
}

} // namespace

WfsFeatureProvider::WfsFeatureProvider(WfsSource source,
                                       std::shared_ptr<NetworkClient> network,
                                       WebTileSettings settings)
    : m_source(std::move(source)),
      m_rules(styleRulesFromJson(m_source.styleRules)),
      m_fetcher(std::make_unique<CachedTileFetcher>(
          m_source.id(), m_source.url.host(), std::move(network),
          std::move(settings),
          CachedTileFetcher::Messages{statusErrorMessage, notFeaturesMessage,
                                      false},
          [this](const ProviderStatus& status) { reportStatus(status); }))
{
}

WfsFeatureProvider::~WfsFeatureProvider() = default;

QString WfsFeatureProvider::id() const
{
  return m_source.id();
}

QString WfsFeatureProvider::name() const
{
  return m_source.title.isEmpty() ? m_source.typeName : m_source.title;
}

int32_t WfsFeatureProvider::maximumLevel() const
{
  return TileKey::maximumLevel;
}

int32_t WfsFeatureProvider::preferredLoaderThreads() const
{
  return std::min(m_fetcher->settings().loaderThreads, 4);
}

WebTileStatistics WfsFeatureProvider::statistics() const
{
  return m_fetcher->statistics();
}

TileKey WfsFeatureProvider::pageKey(TileKey key, int32_t page)
{
  return TileKey{key.level, key.x, key.y | (page << pageShift)};
}

TileKey WfsFeatureProvider::storedKey(TileKey key) const
{
  const int32_t level = m_source.storedLevel(key.level);
  if (level < 0) {
    return key;
  }
  TileKey stored = key;
  while (stored.level > level) {
    stored = stored.parent();
  }
  return stored;
}

std::optional<FeatureTile>
WfsFeatureProvider::loadFeatures(TileKey key,
                                 const std::atomic<bool>& cancelled)
{
  if (m_source.storedLevel(key.level) < 0) {
    return std::nullopt; // zoomed out too far for this layer
  }
  const TileKey stored = storedKey(key);
  const GeoRect rect = stored.bounds();
  if (m_source.bounds && (rect.eastDeg <= m_source.bounds->westDeg ||
                          rect.westDeg >= m_source.bounds->eastDeg ||
                          rect.northDeg <= m_source.bounds->southDeg ||
                          rect.southDeg >= m_source.bounds->northDeg)) {
    return std::nullopt;
  }
  FeatureTile tile;
  tile.key = stored;
  bool more = false;
  for (int32_t page = 0; page < m_source.maximumPages; ++page) {
    std::optional<WfsPage> parsed;
    const FetchOutcome outcome = m_fetcher->fetch(
        pageKey(stored, page),
        getFeatureUrl(m_source, rect,
                      static_cast<int64_t>(page) * m_source.pageSize),
        [this, &parsed](const QByteArray& data) {
          QString error;
          parsed = parseWfsFeatures(data, m_source.outputFormat, &error);
          return parsed.has_value();
        },
        cancelled);
    if (outcome == FetchOutcome::Cancelled) {
      return std::nullopt;
    }
    if (outcome != FetchOutcome::Data || !parsed) {
      if (page == 0) {
        return std::nullopt;
      }
      break;
    }
    tile.features.insert(tile.features.end(),
                         std::make_move_iterator(parsed->features.begin()),
                         std::make_move_iterator(parsed->features.end()));
    more = parsed->returned >= m_source.pageSize;
    if (!m_source.paging || !more) {
      break;
    }
  }
  if (more) {
    reportStatus(ProviderStatus{
        ProviderStatus::State::Ok,
        QCoreApplication::translate(
            "ts::WfsFeatureProvider",
            "Not all features are shown here: zoom in to see them all.")});
  }
  applyStyleRules(tile.features, m_rules);
  keep(tile);
  return tile;
}

void WfsFeatureProvider::keep(const FeatureTile& tile)
{
  const std::lock_guard<std::mutex> lock(m_keptMutex);
  m_kept.remove_if(
      [&tile](const FeatureTile& kept) { return kept.key == tile.key; });
  m_kept.push_front(tile);
  while (m_kept.size() > keptTiles) {
    m_kept.pop_back();
  }
}

std::vector<Feature> WfsFeatureProvider::featuresAt(const GeoPoint& point,
                                                    double toleranceDeg) const
{
  const std::lock_guard<std::mutex> lock(m_keptMutex);
  std::vector<Feature> found;
  std::vector<QString> seen;
  for (const FeatureTile& tile : m_kept) {
    const GeoRect rect = tile.key.bounds();
    if (point.lonDeg < rect.westDeg || point.lonDeg > rect.eastDeg ||
        point.latDeg < rect.southDeg || point.latDeg > rect.northDeg) {
      continue;
    }
    for (const Feature& feature : tile.features) {
      // A feature in several tiles (or parts) is listed once.
      if (!feature.id.isEmpty() &&
          std::find(seen.begin(), seen.end(), feature.id) != seen.end()) {
        continue;
      }
      if (under(feature, point, toleranceDeg)) {
        found.push_back(feature);
        if (!feature.id.isEmpty()) {
          seen.push_back(feature.id);
        }
      }
    }
  }
  return found;
}

QUrl WfsFeatureProvider::serviceUrl() const
{
  return m_source.service.isEmpty() ? m_source.url : m_source.service;
}

std::optional<GeoRect> WfsFeatureProvider::dataBounds() const
{
  return m_source.bounds;
}

int64_t WfsFeatureProvider::typicalTileBytes() const
{
  return int64_t{200} * 1024;
}

RegionTileResult
WfsFeatureProvider::fetchForRegion(TileKey key, int64_t region,
                                   const std::atomic<bool>& cancelled)
{
  using Outcome = RegionTileResult::Outcome;
  RegionTileResult total;
  total.outcome = Outcome::NoData;
  if (m_source.storedLevel(key.level) < 0) {
    return total;
  }
  const TileKey stored = storedKey(key);
  const GeoRect rect = stored.bounds();
  for (int32_t page = 0; page < m_source.maximumPages; ++page) {
    int64_t returned = 0;
    const RegionTileResult result = m_fetcher->fetchForRegion(
        pageKey(stored, page),
        getFeatureUrl(m_source, rect,
                      static_cast<int64_t>(page) * m_source.pageSize),
        region,
        [this, &returned](const QByteArray& data) {
          QString error;
          const std::optional<WfsPage> parsed =
              parseWfsFeatures(data, m_source.outputFormat, &error);
          returned = parsed ? parsed->returned : 0;
          return parsed.has_value();
        },
        cancelled);
    switch (result.outcome) {
    case Outcome::Cancelled:
    case Outcome::Failed:
      return result;
    case Outcome::NoData:
      return total;
    case Outcome::Downloaded:
    case Outcome::Revalidated:
    case Outcome::AlreadyCached:
      if (total.outcome == Outcome::NoData ||
          result.outcome == Outcome::Downloaded) {
        total.outcome = result.outcome;
      }
      total.bytes += result.bytes;
      break;
    }
    if (result.outcome == Outcome::AlreadyCached) {
      // Not read again: read the stored page (no request) to know whether
      // another follows.
      m_fetcher->fetch(
          pageKey(stored, page),
          getFeatureUrl(m_source, rect,
                        static_cast<int64_t>(page) * m_source.pageSize),
          [this, &returned](const QByteArray& data) {
            QString error;
            const std::optional<WfsPage> parsed =
                parseWfsFeatures(data, m_source.outputFormat, &error);
            returned = parsed ? parsed->returned : 0;
            return parsed.has_value();
          },
          cancelled);
    }
    if (!m_source.paging || returned < m_source.pageSize) {
      break;
    }
  }
  return total;
}

} // namespace ts
