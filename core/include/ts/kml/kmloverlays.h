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
 * KML's pictures: GroundOverlays as imagery layers, and where a
 * ScreenOverlay goes on the screen.
 */
#ifndef TS_KML_KMLOVERLAYS_H
#define TS_KML_KMLOVERLAYS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include "ts/kml/kmldocument.h"
#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

class NetworkClient;

/**
 * Returns the layer source of the GroundOverlay at @p featurePath (child
 * indices from the root, as kmlContainer()) in the KML file at @p path:
 * `{"kind": "kmloverlay", "path": …, "feature": …}`.
 */
QJsonObject kmlOverlaySource(const QString& path, const QString& featurePath);

/**
 * Returns where the place at @p lonDeg, @p latDeg falls in a GroundOverlay's
 * picture placed by @p box: x from the left edge and y from the top, as
 * fractions; nothing outside it. A rotated box turns the picture
 * anticlockwise about its centre; a gx:LatLonQuad (four corners
 * anticlockwise from the lower left) stretches it between them. Longitudes
 * may be on either side of the antimeridian.
 */
std::optional<QPointF> kmlOverlayImagePoint(const KmlLatLonBox& box,
                                            double lonDeg, double latDeg);

/** Returns the latitude/longitude rectangle @p box covers (rotated too). */
GeoRect kmlOverlayBounds(const KmlLatLonBox& box);

/**
 * A GroundOverlay's picture (layer source kmlOverlaySource()) warped into
 * each tile, tinted by its colour, draped on the ground. The picture is
 * read once: from the KMZ or next to the file, or fetched from the web.
 * @threadsafe
 */
class KmlGroundOverlayProvider : public ImageryProvider
{
public:
  /**
   * The GroundOverlay at @p featurePath in @p document; web pictures
   * through @p network (none: local only). See isValid().
   */
  KmlGroundOverlayProvider(std::shared_ptr<const KmlDocument> document,
                           const QString& featurePath,
                           std::shared_ptr<NetworkClient> network);

  /** Returns false if the feature isn't a GroundOverlay with a box. */
  bool isValid() const { return m_overlay != nullptr; }

  /** Returns the overlay (null if not valid). */
  const KmlFeature* overlay() const { return m_overlay; }

  /** Returns the rectangle it covers. */
  GeoRect bounds() const { return m_bounds; }

  /** See TileProvider::id(). */
  QString id() const override;
  /** See TileProvider::name(). */
  QString name() const override;
  /**
   * See TileProvider::maximumLevel(): where a tile's pixels are about the
   * picture's (until it is read: the deepest).
   */
  int32_t maximumLevel() const override;
  /** See ImageryProvider::tileSizePx(). */
  int32_t tileSizePx() const override;
  /** See ImageryProvider::loadTileImage(). */
  ImageLoadResult loadTileImage(TileKey key,
                                const std::atomic<bool>& cancelled) override;
  /** See ImageryProvider::loadImage(). */
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  bool picture(const std::atomic<bool>& cancelled);

  std::shared_ptr<const KmlDocument> m_document;
  QString m_featurePath;
  std::shared_ptr<NetworkClient> m_network;
  const KmlFeature* m_overlay = nullptr;
  GeoRect m_bounds;
  mutable std::mutex m_mutex;
  QImage m_image;
  bool m_read = false;
};

/** Where a ScreenOverlay goes on a view. */
struct KmlScreenRect {
  /** The picture's rectangle before turning, pixels from the top left. */
  QRectF rect;
  /** The point it turns about, pixels from the top left. */
  QPointF pivot;
  /** Degrees clockwise (Qt's sense). */
  double rotationDeg = 0.0;
};

/**
 * Returns where a ScreenOverlay with @p placement and a picture of
 * @p imageSize goes on a view of @p viewSize (KML measures from the
 * bottom left; insetPixels from the top right).
 */
KmlScreenRect kmlScreenRect(const KmlScreenPlacement& placement,
                            QSizeF imageSize, QSizeF viewSize);

} // namespace ts

#endif // TS_KML_KMLOVERLAYS_H
