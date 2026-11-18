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
