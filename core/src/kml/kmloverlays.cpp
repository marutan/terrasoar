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
#include "ts/kml/kmloverlays.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QUrl>

#include "ts/kml/kmlfeatureprovider.h"
#include "ts/kml/kmlreader.h"
#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

constexpr int32_t tilePx = 256;
constexpr double degToRad = 3.14159265358979323846 / 180.0;

double cross(QPointF a, QPointF b)
{
  return a.x() * b.y() - a.y() * b.x();
}

/** @p lonDeg moved by whole turns to within 180° of @p nearDeg. */
double near(double lonDeg, double nearDeg)
{
  while (lonDeg - nearDeg > 180.0) {
    lonDeg -= 360.0;
  }
  while (lonDeg - nearDeg < -180.0) {
    lonDeg += 360.0;
  }
  return lonDeg;
}

/** The quad's corners, longitudes continuous from the first. */
std::array<QPointF, 4> quadCorners(const KmlLatLonBox& box)
{
  std::array<QPointF, 4> corners;
  for (std::size_t i = 0; i < 4; ++i) {
    const KmlCoordinate& c = box.quad.at(i);
    corners.at(i) = QPointF(
        i == 0 ? c.lonDeg : near(c.lonDeg, box.quad.front().lonDeg), c.latDeg);
  }
  return corners;
}

/** Inverse bilinear: (s, t) in the quad a b c d, t up from a–b. */
std::optional<QPointF> inQuad(const std::array<QPointF, 4>& q, QPointF p)
{
  constexpr double tiny = 1e-12;
  const QPointF e = q[1] - q[0];
  const QPointF f = q[3] - q[0];
  const QPointF g = q[0] - q[1] + q[2] - q[3];
  const QPointF h = p - q[0];
  const double k2 = cross(g, f);
  const double k1 = cross(e, f) + cross(h, g);
  const double k0 = cross(h, e);
  std::array<double, 2> candidates{};
  int32_t count = 0;
  if (std::abs(k2) < tiny) {
    if (std::abs(k1) < tiny) {
      return std::nullopt;
    }
    candidates.at(0) = -k0 / k1;
    count = 1;
  } else {
    const double w = k1 * k1 - 4.0 * k0 * k2;
    if (w < 0.0) {
      return std::nullopt;
    }
    const double root = std::sqrt(w);
    candidates = {(-k1 - root) / (2.0 * k2), (-k1 + root) / (2.0 * k2)};
    count = 2;
  }
  for (int32_t i = 0; i < count; ++i) {
    const double t = candidates.at(static_cast<std::size_t>(i));
    if (t < 0.0 || t > 1.0) {
      continue;
    }
    const QPointF across = e + g * t;
    const double s = std::abs(across.x()) > std::abs(across.y())
                         ? (h.x() - f.x() * t) / across.x()
                         : (h.y() - f.y() * t) / across.y();
    if (s >= 0.0 && s <= 1.0) {
      return QPointF(s, t);
    }
  }
  return std::nullopt;
}

double placed(double value, KmlVec2::Units units, double extent)
{
  switch (units) {
  case KmlVec2::Units::Fraction:
    return value * extent;
  case KmlVec2::Units::Pixels:
    return value;
  case KmlVec2::Units::InsetPixels:
    break;
  }
  return extent - value;
}

