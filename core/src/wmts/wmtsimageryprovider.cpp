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
#include "ts/wmts/wmtsimageryprovider.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QXmlStreamReader>

#include "ts/network/networkclient.h"
#include "ts/wms/wmssource.h"

namespace ts
{

namespace
{

/** The message of an OWS ExceptionReport, or empty if it isn't one. */
QString exceptionMessage(const QByteArray& body)
{
  QXmlStreamReader reader(body);
  if (!reader.readNextStartElement() ||
      (reader.name() != QLatin1String("ExceptionReport") &&
       reader.name() != QLatin1String("ServiceExceptionReport"))) {
    return QString();
  }
  QStringList messages;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement() &&
        (reader.name() == QLatin1String("ExceptionText") ||
         reader.name() == QLatin1String("ServiceException"))) {
      messages.append(
          reader.readElementText(QXmlStreamReader::IncludeChildElements)
              .simplified());
    }
  }
  return messages.join(QLatin1Char(' ')).simplified();
}

QString statusErrorMessage(const HttpResponse& response)
{
  const QString exception = exceptionMessage(response.body);
  if (exception.isEmpty()) {
    return QCoreApplication::translate("ts::WmtsImageryProvider",
                                       "The server answered %1.")
        .arg(response.errorString);
  }
  return QCoreApplication::translate("ts::WmtsImageryProvider",
                                     "The server reported an error: %1")
      .arg(exception);
}

QString notAnImageMessage(const HttpResponse& response)
{
  const QString exception = exceptionMessage(response.body);
  if (exception.isEmpty()) {
    return QCoreApplication::translate("ts::WmtsImageryProvider",
                                       "The server sent something that "
                                       "isn't an image (%1).")
        .arg(QString::fromLatin1(response.header("content-type")));
  }
  return QCoreApplication::translate("ts::WmtsImageryProvider",
                                     "The server reported an error: %1")
      .arg(exception);
}

QString hostOf(const WmtsSource& source)
{
  if (!source.urlTemplate.isEmpty()) {
    return QUrl(source.urlTemplate).host();
  }
  return source.url.host();
}

ImageLoadResult outcome(ImageLoadResult::Outcome kind)
{
  ImageLoadResult result;
  result.outcome = kind;
  return result;
}

} // namespace

WmtsImageryProvider::WmtsImageryProvider(WmtsSource source,
                                         std::shared_ptr<NetworkClient> network,
                                         WebTileSettings settings)
    : m_source(std::move(source)),
      m_fetcher(std::make_unique<CachedTileFetcher>(
          m_source.id(), hostOf(m_source), std::move(network),
          std::move(settings),
          CachedTileFetcher::Messages{statusErrorMessage, notAnImageMessage,
                                      true},
          [this](const ProviderStatus& status) { reportStatus(status); }))
{
}

WmtsImageryProvider::~WmtsImageryProvider() = default;

QString WmtsImageryProvider::id() const
{
  return m_source.id();
}

QString WmtsImageryProvider::name() const
{
  return m_source.title.isEmpty() ? m_source.layer : m_source.title;
}

int32_t WmtsImageryProvider::maximumLevel() const
{
  return m_source.maximumLevel;
}

int32_t WmtsImageryProvider::tileSizePx() const
{
  return m_source.tileSizePx;
}

int32_t WmtsImageryProvider::preferredLoaderThreads() const
{
  return m_fetcher->settings().loaderThreads;
}

TileImageProjection WmtsImageryProvider::imageProjection() const
{
  return m_source.imageProjection();
}

WebTileStatistics WmtsImageryProvider::statistics() const
{
  return m_fetcher->statistics();
}

QImage WmtsImageryProvider::loadImage(TileKey key,
                                      const std::atomic<bool>& cancelled)
{
  return loadTileImage(key, cancelled).image;
}

