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

#include "ts/kml/kmlactivity.h"
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

bool isRaised(KmlAltitudeMode mode)
{
  return mode != KmlAltitudeMode::ClampToGround &&
         mode != KmlAltitudeMode::ClampToSeaFloor;
}

void addGeometry(const KmlGeometry& geometry, const Feature& base,
                 std::vector<Feature>& features, std::vector<bool>* raised)
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
      addGeometry(child, base, features, raised);
    }
    return;
  }
  const std::size_t needed = feature.type == Feature::Type::LineStrings ? 2 : 1;
  if (!feature.parts.empty() && feature.parts.front().size() >= needed) {
    features.push_back(std::move(feature));
    if (raised != nullptr) {
      raised->push_back(geometry.kind != KmlGeometry::Kind::Point &&
                        isRaised(geometry.altitudeMode));
    }
  }
}

/** @p coordinates with continuous longitudes and no edge over a degree. */
std::vector<KmlCoordinate>
densified(const std::vector<KmlCoordinate>& coordinates)
{
  constexpr double stepDeg = 1.0;
  const std::vector<GeoPoint> points = unwrapped(coordinates);
  std::vector<KmlCoordinate> out;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const KmlCoordinate here{points[i].lonDeg, points[i].latDeg,
                             coordinates[i].altM};
    if (i > 0) {
      const KmlCoordinate& last = out.back();
      const double span = std::max(std::abs(here.lonDeg - last.lonDeg),
                                   std::abs(here.latDeg - last.latDeg));
      const int32_t steps = static_cast<int32_t>(std::ceil(span / stepDeg));
      const KmlCoordinate from = last;
      for (int32_t k = 1; k < steps; ++k) {
        const double t = static_cast<double>(k) / steps;
        out.push_back(
            KmlCoordinate{from.lonDeg + t * (here.lonDeg - from.lonDeg),
                          from.latDeg + t * (here.latDeg - from.latDeg),
                          from.altM + t * (here.altM - from.altM)});
      }
    }
    out.push_back(here);
  }
  return out;
}

void addRaised(const KmlGeometry& geometry, const KmlRaisedShape& base,
               std::vector<KmlRaisedShape>& shapes)
{
  if (geometry.kind == KmlGeometry::Kind::MultiGeometry) {
    for (const KmlGeometry& child : geometry.children) {
      addRaised(child, base, shapes);
    }
    return;
  }
  if (geometry.kind == KmlGeometry::Kind::Point ||
      !isRaised(geometry.altitudeMode) || geometry.coordinates.size() < 2) {
    return;
  }
  KmlRaisedShape shape = base;
  shape.relative = geometry.altitudeMode == KmlAltitudeMode::RelativeToGround ||
                   geometry.altitudeMode == KmlAltitudeMode::RelativeToSeaFloor;
  shape.extrude = geometry.extrude;
  shape.surface = geometry.kind == KmlGeometry::Kind::Polygon;
  shape.closed =
      shape.surface || geometry.kind == KmlGeometry::Kind::LinearRing;
  shape.rings.push_back(densified(geometry.coordinates));
  for (const std::vector<KmlCoordinate>& hole : geometry.inner) {
    shape.rings.push_back(densified(hole));
  }
  // Lines and rings fill nothing but their walls.
  if (!shape.surface && !shape.extrude) {
    shape.fill = QColor(0, 0, 0, 0);
  }
  shapes.push_back(std::move(shape));
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

namespace
{

/** Files that are copies of web documents: path → address. */
std::map<QString, QString>& webBases()
{
  static std::map<QString, QString> bases;
  return bases;
}

} // namespace

void setKmlWebBase(const QString& path, const QString& url)
{
  const std::lock_guard<std::mutex> lock(documentsMutex());
  webBases()[path] = url;
}

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
  const std::map<QString, QString>::const_iterator web = webBases().find(path);
  if (web != webBases().end()) {
    read->webBase = web->second;
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

// The file, then the container in it.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
QJsonObject kmlLayerSource(const QString& path, const QString& containerPath,
                           bool deep)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("kml"));
  source.insert(QStringLiteral("path"), path);
  source.insert(QStringLiteral("container"), containerPath);
  if (deep) {
    source.insert(QStringLiteral("deep"), true);
  }
  return source;
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
    billboard.extrude = geometry.extrude && isRaised(geometry.altitudeMode);
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
  base.stalkColor = style.line.value_or(KmlLineStyle{}).color;
  addBillboards(*placemark.geometry, base, billboards);
  return billboards;
}

