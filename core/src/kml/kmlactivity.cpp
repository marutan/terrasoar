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
#include "ts/kml/kmlactivity.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QLocale>
#include <QRectF>
#include <QStringList>
#include <QUrlQuery>

#include "ts/geo/geodetic.h"
#include "ts/kml/kmlreader.h"
#include "ts/maths/ray.h"

namespace ts
{

namespace
{

constexpr double radToDeg = 180.0 / 3.14159265358979323846;

/** The features from the root to the one at @p featurePath; empty if none. */
std::vector<const KmlFeature*> chainTo(const KmlDocument& document,
                                       const QString& featurePath)
{
  std::vector<const KmlFeature*> chain{&document.root};
  if (featurePath.isEmpty()) {
    return chain;
  }
  for (const QString& part : featurePath.split(QLatin1Char('.'))) {
    bool ok = false;
    const int32_t index = part.toInt(&ok);
    const KmlFeature* last = chain.back();
    if (!ok || index < 0 ||
        static_cast<std::size_t>(index) >= last->children.size()) {
      return {};
    }
    chain.push_back(&last->children.at(static_cast<std::size_t>(index)));
  }
  return chain;
}

void addBoundaries(const std::optional<KmlTime>& time,
                   std::vector<QDateTime>& boundaries)
{
  if (!time) {
    return;
  }
  if (time->begin) {
    boundaries.push_back(*time->begin);
  }
  if (time->end && time->end != time->begin) {
    boundaries.push_back(*time->end);
  }
}

void collectBoundaries(const KmlFeature& container, bool deep,
                       std::vector<QDateTime>& boundaries)
{
  for (const KmlFeature& child : container.children) {
    const bool isContainer = child.kind == KmlFeature::Kind::Document ||
                             child.kind == KmlFeature::Kind::Folder;
    if (isContainer && !deep) {
      continue;
    }
    addBoundaries(child.time, boundaries);
    if (isContainer) {
      collectBoundaries(child, deep, boundaries);
    }
  }
}

void collectTracks(const KmlGeometry& geometry,
                   std::vector<const KmlGeometry*>& tracks)
{
  if (geometry.kind == KmlGeometry::Kind::Track && geometry.when.size() >= 2) {
    tracks.push_back(&geometry);
  }
  for (const KmlGeometry& child : geometry.children) {
    collectTracks(child, tracks);
  }
}

void collectTracks(const KmlFeature& container, bool deep,
                   std::vector<const KmlGeometry*>& tracks)
{
  if (container.geometry) {
    collectTracks(*container.geometry, tracks);
  }
  for (const KmlFeature& child : container.children) {
    const bool isContainer = child.kind == KmlFeature::Kind::Document ||
                             child.kind == KmlFeature::Kind::Folder;
    if (child.geometry) {
      collectTracks(*child.geometry, tracks);
    }
    if (isContainer && deep) {
      collectTracks(child, deep, tracks);
    }
  }
}

QString isoTime(const QDateTime& time)
{
  return time.toUTC().toString(Qt::ISODate);
}

QString number(double value)
{
  return QString::number(value, 'g', 10);
}

} // namespace

bool kmlShownAt(const KmlTime& time, const QDateTime& clock)
{
  if (time.begin && clock < *time.begin) {
    return false;
  }
  if (time.begin && time.end && *time.begin == *time.end) {
    return true; // a TimeStamp: from then on
  }
  return !time.end || clock < *time.end;
}

std::optional<KmlTime> kmlNarrowTime(const std::optional<KmlTime>& outer,
                                     const std::optional<KmlTime>& inner)
{
  if (!outer) {
    return inner;
  }
  if (!inner) {
    return outer;
  }
  KmlTime narrowed = *inner;
  if (outer->begin && (!narrowed.begin || *outer->begin > *narrowed.begin)) {
    narrowed.begin = outer->begin;
  }
  // A container's TimeStamp has no end for what it holds.
  const bool outerStamp =
      outer->begin && outer->end && *outer->begin == *outer->end;
  if (outer->end && !outerStamp &&
      (!narrowed.end || *outer->end < *narrowed.end)) {
    narrowed.end = outer->end;
  }
  return narrowed;
}

std::optional<KmlTime> kmlEffectiveTime(const KmlDocument& document,
                                        const QString& featurePath)
{
  std::optional<KmlTime> time;
  for (const KmlFeature* feature : chainTo(document, featurePath)) {
    time = kmlNarrowTime(time, feature->time);
  }
  return time;
}

std::vector<QDateTime> kmlTimeBoundaries(const KmlDocument& document,
                                         const QString& containerPath,
                                         bool deep)
{
  std::vector<QDateTime> boundaries;
  const std::vector<const KmlFeature*> chain = chainTo(document, containerPath);
  if (chain.empty()) {
    return boundaries;
  }
  for (const KmlFeature* feature : chain) {
    addBoundaries(feature->time, boundaries);
  }
  collectBoundaries(*chain.back(), deep, boundaries);
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());
  return boundaries;
}