/** A pixel of @p image (ARGB32) at fractions @p uv, bilinear. */
QRgb sample(const QImage& image, QPointF uv)
{
  const double x = std::clamp(uv.x() * image.width() - 0.5, 0.0,
                              static_cast<double>(image.width() - 1));
  const double y = std::clamp(uv.y() * image.height() - 0.5, 0.0,
                              static_cast<double>(image.height() - 1));
  const int32_t x0 = static_cast<int32_t>(x);
  const int32_t y0 = static_cast<int32_t>(y);
  const int32_t x1 = std::min(x0 + 1, image.width() - 1);
  const int32_t y1 = std::min(y0 + 1, image.height() - 1);
  const double fx = x - x0;
  const double fy = y - y0;
  const QRgb* r0 = reinterpret_cast<const QRgb*>(image.constScanLine(y0));
  const QRgb* r1 = reinterpret_cast<const QRgb*>(image.constScanLine(y1));
  const std::array<QRgb, 4> p{r0[x0], r0[x1], r1[x0], r1[x1]};
  const std::array<double, 4> w{(1 - fx) * (1 - fy), fx * (1 - fy),
                                (1 - fx) * fy, fx * fy};
  std::array<double, 4> sum{};
  for (std::size_t i = 0; i < 4; ++i) {
    sum[0] += w.at(i) * qRed(p.at(i));
    sum[1] += w.at(i) * qGreen(p.at(i));
    sum[2] += w.at(i) * qBlue(p.at(i));
    sum[3] += w.at(i) * qAlpha(p.at(i));
  }
  return qRgba(static_cast<int32_t>(std::lround(sum[0])),
               static_cast<int32_t>(std::lround(sum[1])),
               static_cast<int32_t>(std::lround(sum[2])),
               static_cast<int32_t>(std::lround(sum[3])));
}

} // namespace

QJsonObject kmlOverlaySource(const QString& path, const QString& featurePath)
{
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("kmloverlay"));
  source.insert(QStringLiteral("path"), path);
  source.insert(QStringLiteral("feature"), featurePath);
  return source;
}

std::optional<QPointF> kmlOverlayImagePoint(const KmlLatLonBox& box,
                                            double lonDeg, double latDeg)
{
  if (box.quad.size() == 4) {
    const std::array<QPointF, 4> q = quadCorners(box);
    const double centre = (q[0].x() + q[1].x() + q[2].x() + q[3].x()) / 4.0;
    const std::optional<QPointF> st =
        inQuad(q, QPointF(near(lonDeg, centre), latDeg));
    if (!st) {
      return std::nullopt;
    }
    return QPointF(st->x(), 1.0 - st->y());
  }
  double east = box.east;
  if (east <= box.west) {
    east += 360.0;
  }
  const double width = east - box.west;
  const double height = box.north - box.south;
  if (width <= 0.0 || height <= 0.0) {
    return std::nullopt;
  }
  const double cx = (box.west + east) / 2.0;
  const double cy = (box.north + box.south) / 2.0;
  const double dx = near(lonDeg, cx) - cx;
  const double dy = latDeg - cy;
  // Turn the place back by the picture's (anticlockwise) rotation.
  const double a = box.rotationDeg * degToRad;
  const double x = dx * std::cos(a) + dy * std::sin(a);
  const double y = -dx * std::sin(a) + dy * std::cos(a);
  const double u = x / width + 0.5;
  const double v = 0.5 - y / height;
  if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
    return std::nullopt;
  }
  return QPointF(u, v);
}

GeoRect kmlOverlayBounds(const KmlLatLonBox& box)
{
  std::vector<QPointF> corners;
  if (box.quad.size() == 4) {
    const std::array<QPointF, 4> q = quadCorners(box);
    corners.assign(q.begin(), q.end());
  } else {
    double east = box.east;
    if (east <= box.west) {
      east += 360.0;
    }
    const double cx = (box.west + east) / 2.0;
    const double cy = (box.north + box.south) / 2.0;
    const double a = box.rotationDeg * degToRad;
    for (const QPointF& c :
         {QPointF(box.west, box.south), QPointF(east, box.south),
          QPointF(east, box.north), QPointF(box.west, box.north)}) {
      const double dx = c.x() - cx;
      const double dy = c.y() - cy;
      corners.emplace_back(cx + dx * std::cos(a) - dy * std::sin(a),
                           cy + dx * std::sin(a) + dy * std::cos(a));
    }
  }
  GeoRect rect;
  rect.westDeg = 1e9;
  rect.eastDeg = -1e9;
  rect.southDeg = 90.0;
  rect.northDeg = -90.0;
  for (const QPointF& c : corners) {
    rect.westDeg = std::min(rect.westDeg, c.x());
    rect.eastDeg = std::max(rect.eastDeg, c.x());
    rect.southDeg = std::min(rect.southDeg, c.y());
    rect.northDeg = std::max(rect.northDeg, c.y());
  }
  rect.southDeg = std::max(rect.southDeg, -90.0);
  rect.northDeg = std::min(rect.northDeg, 90.0);
  return rect;
}