std::vector<KmlRaisedShape> kmlRaisedShapes(const KmlFeature& placemark,
                                            const KmlStyle& style)
{
  std::vector<KmlRaisedShape> shapes;
  if (!placemark.geometry) {
    return shapes;
  }
  const KmlLineStyle line = style.line.value_or(KmlLineStyle{});
  const KmlPolyStyle poly = style.poly.value_or(KmlPolyStyle{});
  KmlRaisedShape base;
  base.placemark = &placemark;
  base.line = line.color;
  base.widthPx = line.width;
  base.fill = poly.fill ? poly.color : QColor(0, 0, 0, 0);
  addRaised(*placemark.geometry, base, shapes);
  // A polygon's outline only if PolyStyle says so.
  if (!poly.outline) {
    for (KmlRaisedShape& shape : shapes) {
      if (shape.closed) {
        shape.line = QColor(0, 0, 0, 0);
      }
    }
  }
  return shapes;
}

namespace
{

/** A feature with the placemark's id and attributes, no geometry yet. */
Feature placemarkBase(const KmlFeature& placemark)
{
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
  return base;
}

/** How a part of type @p type looks with @p style. */
FeatureStyle partStyle(const KmlStyle& style, Feature::Type type)
{
  const KmlLineStyle line = style.line.value_or(KmlLineStyle{});
  const KmlPolyStyle poly = style.poly.value_or(KmlPolyStyle{});
  const bool tinted = style.icon && style.icon->color != QColor(255, 255, 255);
  FeatureStyle look;
  look.outline = line.color;
  look.outlineWidthPx = line.width;
  look.fill = poly.fill ? poly.color : QColor(0, 0, 0, 0);
  if (type == Feature::Type::Points) {
    // Dots in the icon's colour, yellow for the default pushpin.
    look.outline = tinted ? style.icon->color : defaultIconColour;
    look.pointSizePx = pointSizePx * (style.icon ? style.icon->scale : 1.0);
  } else if (type == Feature::Type::Polygon && !poly.outline) {
    look.outline = QColor(0, 0, 0, 0);
  }
  return look;
}

std::vector<Feature> placemarkFeatures(const KmlFeature& placemark,
                                       const KmlStyle& style,
                                       std::vector<bool>* overlay)
{
  std::vector<Feature> features;
  if (!placemark.geometry) {
    return features;
  }
  std::vector<Feature> parts;
  std::vector<bool> raised;
  addGeometry(*placemark.geometry, placemarkBase(placemark), parts, &raised);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    Feature& feature = parts[i];
    const std::size_t before = features.size();
    feature.style = partStyle(style, feature.type);
    const GeoRect bounds = boundsOf(feature);
    if (bounds.eastDeg > 180.0) {
      features.push_back(shifted(feature, -360.0));
    }
    if (bounds.westDeg < -180.0) {
      features.push_back(shifted(feature, 360.0));
    }
    features.push_back(std::move(feature));
    if (overlay != nullptr) {
      // Points are billboards, raised shapes drawn in 3D: both over the
      // view.
      const bool over =
          raised[i] || features.back().type == Feature::Type::Points;
      overlay->insert(overlay->end(), features.size() - before, over);
    }
  }
  return features;
}

} // namespace

std::vector<Feature> kmlPlacemarkFeatures(const KmlFeature& placemark,
                                          const KmlStyle& style)
{
  return placemarkFeatures(placemark, style, nullptr);
}

namespace
{

void collectTracks(const KmlGeometry& geometry,
                   std::vector<const KmlGeometry*>& tracks)
{
  if (geometry.kind == KmlGeometry::Kind::Track) {
    tracks.push_back(&geometry);
  }
  for (const KmlGeometry& child : geometry.children) {
    collectTracks(child, tracks);
  }
}

} // namespace

KmlFeatureProvider::KmlFeatureProvider(
    std::shared_ptr<const KmlDocument> document, const QString& containerPath,
    std::optional<QDateTime> at, bool deep)
    : m_document(std::move(document)), m_containerPath(containerPath),
      m_at(std::move(at)), m_deep(deep)
{
  const KmlFeature* container =
      m_document ? kmlContainer(*m_document, containerPath) : nullptr;
  if (container == nullptr) {
    return;
  }
  m_name = container->name;
  KmlStyleResolver resolver(*m_document);
  const std::optional<KmlTime> time =
      kmlEffectiveTime(*m_document, containerPath);
  if (!shownAt(time)) {
    return;
  }
  // A placemark or screen overlay named itself (a file of one placemark;
  // a screen overlay's own layer).
  if (container->kind == KmlFeature::Kind::Placemark) {
    addPlacemark(*container, resolver);
  } else if (container->kind == KmlFeature::Kind::ScreenOverlay) {
    m_screens.push_back(container);
  }
  addChildren(*container, time, resolver);
}