QDateTime kmlTimeKey(const std::vector<QDateTime>& boundaries,
                     const QDateTime& clock)
{
  if (boundaries.empty()) {
    return QDateTime();
  }
  const std::vector<QDateTime>::const_iterator after =
      std::upper_bound(boundaries.begin(), boundaries.end(), clock);
  if (after == boundaries.begin()) {
    return boundaries.front().addSecs(-1);
  }
  return *(after - 1);
}

QString kmlTimeExtent(const KmlDocument& document, const QString& containerPath,
                      bool deep)
{
  QStringList parts;
  for (const QDateTime& boundary :
       kmlTimeBoundaries(document, containerPath, deep)) {
    parts << isoTime(boundary);
  }
  const std::vector<const KmlFeature*> chain = chainTo(document, containerPath);
  if (!chain.empty()) {
    std::vector<const KmlGeometry*> tracks;
    collectTracks(*chain.back(), deep, tracks);
    for (const KmlGeometry* track : tracks) {
      const QDateTime first =
          *std::min_element(track->when.begin(), track->when.end());
      const QDateTime last =
          *std::max_element(track->when.begin(), track->when.end());
      const int64_t step = std::max<int64_t>(1, first.secsTo(last) / 100);
      parts << QStringLiteral("%1/%2/PT%3S")
                   .arg(isoTime(first), isoTime(last))
                   .arg(step);
    }
  }
  return parts.join(QLatin1Char(','));
}

std::optional<KmlCoordinate> kmlTrackPosition(const KmlGeometry& track,
                                              const QDateTime& clock)
{
  if (track.kind == KmlGeometry::Kind::MultiGeometry) {
    for (const KmlGeometry& child : track.children) {
      const std::optional<KmlCoordinate> found = kmlTrackPosition(child, clock);
      if (found) {
        return found;
      }
    }
    return std::nullopt;
  }
  if (track.kind != KmlGeometry::Kind::Track || track.when.empty() ||
      track.when.size() != track.coordinates.size()) {
    return std::nullopt;
  }
  if (clock < track.when.front() || clock > track.when.back()) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i + 1 < track.when.size(); ++i) {
    const QDateTime& from = track.when.at(i);
    const QDateTime& to = track.when.at(i + 1);
    if (clock < from || clock > to) {
      continue;
    }
    const int64_t span = from.msecsTo(to);
    const double t = span > 0 ? static_cast<double>(from.msecsTo(clock)) /
                                    static_cast<double>(span)
                              : 0.0;
    const KmlCoordinate& a = track.coordinates.at(i);
    const KmlCoordinate& b = track.coordinates.at(i + 1);
    double toLon = b.lonDeg;
    while (toLon - a.lonDeg > 180.0) {
      toLon -= 360.0;
    }
    while (toLon - a.lonDeg < -180.0) {
      toLon += 360.0;
    }
    double lon = a.lonDeg + t * (toLon - a.lonDeg);
    if (lon > 180.0) {
      lon -= 360.0;
    } else if (lon < -180.0) {
      lon += 360.0;
    }
    return KmlCoordinate{lon, a.latDeg + t * (b.latDeg - a.latDeg),
                         a.altM + t * (b.altM - a.altM)};
  }
  return track.coordinates.back();
}

