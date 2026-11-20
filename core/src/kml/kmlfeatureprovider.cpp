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
#include "ts/kml/kmlfeatureprovider.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include <QStringList>

#include "ts/kml/kmlreader.h"
#include "ts/vector/featurepick.h"

namespace ts
{

namespace
{

/** The level features are stored at, at most: they are whole anyway. */
constexpr int32_t storedLevelLimit = 6;

/** Yellow, as KML's default pushpin. */
const QColor defaultIconColour(255, 214, 0);

/** Size of a point for an icon of scale 1, pixels. */
constexpr double pointSizePx = 9.0;

std::mutex& documentsMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::map<QString, std::weak_ptr<const KmlDocument>>& documents()
{
  static std::map<QString, std::weak_ptr<const KmlDocument>> map;
  return map;
}

/** @p coordinates as points, longitudes made continuous. */
std::vector<GeoPoint> unwrapped(const std::vector<KmlCoordinate>& coordinates)
{
  std::vector<GeoPoint> points;
  points.reserve(coordinates.size());
  double previous = 0.0;
  for (const KmlCoordinate& c : coordinates) {
    double lon = c.lonDeg;
    if (!points.empty()) {
      while (lon - previous > 180.0) {
        lon -= 360.0;
      }
      while (lon - previous < -180.0) {
        lon += 360.0;
      }
    }
    points.push_back(GeoPoint{c.latDeg, lon});
    previous = lon;
  }
  return points;
}

void addGeometry(const KmlGeometry& geometry, const Feature& base,
                 std::vector<Feature>& features)
{
  Feature feature = base;
  switch (geometry.kind) {
  case KmlGeometry::Kind::Point:
    feature.type = Feature::Type::Points;
    for (const GeoPoint& point : unwrapped(geometry.coordinates)) {
      feature.parts.push_back({point});
    }
    break;
  case KmlGeometry::Kind::LineString:
  case KmlGeometry::Kind::LinearRing:
  case KmlGeometry::Kind::Track:
    feature.type = Feature::Type::LineStrings;
    feature.parts.push_back(unwrapped(geometry.coordinates));
    break;
  case KmlGeometry::Kind::Polygon:
    feature.type = Feature::Type::Polygon;
    feature.parts.push_back(unwrapped(geometry.coordinates));
    for (const std::vector<KmlCoordinate>& hole : geometry.inner) {
      feature.parts.push_back(unwrapped(hole));
    }
    break;
  case KmlGeometry::Kind::MultiGeometry:
    for (const KmlGeometry& child : geometry.children) {
      addGeometry(child, base, features);
    }
    return;
  }
  const std::size_t needed = feature.type == Feature::Type::LineStrings ? 2 : 1;
  if (!feature.parts.empty() && feature.parts.front().size() >= needed) {
    features.push_back(std::move(feature));
  }
}

GeoRect boundsOf(const Feature& feature)
{
  GeoRect rect{1e9, 1e9, -1e9, -1e9};
  for (const std::vector<GeoPoint>& part : feature.parts) {
    for (const GeoPoint& p : part) {
      rect.westDeg = std::min(rect.westDeg, p.lonDeg);
      rect.eastDeg = std::max(rect.eastDeg, p.lonDeg);
      rect.southDeg = std::min(rect.southDeg, p.latDeg);
      rect.northDeg = std::max(rect.northDeg, p.latDeg);
    }
  }
  return rect;
}

Feature shifted(const Feature& feature, double byDeg)
{
  Feature copy = feature;
  for (std::vector<GeoPoint>& part : copy.parts) {
    for (GeoPoint& p : part) {
      p.lonDeg += byDeg;
    }
  }
  return copy;
}

bool overlaps(const GeoRect& a, const GeoRect& b)
{
  return a.westDeg <= b.eastDeg && a.eastDeg >= b.westDeg &&
         a.southDeg <= b.northDeg && a.northDeg >= b.southDeg;
}

} // namespace

std::shared_ptr<const KmlDocument> sharedKmlDocument(const QString& path,
                                                     QString* errorString)
{
  const std::lock_guard<std::mutex> lock(documentsMutex());
  std::weak_ptr<const KmlDocument>& slot = documents()[path];
  std::shared_ptr<const KmlDocument> document = slot.lock();
  if (document) {
    return document;
  }
  std::optional<KmlDocument> read = readKmlFile(path, errorString);
  if (!read) {
    return nullptr;
  }
  document = std::make_shared<const KmlDocument>(std::move(*read));
  slot = document;
  return document;
}

const KmlFeature* kmlContainer(const KmlDocument& document,
                               const QString& containerPath)
{
  const KmlFeature* feature = &document.root;
  const QStringList steps =
      containerPath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
  for (const QString& step : steps) {
    bool ok = false;
    const int32_t index = step.toInt(&ok);
    if (!ok || index < 0 ||
        static_cast<std::size_t>(index) >= feature->children.size()) {
      return nullptr;
    }
    feature = &feature->children.at(static_cast<std::size_t>(index));
  }
  return feature;
}

QJsonObject kmlLayerSource(const QString& path, const QString& containerPath)
{
  return QJsonObject{{QStringLiteral("kind"), QStringLiteral("kml")},
                     {QStringLiteral("path"), path},
                     {QStringLiteral("container"), containerPath}};
}

namespace
{

void addBillboards(const KmlGeometry& geometry, const KmlBillboard& base,
                   std::vector<KmlBillboard>& billboards)
{
  if (geometry.kind == KmlGeometry::Kind::Point &&
      !geometry.coordinates.empty()) {
    KmlBillboard billboard = base;
    billboard.lonDeg = geometry.coordinates.front().lonDeg;
    billboard.latDeg = geometry.coordinates.front().latDeg;
    billboard.altM = geometry.coordinates.front().altM;
    billboard.altitudeMode = geometry.altitudeMode;
    if (!billboards.empty() && billboards.back().placemark == base.placemark) {
      billboard.label.clear(); // the label once, on the first
    }
    billboards.push_back(billboard);
  }
  for (const KmlGeometry& child : geometry.children) {
    addBillboards(child, base, billboards);
  }
}

} // namespace

std::vector<KmlBillboard> kmlBillboards(const KmlDocument& document,
                                        const KmlFeature& placemark,
                                        const KmlStyle& style)
{
  std::vector<KmlBillboard> billboards;
  if (!placemark.geometry) {
    return billboards;
  }
  KmlBillboard base;
  base.placemark = &placemark;
  base.label = placemark.name;
  if (style.icon) {
    base.icon = resolveKmlHref(document, style.icon->href);
    base.tint = style.icon->color;
    base.scale = style.icon->scale;
    base.headingDeg = style.icon->heading;
    base.hotSpot = style.icon->hotSpot;
  }
  if (style.label) {
    base.labelColor = style.label->color;
    base.labelScale = style.label->scale;
  }
  addBillboards(*placemark.geometry, base, billboards);
  return billboards;
}

std::vector<Feature> kmlPlacemarkFeatures(const KmlFeature& placemark,
                                          const KmlStyle& style)
{
  std::vector<Feature> features;
  if (!placemark.geometry) {
    return features;
  }
  Feature base;
  base.id = placemark.id;
  base.attributes.insert(QStringLiteral("name"), placemark.name);
  if (!placemark.description.isEmpty()) {
    base.attributes.insert(QStringLiteral("description"),
                           placemark.description);
  }
  for (const std::pair<QString, QString>& data : placemark.extendedData) {
    base.attributes.insert(data.first, data.second);
  }
  const KmlLineStyle line = style.line.value_or(KmlLineStyle{});
  const KmlPolyStyle poly = style.poly.value_or(KmlPolyStyle{});
  FeatureStyle look;
  look.outline = line.color;
  look.outlineWidthPx = line.width;
  look.fill = poly.fill ? poly.color : QColor(0, 0, 0, 0);
  if (style.icon) {
    const bool tinted = style.icon->color != QColor(255, 255, 255);
    look.pointSizePx = pointSizePx * style.icon->scale;
    if (tinted) {
      look.outline = style.icon->color;
    }
  }
  std::vector<Feature> parts;
  addGeometry(*placemark.geometry, base, parts);
  for (Feature& feature : parts) {
    FeatureStyle own = look;
    if (feature.type == Feature::Type::Points) {
      // Dots in the icon's colour, yellow for the default pushpin.
      own.outline = style.icon && style.icon->color != QColor(255, 255, 255)
                        ? style.icon->color
                        : defaultIconColour;
      own.pointSizePx = pointSizePx * (style.icon ? style.icon->scale : 1.0);
    } else if (feature.type == Feature::Type::Polygon && !poly.outline) {
      own.outline = QColor(0, 0, 0, 0);
    }
    feature.style = own;
    const GeoRect bounds = boundsOf(feature);
    if (bounds.eastDeg > 180.0) {
      features.push_back(shifted(feature, -360.0));
    }
    if (bounds.westDeg < -180.0) {
      features.push_back(shifted(feature, 360.0));
    }
    features.push_back(std::move(feature));
  }
  return features;
}

KmlFeatureProvider::KmlFeatureProvider(
    std::shared_ptr<const KmlDocument> document, const QString& containerPath)
    : m_document(std::move(document)), m_containerPath(containerPath)
{
  const KmlFeature* container =
      m_document ? kmlContainer(*m_document, containerPath) : nullptr;
  if (container == nullptr) {
    return;
  }
  m_name = container->name;
  KmlStyleResolver resolver(*m_document);
  const std::function<void(const KmlFeature&)> add =
      [&](const KmlFeature& placemark) {
        const KmlStyle style = resolver.resolve(placemark);
        for (Feature& feature : kmlPlacemarkFeatures(placemark, style)) {
          const GeoRect bounds = boundsOf(feature);
          m_entries.push_back(Entry{std::move(feature), bounds, &placemark});
        }
        std::vector<KmlBillboard> points =
            kmlBillboards(*m_document, placemark, style);
        m_billboards.insert(m_billboards.end(), points.begin(), points.end());
      };
  for (const KmlFeature& child : container->children) {
    if (child.kind == KmlFeature::Kind::Placemark) {
      add(child);
    }
  }
}

QString KmlFeatureProvider::id() const
{
  return QStringLiteral("kml:%1|%2")
      .arg(m_document ? m_document->path : QString(), m_containerPath);
}

QString KmlFeatureProvider::name() const
{
  return m_name;
}

int32_t KmlFeatureProvider::maximumLevel() const
{
  return TileKey::maximumLevel;
}

TileKey KmlFeatureProvider::storedKey(TileKey key) const
{
  while (key.level > storedLevelLimit) {
    key = key.parent();
  }
  return key;
}

std::optional<FeatureTile>
KmlFeatureProvider::loadFeatures(TileKey key,
                                 const std::atomic<bool>& cancelled)
{
  FeatureTile tile;
  tile.key = storedKey(key);
  const GeoRect rect = tile.key.bounds();
  for (const Entry& entry : m_entries) {
    if (cancelled.load()) {
      return std::nullopt;
    }
    // Points are drawn as billboards over the view, not as dots.
    if (entry.feature.type != Feature::Type::Points &&
        overlaps(entry.bounds, rect)) {
      tile.features.push_back(entry.feature);
    }
  }
  if (tile.features.empty()) {
    return std::nullopt;
  }
  return tile;
}

std::vector<Feature> KmlFeatureProvider::featuresAt(GeoPoint point,
                                                    double toleranceDeg) const
{
  std::vector<Feature> found;
  for (const KmlFeature* placemark : placemarksAt(point, toleranceDeg)) {
    for (const Entry& entry : m_entries) {
      if (entry.placemark == placemark) {
        found.push_back(entry.feature);
        break;
      }
    }
  }
  return found;
}

std::vector<const KmlFeature*>
KmlFeatureProvider::placemarksAt(GeoPoint point, double toleranceDeg) const
{
  // Points over lines over areas, as they are drawn; then the last drawn.
  std::vector<std::pair<int32_t, const KmlFeature*>> hits;
  for (std::size_t i = m_entries.size(); i-- > 0;) {
    const Entry& entry = m_entries[i];
    const bool near = point.lonDeg >= entry.bounds.westDeg - toleranceDeg &&
                      point.lonDeg <= entry.bounds.eastDeg + toleranceDeg &&
                      point.latDeg >= entry.bounds.southDeg - toleranceDeg &&
                      point.latDeg <= entry.bounds.northDeg + toleranceDeg;
    if (!near || !featureUnder(entry.feature, point, toleranceDeg)) {
      continue;
    }
    const int32_t rank = entry.feature.type == Feature::Type::Points        ? 0
                         : entry.feature.type == Feature::Type::LineStrings ? 1
                                                                            : 2;
    hits.emplace_back(rank, entry.placemark);
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const std::pair<int32_t, const KmlFeature*>& a,
                      const std::pair<int32_t, const KmlFeature*>& b) {
                     return a.first < b.first;
                   });
  std::vector<const KmlFeature*> found;
  for (const std::pair<int32_t, const KmlFeature*>& hit : hits) {
    if (std::find(found.begin(), found.end(), hit.second) == found.end()) {
      found.push_back(hit.second);
    }
  }
  return found;
}

} // namespace ts