bool KmlFeatureProvider::shownAt(const std::optional<KmlTime>& time) const
{
  return !m_at || !time || kmlShownAt(*time, *m_at);
}

void KmlFeatureProvider::addChildren(const KmlFeature& parent,
                                     const std::optional<KmlTime>& time,
                                     KmlStyleResolver& resolver)
{
  for (const KmlFeature& child : parent.children) {
    const std::optional<KmlTime> own = kmlNarrowTime(time, child.time);
    if (!shownAt(own)) {
      continue;
    }
    if (child.kind == KmlFeature::Kind::Placemark) {
      addPlacemark(child, resolver);
    } else if (m_deep && child.kind == KmlFeature::Kind::ScreenOverlay) {
      m_screens.push_back(&child);
    } else if (m_deep && (child.kind == KmlFeature::Kind::Document ||
                          child.kind == KmlFeature::Kind::Folder)) {
      addChildren(child, own, resolver);
    }
  }
}

void KmlFeatureProvider::addPlacemark(const KmlFeature& placemark,
                                      KmlStyleResolver& resolver)
{
  const KmlStyle style = resolver.resolve(placemark);
  std::vector<bool> overlay;
  std::vector<Feature> features = placemarkFeatures(placemark, style, &overlay);
  for (std::size_t i = 0; i < features.size(); ++i) {
    const GeoRect bounds = boundsOf(features[i]);
    m_entries.push_back(
        Entry{overlay[i], std::move(features[i]), bounds, &placemark});
  }
  std::vector<KmlBillboard> points =
      kmlBillboards(*m_document, placemark, style);
  m_billboards.insert(m_billboards.end(), points.begin(), points.end());
  std::vector<KmlRaisedShape> raised = kmlRaisedShapes(placemark, style);
  m_raised.insert(m_raised.end(), raised.begin(), raised.end());
  if (!placemark.geometry) {
    return;
  }
  std::vector<const KmlGeometry*> tracks;
  collectTracks(*placemark.geometry, tracks);
  if (tracks.empty()) {
    return;
  }
  // The marker looks as a point of the placemark would.
  KmlFeature asPoint = placemark;
  KmlGeometry point;
  point.kind = KmlGeometry::Kind::Point;
  point.coordinates = {KmlCoordinate{}};
  asPoint.geometry = point;
  const std::vector<KmlBillboard> look =
      kmlBillboards(*m_document, asPoint, style);
  for (const KmlGeometry* track : tracks) {
    KmlBillboard marker = look.front();
    marker.placemark = &placemark;
    marker.altitudeMode = track->altitudeMode;
    m_tracks.emplace_back(track, marker);
  }
}

std::vector<KmlBillboard>
KmlFeatureProvider::trackMarkersAt(const QDateTime& clock) const
{
  std::vector<KmlBillboard> markers;
  for (const std::pair<const KmlGeometry*, KmlBillboard>& track : m_tracks) {
    const std::optional<KmlCoordinate> at =
        kmlTrackPosition(*track.first, clock);
    if (!at) {
      continue;
    }
    KmlBillboard marker = track.second;
    marker.lonDeg = at->lonDeg;
    marker.latDeg = at->latDeg;
    marker.altM = at->altM;
    markers.push_back(marker);
  }
  return markers;
}

QString KmlFeatureProvider::id() const
{
  return QStringLiteral("kml:%1|%2|%3|%4")
      .arg(m_document ? m_document->path : QString(), m_containerPath,
           m_at ? m_at->toString(Qt::ISODate) : QString(),
           m_deep ? QStringLiteral("deep") : QString());
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
    // Points and raised shapes are drawn over the view, not in tiles.
    if (!entry.overlay && overlaps(entry.bounds, rect)) {
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

namespace
{

/** Points are picked before lines, lines before areas. */
int32_t pickRank(Feature::Type type)
{
  switch (type) {
  case Feature::Type::Points:
    return 0;
  case Feature::Type::LineStrings:
    return 1;
  case Feature::Type::Polygon:
    break;
  }
  return 2;
}

} // namespace

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
    hits.emplace_back(pickRank(entry.feature.type), entry.placemark);
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](std::pair<int32_t, const KmlFeature*> a,
                      std::pair<int32_t, const KmlFeature*> b) {
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