double kmlRegionPixels(const KmlRegion& region, const Camera& camera)
{
  constexpr int32_t samples = 5;
  const bool onMap = camera.morph() > 0.0;
  double east = region.east;
  if (east < region.west) {
    east += 360.0;
  }
  QRectF covered;
  bool any = false;
  for (int32_t j = 0; j < samples; ++j) {
    const double lat =
        region.south + (region.north - region.south) * j / (samples - 1);
    for (int32_t i = 0; i < samples; ++i) {
      double lon = region.west + (east - region.west) * i / (samples - 1);
      if (lon > 180.0) {
        lon -= 360.0;
      }
      std::optional<ScreenPoint> screen;
      if (onMap) {
        screen = camera.projectGeodetic(lat, lon);
      } else {
        const Vec3 point = geodeticToEcef(Geodetic{lat, lon, region.minAltM});
        if (dot(surfaceNormalEcef(lat, lon), camera.eyeEcef() - point) <= 0.0) {
          continue; // the far side of the Earth
        }
        screen = camera.project(point);
      }
      if (!screen) {
        continue;
      }
      const QRectF here(screen->xPx, screen->yPx, 0.0, 0.0);
      covered = any ? covered.united(here.adjusted(0, 0, 1e-9, 1e-9))
                    : here.adjusted(0, 0, 1e-9, 1e-9);
      any = true;
    }
  }
  const QRectF view(0.0, 0.0, camera.viewportWidthPx(),
                    camera.viewportHeightPx());
  if (!any || !covered.intersects(view)) {
    return -1.0;
  }
  return std::sqrt(covered.width() * covered.height());
}

bool kmlRegionActive(const KmlRegion& region, const Camera& camera)
{
  const double pixels = kmlRegionPixels(region, camera);
  if (pixels < 0.0) {
    return false;
  }
  return pixels >= region.minLodPixels &&
         (region.maxLodPixels < 0.0 || pixels <= region.maxLodPixels);
}

bool kmlRegionsActive(const KmlDocument& document, const QString& featurePath,
                      const Camera& camera)
{
  const std::vector<const KmlFeature*> chain = chainTo(document, featurePath);
  return std::all_of(
      chain.begin(), chain.end(), [&camera](const KmlFeature* feature) {
        return !feature->region || kmlRegionActive(*feature->region, camera);
      });
}

bool kmlHasRegion(const KmlDocument& document, const QString& featurePath)
{
  const std::vector<const KmlFeature*> chain = chainTo(document, featurePath);
  return std::any_of(chain.begin(), chain.end(),
                     [](const KmlFeature* f) { return f->region.has_value(); });
}

KmlViewParameters kmlViewParameters(const Camera& camera, bool terrain)
{
  KmlViewParameters view;
  view.pose = camera.pose();
  view.eye = camera.eyeGeodetic();
  view.widthPx = static_cast<int32_t>(std::lround(camera.viewportWidthPx()));
  view.heightPx = static_cast<int32_t>(std::lround(camera.viewportHeightPx()));
  view.horizontalFovDeg = 2.0 * std::atan(camera.tanHalfFovX()) * radToDeg;
  view.verticalFovDeg = 2.0 * std::atan(camera.tanHalfFovY()) * radToDeg;
  view.terrain = terrain;
  // Where the view's edges meet the ground; all of the Earth if any of
  // them looks past it.
  const double w = camera.viewportWidthPx();
  const double h = camera.viewportHeightPx();
  const std::array<QPointF, 8> edges{
      QPointF(0, 0), QPointF(w / 2, 0), QPointF(w, 0), QPointF(w, h / 2),
      QPointF(w, h), QPointF(w / 2, h), QPointF(0, h), QPointF(0, h / 2)};
  const double centreLon = view.pose.targetLonDeg;
  GeoRect bounds{1e9, 90.0, -1e9, -90.0};
  for (const QPointF& edge : edges) {
    const std::optional<Geodetic> ground =
        camera.pickSurface(camera.rayThroughPixel(edge.x(), edge.y()));
    if (!ground) {
      return view;
    }
    double lon = ground->lonDeg;
    while (lon - centreLon > 180.0) {
      lon -= 360.0;
    }
    while (lon - centreLon < -180.0) {
      lon += 360.0;
    }
    bounds.westDeg = std::min(bounds.westDeg, lon);
    bounds.eastDeg = std::max(bounds.eastDeg, lon);
    bounds.southDeg = std::min(bounds.southDeg, ground->latDeg);
    bounds.northDeg = std::max(bounds.northDeg, ground->latDeg);
  }
  if (bounds.westDeg < -180.0 || bounds.eastDeg > 180.0) {
    bounds.westDeg = -180.0;
    bounds.eastDeg = 180.0;
  }
  view.bounds = bounds;
  return view;
}