ImageLoadResult
WmtsImageryProvider::loadTileImage(TileKey key,
                                   const std::atomic<bool>& cancelled)
{
  const int32_t matrix = wmtsMatrixFor(m_source, key);
  const std::optional<WmtsTileWindow> window =
      wmtsTileWindow(m_source, key, matrix);
  if (!window) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  const WmtsMatrix& m = m_source.matrices.at(static_cast<std::size_t>(matrix));
  // The block of WMTS tiles, side by side; missing ones stay transparent.
  QImage mosaic(window->columns * m.tileWidth, window->rows * m.tileHeight,
                QImage::Format_ARGB32_Premultiplied);
  mosaic.fill(Qt::transparent);
  bool anyData = false;
  bool anyFailed = false;
  {
    QPainter painter(&mosaic);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (int32_t r = 0; r < window->rows; ++r) {
      for (int32_t c = 0; c < window->columns; ++c) {
        const int64_t column = window->firstColumn + c;
        const int64_t row = window->firstRow + r;
        const WmtsTileIndex tileIndex{matrix, column, row};
        if (!wmtsTileExists(m_source, tileIndex)) {
          continue;
        }
        QImage tile;
        // The disk cache keys WMTS tiles by (matrix, column, row).
        const TileKey cacheKey{matrix, static_cast<int32_t>(column),
                               static_cast<int32_t>(row)};
        const FetchOutcome fetched = m_fetcher->fetch(
            cacheKey, getTileUrl(m_source, tileIndex),
            [&tile](const QByteArray& data) {
              tile.loadFromData(data);
              return !tile.isNull();
            },
            cancelled);
        switch (fetched) {
        case FetchOutcome::Data:
          // A server sending the wrong size is scaled to fit its place.
          painter.drawImage(c * m.tileWidth, r * m.tileHeight,
                            tile.size() == QSize(m.tileWidth, m.tileHeight)
                                ? tile
                                : tile.scaled(m.tileWidth, m.tileHeight,
                                              Qt::IgnoreAspectRatio,
                                              Qt::SmoothTransformation));
          anyData = true;
          break;
        case FetchOutcome::NoData:
          break;
        case FetchOutcome::Cancelled:
          return outcome(ImageLoadResult::Outcome::Cancelled);
        case FetchOutcome::Failed:
          anyFailed = true;
          break;
        }
      }
    }
  }
  if (anyFailed) {
    return outcome(ImageLoadResult::Outcome::Failed);
  }
  if (!anyData) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  // The quadtree tile's part of the block, scaled to the tile's image.
  const int32_t size = m_source.tileSizePx;
  QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(
        QRectF(0.0, 0.0, size, size), mosaic,
        QRectF(window->left, window->top, window->width, window->height));
  }
  ImageLoadResult result;
  result.outcome = ImageLoadResult::Outcome::Image;
  result.image = std::move(image);
  return result;
}

QUrl WmtsImageryProvider::serviceUrl() const
{
  if (!m_source.service.isEmpty()) {
    return m_source.service;
  }
  if (!m_source.url.isEmpty()) {
    return m_source.url;
  }
  QUrl host(m_source.urlTemplate);
  host.setPath(QString());
  host.setQuery(QString());
  return host;
}

int64_t WmtsImageryProvider::typicalTileBytes() const
{
  const WmtsMatrix& finest = m_source.matrices.back();
  return typicalImageBytes(m_source.format,
                           std::max(finest.tileWidth, finest.tileHeight));
}

RegionTileResult
WmtsImageryProvider::fetchForRegion(TileKey key, int64_t region,
                                    const std::atomic<bool>& cancelled)
{
  using Outcome = RegionTileResult::Outcome;
  const int32_t matrix = wmtsMatrixFor(m_source, key);
  const std::optional<WmtsTileWindow> window =
      wmtsTileWindow(m_source, key, matrix);
  RegionTileResult total;
  total.outcome = Outcome::NoData;
  if (!window) {
    return total;
  }
  bool anyDownloaded = false;
  bool anyRevalidated = false;
  bool anyStored = false;
  for (int32_t r = 0; r < window->rows; ++r) {
    for (int32_t c = 0; c < window->columns; ++c) {
      const WmtsTileIndex tile{matrix, window->firstColumn + c,
                               window->firstRow + r};
      if (!wmtsTileExists(m_source, tile)) {
        continue;
      }
      const RegionTileResult result = m_fetcher->fetchForRegion(
          TileKey{matrix, static_cast<int32_t>(tile.column),
                  static_cast<int32_t>(tile.row)},
          getTileUrl(m_source, tile), region,
          [](const QByteArray& data) {
            QImage image;
            return image.loadFromData(data);
          },
          cancelled);
      switch (result.outcome) {
      case Outcome::Cancelled:
      case Outcome::Failed:
        return result;
      case Outcome::NoData:
        break;
      case Outcome::Downloaded:
        anyDownloaded = true;
        break;
      case Outcome::Revalidated:
        anyRevalidated = true;
        break;
      case Outcome::AlreadyCached:
        break;
      }
      if (result.outcome != Outcome::NoData) {
        anyStored = true;
        total.bytes += result.bytes;
      }
    }
  }
  if (anyDownloaded) {
    total.outcome = Outcome::Downloaded;
  } else if (anyRevalidated) {
    total.outcome = Outcome::Revalidated;
  } else if (anyStored) {
    total.outcome = Outcome::AlreadyCached;
  }
  return total;
}

std::optional<GeoRect> WmtsImageryProvider::dataBounds() const
{
  std::optional<GeoRect> bounds = m_source.bounds;
  if (m_source.imageProjection() == TileImageProjection::WebMercator) {
    bounds = webMercatorCoverage(
        bounds.value_or(GeoRect{-180.0, -90.0, 180.0, 90.0}));
    if (!bounds) {
      // Nothing at all: an empty rectangle no tile overlaps.
      return GeoRect{0.0, 0.0, 0.0, 0.0};
    }
  }
  return bounds;
}

} // namespace ts