KmlGroundOverlayProvider::KmlGroundOverlayProvider(
    std::shared_ptr<const KmlDocument> document, const QString& featurePath,
    std::shared_ptr<NetworkClient> network)
    : m_document(std::move(document)), m_featurePath(featurePath),
      m_network(std::move(network))
{
  const KmlFeature* feature =
      m_document ? kmlContainer(*m_document, featurePath) : nullptr;
  if (feature != nullptr && feature->kind == KmlFeature::Kind::GroundOverlay &&
      feature->latLonBox) {
    m_overlay = feature;
    m_bounds = kmlOverlayBounds(*feature->latLonBox);
  }
}

QString KmlGroundOverlayProvider::id() const
{
  return QStringLiteral("kmloverlay:%1|%2")
      .arg(m_document ? m_document->path : QString(), m_featurePath);
}

QString KmlGroundOverlayProvider::name() const
{
  return m_overlay != nullptr ? m_overlay->name : QString();
}

int32_t KmlGroundOverlayProvider::tileSizePx() const
{
  return tilePx;
}

int32_t KmlGroundOverlayProvider::maximumLevel() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (m_image.isNull()) {
    return TileKey::maximumLevel;
  }
  // Level 0's tiles are 180° across; each level halves them.
  const double degPerPixel =
      std::max(m_bounds.eastDeg - m_bounds.westDeg, 1e-9) /
      std::max(m_image.width(), 1);
  const double level = std::log2(180.0 / (degPerPixel * tilePx));
  return std::clamp(static_cast<int32_t>(std::ceil(level)) + 1, 0,
                    TileKey::maximumLevel);
}

bool KmlGroundOverlayProvider::picture(const std::atomic<bool>& cancelled)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_read) {
      return true;
    }
  }
  const QString where = resolveKmlHref(*m_document, m_overlay->iconHref);
  QImage image;
  const bool web = where.startsWith(QLatin1String("http://")) ||
                   where.startsWith(QLatin1String("https://"));
  if (web) {
    if (!m_network) {
      return false;
    }
    const HttpResponse response =
        m_network->fetch(HttpRequest{QUrl(where), {}}, cancelled);
    if (!response.isSuccess()) {
      return false; // asked again later
    }
    image = QImage::fromData(response.body);
  } else {
    image = QImage::fromData(readKmlLocalFile(where));
  }
  // Tinted once: KML multiplies the picture by the colour.
  image = image.convertToFormat(QImage::Format_ARGB32);
  const QColor tint = m_overlay->color;
  if (tint != QColor(255, 255, 255)) {
    for (int32_t y = 0; y < image.height(); ++y) {
      QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
      for (int32_t x = 0; x < image.width(); ++x) {
        const QRgb p = row[x];
        row[x] =
            qRgba(qRed(p) * tint.red() / 255, qGreen(p) * tint.green() / 255,
                  qBlue(p) * tint.blue() / 255, qAlpha(p) * tint.alpha() / 255);
      }
    }
  }
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_image = image;
  m_read = true;
  return true;
}

