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

#include "ts/wms/wmsimageryprovider.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QXmlStreamReader>

#include "ts/network/httpcache.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"

namespace ts
{

namespace
{

/** The message of a ServiceExceptionReport, or empty if it isn't one. */
QString serviceException(const QByteArray& body)
{
  QXmlStreamReader reader(body);
  if (!reader.readNextStartElement() ||
      (reader.name() != QLatin1String("ServiceExceptionReport") &&
       reader.name() != QLatin1String("ExceptionReport"))) {
    return QString();
  }
  QStringList messages;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement() &&
        (reader.name() == QLatin1String("ServiceException") ||
         reader.name() == QLatin1String("ExceptionText"))) {
      messages.append(
          reader.readElementText(QXmlStreamReader::IncludeChildElements)
              .simplified());
    }
  }
  return messages.join(QLatin1Char(' ')).simplified();
}

QImage decode(const QByteArray& data)
{
  QImage image;
  image.loadFromData(data);
  return image;
}

ImageLoadResult imageResult(QImage image)
{
  ImageLoadResult result;
  result.outcome = ImageLoadResult::Outcome::Image;
  result.image = std::move(image);
  return result;
}

ImageLoadResult outcome(ImageLoadResult::Outcome kind)
{
  ImageLoadResult result;
  result.outcome = kind;
  return result;
}

/** What to say about a response with an error status. */
QString statusErrorMessage(const HttpResponse& response)
{
  const QString exception = serviceException(response.body);
  if (exception.isEmpty()) {
    return QCoreApplication::translate("ts::WmsImageryProvider",
                                       "The server answered %1.")
        .arg(response.errorString);
  }
  return QCoreApplication::translate("ts::WmsImageryProvider",
                                     "The server reported an error: %1")
      .arg(exception);
}

/** What to say about a response that isn't an image. WMS servers report
 * errors with status 200 and an XML document. */
QString notAnImageMessage(const HttpResponse& response)
{
  const QString exception = serviceException(response.body);
  if (exception.isEmpty()) {
    return QCoreApplication::translate("ts::WmsImageryProvider",
                                       "The server sent something that "
                                       "isn't an image (%1).")
        .arg(QString::fromLatin1(response.header("content-type")));
  }
  return QCoreApplication::translate("ts::WmsImageryProvider",
                                     "The server reported an error: %1")
      .arg(exception);
}

} // namespace

QString wmsCacheFileName(const QString& sourceId)
{
  return webCacheFileName(sourceId);
}

WmsImageryProvider::WmsImageryProvider(WmsSource source,
                                       std::shared_ptr<NetworkClient> network,
                                       WmsProviderSettings settings)
    : m_source(std::move(source)),
      m_fetcher(std::make_unique<CachedTileFetcher>(
          m_source.id(), m_source.url.host(), std::move(network),
          std::move(settings),
          CachedTileFetcher::Messages{statusErrorMessage, notAnImageMessage},
          [this](const ProviderStatus& status) { reportStatus(status); }))
{
}

WmsImageryProvider::~WmsImageryProvider() = default;

QString WmsImageryProvider::id() const
{
  return m_source.id();
}

QString WmsImageryProvider::name() const
{
  return m_source.title.isEmpty() ? m_source.layers.join(QStringLiteral(", "))
                                  : m_source.title;
}

int32_t WmsImageryProvider::maximumLevel() const
{
  return m_source.maximumLevel;
}

int32_t WmsImageryProvider::tileSizePx() const
{
  return m_source.tileSizePx;
}

int32_t WmsImageryProvider::preferredLoaderThreads() const
{
  return m_fetcher->settings().loaderThreads;
}

TileImageProjection WmsImageryProvider::imageProjection() const
{
  return m_source.imageProjection();
}

WmsStatistics WmsImageryProvider::statistics() const
{
  return m_fetcher->statistics();
}

QImage WmsImageryProvider::loadImage(TileKey key,
                                     const std::atomic<bool>& cancelled)
{
  return loadTileImage(key, cancelled).image;
}

ImageLoadResult
WmsImageryProvider::loadTileImage(TileKey key,
                                  const std::atomic<bool>& cancelled)
{
  const QUrl url = getMapUrl(m_source, key);
  if (url.isEmpty()) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  if (std::optional<ImageLoadResult> fromBlock =
          loadFromBlock(key, cancelled)) {
    return std::move(*fromBlock);
  }
  QImage image;
  const FetchOutcome fetched = m_fetcher->fetch(
      key, url,
      [&image](const QByteArray& data) {
        image = decode(data);
        return !image.isNull();
      },
      cancelled);
  switch (fetched) {
  case FetchOutcome::Data:
    return imageResult(std::move(image));
  case FetchOutcome::NoData:
    return outcome(ImageLoadResult::Outcome::NoData);
  case FetchOutcome::Cancelled:
    return outcome(ImageLoadResult::Outcome::Cancelled);
  case FetchOutcome::Failed:
    break;
  }
  return outcome(ImageLoadResult::Outcome::Failed);
}

bool WmsImageryProvider::usesBlocks(TileKey key) const
{
  return m_fetcher->settings().blocks && !m_blocksRefused.load() &&
         key.level >= 1 &&
         m_source.imageProjection() == TileImageProjection::Geographic &&
         2 * m_source.tileSizePx <= 4096;
}