QUrl kmlLinkUrl(const KmlLink& link, const KmlDocument& document,
                const KmlViewParameters& view)
{
  const QString where = resolveKmlHref(document, link.href);
  if (!where.startsWith(QLatin1String("http://")) &&
      !where.startsWith(QLatin1String("https://"))) {
    return QUrl::fromLocalFile(where);
  }
  QUrl url(where);
  // The view's bounds, scaled about their centre.
  const GeoRect& b = view.bounds;
  const double cx = (b.westDeg + b.eastDeg) / 2.0;
  const double cy = (b.southDeg + b.northDeg) / 2.0;
  const double hw = (b.eastDeg - b.westDeg) / 2.0 * link.viewBoundScale;
  const double hh = (b.northDeg - b.southDeg) / 2.0 * link.viewBoundScale;
  const std::vector<std::pair<QString, QString>> values{
      {QStringLiteral("[bboxWest]"), number(std::max(cx - hw, -180.0))},
      {QStringLiteral("[bboxSouth]"), number(std::max(cy - hh, -90.0))},
      {QStringLiteral("[bboxEast]"), number(std::min(cx + hw, 180.0))},
      {QStringLiteral("[bboxNorth]"), number(std::min(cy + hh, 90.0))},
      {QStringLiteral("[lookatLon]"), number(view.pose.targetLonDeg)},
      {QStringLiteral("[lookatLat]"), number(view.pose.targetLatDeg)},
      {QStringLiteral("[lookatRange]"), number(view.pose.rangeM)},
      {QStringLiteral("[lookatTilt]"), number(view.pose.tiltDeg)},
      {QStringLiteral("[lookatHeading]"), number(view.pose.headingDeg)},
      {QStringLiteral("[lookatTerrainLon]"), number(view.pose.targetLonDeg)},
      {QStringLiteral("[lookatTerrainLat]"), number(view.pose.targetLatDeg)},
      {QStringLiteral("[lookatTerrainAlt]"), number(0.0)},
      {QStringLiteral("[cameraLon]"), number(view.eye.lonDeg)},
      {QStringLiteral("[cameraLat]"), number(view.eye.latDeg)},
      {QStringLiteral("[cameraAlt]"), number(view.eye.heightM)},
      {QStringLiteral("[horizPixels]"), QString::number(view.widthPx)},
      {QStringLiteral("[vertPixels]"), QString::number(view.heightPx)},
      {QStringLiteral("[horizFov]"), number(view.horizontalFovDeg)},
      {QStringLiteral("[vertFov]"), number(view.verticalFovDeg)},
      {QStringLiteral("[terrainEnabled]"),
       view.terrain ? QStringLiteral("1") : QStringLiteral("0")},
      {QStringLiteral("[clientVersion]"), QStringLiteral(TS_VERSION_STRING)},
      {QStringLiteral("[kmlVersion]"), QStringLiteral("2.2")},
      {QStringLiteral("[clientName]"), QStringLiteral("TerraSoar")},
      {QStringLiteral("[language]"), QLocale().bcp47Name()},
  };
  QString format = link.viewFormat;
  if (format.isEmpty() &&
      link.viewRefreshMode != KmlLink::ViewRefreshMode::Never) {
    format =
        QStringLiteral("BBOX=[bboxWest],[bboxSouth],[bboxEast],[bboxNorth]");
  }
  QStringList extra;
  for (QString part : {format, link.httpQuery}) {
    if (part.isEmpty()) {
      continue;
    }
    for (const std::pair<QString, QString>& value : values) {
      part.replace(value.first, value.second);
    }
    extra << part;
  }
  if (!extra.isEmpty()) {
    const QString query = url.query(QUrl::FullyEncoded);
    QStringList all;
    if (!query.isEmpty()) {
      all << query;
    }
    all << extra;
    url.setQuery(all.join(QLatin1Char('&')), QUrl::TolerantMode);
  }
  return url;
}

} // namespace ts