ImageLoadResult
KmlGroundOverlayProvider::loadTileImage(TileKey key,
                                        const std::atomic<bool>& cancelled)
{
  ImageLoadResult result;
  if (m_overlay == nullptr) {
    return result;
  }
  const GeoRect tile = key.bounds();
  constexpr std::array<double, 3> shifts{-360.0, 0.0, 360.0};
  const bool meets =
      tile.southDeg < m_bounds.northDeg && tile.northDeg > m_bounds.southDeg &&
      std::any_of(shifts.begin(), shifts.end(), [&](double shift) {
        return tile.westDeg + shift < m_bounds.eastDeg &&
               tile.eastDeg + shift > m_bounds.westDeg;
      });
  if (!meets) {
    return result;
  }
  if (!picture(cancelled)) {
    result.outcome = cancelled.load() ? ImageLoadResult::Outcome::Cancelled
                                      : ImageLoadResult::Outcome::Failed;
    return result;
  }
  QImage source;
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    source = m_image;
  }
  if (source.isNull()) {
    return result;
  }
  QImage image(tilePx, tilePx, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  bool any = false;
  const KmlLatLonBox& box = *m_overlay->latLonBox;
  for (int32_t j = 0; j < tilePx; ++j) {
    if (cancelled.load()) {
      result.outcome = ImageLoadResult::Outcome::Cancelled;
      return result;
    }
    const double lat =
        tile.northDeg - (j + 0.5) / tilePx * (tile.northDeg - tile.southDeg);
    QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(j));
    for (int32_t i = 0; i < tilePx; ++i) {
      const double lon =
          tile.westDeg + (i + 0.5) / tilePx * (tile.eastDeg - tile.westDeg);
      const std::optional<QPointF> uv = kmlOverlayImagePoint(box, lon, lat);
      if (uv) {
        row[i] = sample(source, *uv);
        any = true;
      }
    }
  }
  if (!any) {
    return result;
  }
  result.outcome = ImageLoadResult::Outcome::Image;
  result.image = image.convertToFormat(QImage::Format_RGBA8888);
  return result;
}

QImage KmlGroundOverlayProvider::loadImage(TileKey key,
                                           const std::atomic<bool>& cancelled)
{
  return loadTileImage(key, cancelled).image;
}

KmlScreenRect kmlScreenRect(const KmlScreenPlacement& placement,
                            QSizeF imageSize, QSizeF viewSize)
{
  // The size: −1 the picture's own; 0 keeping its aspect (both 0: own).
  const double aspect =
      imageSize.height() > 0.0 ? imageSize.width() / imageSize.height() : 1.0;
  const double sizeX = placement.size.x;
  const double sizeY = placement.size.y;
  double w = sizeX > 0.0
                 ? placed(sizeX, placement.size.xUnits, viewSize.width())
                 : imageSize.width();
  double h = sizeY > 0.0
                 ? placed(sizeY, placement.size.yUnits, viewSize.height())
                 : imageSize.height();
  if (sizeX == 0.0 && sizeY > 0.0) {
    w = h * aspect;
  } else if (sizeY == 0.0 && sizeX > 0.0) {
    h = aspect > 0.0 ? w / aspect : w;
  }
  // From the bottom left, y up.
  const double ox =
      placed(placement.overlayXY.x, placement.overlayXY.xUnits, w);
  const double oy =
      placed(placement.overlayXY.y, placement.overlayXY.yUnits, h);
  const double sx =
      placed(placement.screenXY.x, placement.screenXY.xUnits, viewSize.width());
  const double sy = placed(placement.screenXY.y, placement.screenXY.yUnits,
                           viewSize.height());
  const double rx = placed(placement.rotationXY.x, placement.rotationXY.xUnits,
                           viewSize.width());
  const double ry = placed(placement.rotationXY.y, placement.rotationXY.yUnits,
                           viewSize.height());
  KmlScreenRect placedRect;
  placedRect.rect = QRectF(sx - ox, viewSize.height() - (sy - oy) - h, w, h);
  placedRect.pivot = QPointF(rx, viewSize.height() - ry);
  placedRect.rotationDeg = -placement.rotationDeg;
  return placedRect;
}

} // namespace ts