WmsImageryProvider::BlockWait WmsImageryProvider::waitForBlock(
    const QString& blockId, TileKey key,
    const std::function<bool(const QByteArray&)>& accept,
    const std::atomic<bool>& cancelled)
{
  // Another thread fetching this block: wait for it, then look again.
  std::unique_lock<std::mutex> lock(m_blocksMutex);
  while (m_blocksInFlight.count(blockId) != 0) {
    m_blockDone.wait_for(lock, std::chrono::milliseconds(20));
    if (cancelled.load()) {
      return BlockWait::Cancelled;
    }
    if (m_blocksInFlight.count(blockId) == 0) {
      lock.unlock();
      if (m_fetcher->fromFreshCache(key, accept)) {
        return BlockWait::Cached;
      }
      lock.lock(); // it failed: try it here
    }
  }
  m_blocksInFlight.insert(blockId);
  return BlockWait::Fetch;
}

void WmsImageryProvider::blockDone(const QString& blockId)
{
  {
    const std::lock_guard<std::mutex> lock(m_blocksMutex);
    m_blocksInFlight.erase(blockId);
  }
  m_blockDone.notify_all();
}

std::vector<BlockPiece> WmsImageryProvider::cutBlock(const QByteArray& data,
                                                     TileKey key,
                                                     QImage& mine) const
{
  // The block is the tile's parent; the image covers its four children.
  const TileKey block = key.parent();
  const int32_t sizePx = m_source.tileSizePx;
  std::vector<BlockPiece> pieces;
  const QImage blockImage = decode(data);
  if (blockImage.width() != 2 * sizePx || blockImage.height() != 2 * sizePx) {
    return pieces;
  }
  const bool jpeg = m_source.format.contains(QLatin1String("jpeg")) ||
                    m_source.format.contains(QLatin1String("jpg"));
  const GeoRect whole = block.bounds();
  for (int32_t i = 0; i < 4; ++i) {
    const TileKey child = block.child(i);
    const GeoRect rect = child.bounds();
    // North up, west on the left.
    const int32_t column = rect.westDeg > whole.centreLonDeg() - 1e-9 ? 1 : 0;
    const int32_t row = rect.northDeg < whole.northDeg - 1e-9 ? 1 : 0;
    const QImage part =
        blockImage.copy(column * sizePx, row * sizePx, sizePx, sizePx);
    if (child == key) {
      mine = part;
    }
    BlockPiece piece;
    piece.key = child;
    QBuffer buffer(&piece.data);
    buffer.open(QIODevice::WriteOnly);
    part.save(&buffer, jpeg ? "JPG" : "PNG", jpeg ? 92 : -1);
    piece.contentType =
        jpeg ? QByteArrayLiteral("image/jpeg") : QByteArrayLiteral("image/png");
    pieces.push_back(std::move(piece));
  }
  return pieces;
}

std::optional<ImageLoadResult>
WmsImageryProvider::loadFromBlock(TileKey key,
                                  const std::atomic<bool>& cancelled)
{
  if (!usesBlocks(key)) {
    return std::nullopt;
  }
  QImage image;
  const std::function<bool(const QByteArray&)> accept =
      [&image](const QByteArray& data) {
        image = decode(data);
        return !image.isNull();
      };
  // Fresh in the cache: from an earlier block, or on its own.
  if (m_fetcher->fromFreshCache(key, accept)) {
    return imageResult(std::move(image));
  }
  const TileKey block = key.parent();
  WmsSource doubled = m_source;
  doubled.tileSizePx = 2 * m_source.tileSizePx;
  const QUrl url = getMapUrl(doubled, block);
  if (url.isEmpty()) {
    return std::nullopt;
  }
  const QString blockId = url.toString();
  switch (waitForBlock(blockId, key, accept, cancelled)) {
  case BlockWait::Cancelled:
    return outcome(ImageLoadResult::Outcome::Cancelled);
  case BlockWait::Cached:
    return imageResult(std::move(image));
  case BlockWait::Fetch:
    break;
  }
  QImage mine;
  const BlockFetch fetched = m_fetcher->fetchBlock(
      url, [&](const QByteArray& data) { return cutBlock(data, key, mine); },
      cancelled);
  blockDone(blockId);
  if (fetched.outcome == FetchOutcome::Data && !mine.isNull()) {
    return imageResult(std::move(mine));
  }
  if (fetched.outcome == FetchOutcome::Cancelled) {
    return outcome(ImageLoadResult::Outcome::Cancelled);
  }
  if (fetched.refused) {
    m_blocksRefused.store(true);
  }
  // Single tiles (which also show a stale saved tile when offline).
  return std::nullopt;
}

std::optional<GeoRect> WmsImageryProvider::dataBounds() const
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

int64_t WmsImageryProvider::typicalTileBytes() const
{
  return typicalImageBytes(m_source.format, m_source.tileSizePx);
}

RegionTileResult
WmsImageryProvider::fetchForRegion(TileKey key, int64_t region,
                                   const std::atomic<bool>& cancelled)
{
  const QUrl url = getMapUrl(m_source, key);
  if (url.isEmpty()) {
    RegionTileResult result;
    result.outcome = RegionTileResult::Outcome::NoData;
    return result;
  }
  return m_fetcher->fetchForRegion(
      key, url, region,
      [](const QByteArray& data) { return !decode(data).isNull(); }, cancelled);
}

} // namespace ts
