// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/wms/wmsimageryprovider.h"

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

int WmsImageryProvider::maximumLevel() const
{
  return m_source.maximumLevel;
}

int WmsImageryProvider::tileSizePx() const
{
  return m_source.tileSizePx;
}

int WmsImageryProvider::preferredLoaderThreads() const
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

RegionTileResult
WmsImageryProvider::fetchForRegion(TileKey key, long long region,
                                   const std::atomic<bool>& cancelled)
{
  using Outcome = RegionTileResult::Outcome;
  RegionTileResult result;
  const QUrl url = getMapUrl(m_source, key);
  if (url.isEmpty()) {
    result.outcome = Outcome::NoData;
    return result;
  }
  TileDiskCache* const cache = m_fetcher->diskCache();
  if (cache == nullptr) {
    result.errorString = QCoreApplication::translate(
        "ts::WmsImageryProvider", "There is no disk cache to keep tiles in.");
    return result;
  }
  const long long nowS = m_fetcher->now();
  const std::optional<CachedTile> cached = cache->find(key);
  if (cached && nowS < cached->expiresS &&
      (region == 0 || cache->pin(region, key))) {
    result.outcome = Outcome::AlreadyCached;
    result.bytes = cached->data.size();
    return result;
  }

  HttpRequest request;
  request.url = url;
  if (cached) {
    request.headers = revalidationHeaders(cached->etag, cached->lastModified);
  }
  m_fetcher->countRequest();
  const HttpResponse response = m_fetcher->network()->fetch(request, cancelled);
  if (response.error == NetworkError::Cancelled || cancelled.load()) {
    result.outcome = Outcome::Cancelled;
    return result;
  }
  if (response.error == NetworkError::TooLarge) {
    result.errorString = QCoreApplication::translate(
        "ts::WmsImageryProvider",
        "The server sent a tile that is far too large.");
    return result;
  }
  if (response.error != NetworkError::None) {
    result.unreachable = true;
    result.errorString = QCoreApplication::translate("ts::WmsImageryProvider",
                                                     "Can't reach %1 (%2).")
                             .arg(m_source.url.host(), response.errorString);
    return result;
  }
  if (response.status == 304 && cached) {
    const HttpFreshness renewed = revalidatedFreshness(
        HttpFreshness{true, cached->expiresS, cached->etag,
                      cached->lastModified},
        response, nowS, m_fetcher->settings().defaultLifetimeS);
    cache->refresh(key, renewed.expiresS, renewed.etag, renewed.lastModified);
    if (region == 0 || cache->pin(region, key)) {
      result.outcome = Outcome::Revalidated;
      result.bytes = cached->data.size();
    }
    return result;
  }
  if (response.status < 200 || response.status >= 300) {
    result.errorString = statusErrorMessage(response);
    return result;
  }
  if (decode(response.body).isNull()) {
    result.errorString = notAnImageMessage(response);
    return result;
  }
  const HttpFreshness freshness =
      httpFreshness(response, nowS, m_fetcher->settings().defaultLifetimeS);
  if (!freshness.storable) {
    result.errorString = QCoreApplication::translate(
        "ts::WmsImageryProvider",
        "The server doesn't allow its tiles to be kept (no-store).");
    return result;
  }
  if (!cache->store(key,
                    CachedTile{response.body, response.header("content-type"),
                               freshness.expiresS, freshness.etag,
                               freshness.lastModified},
                    region)) {
    result.errorString =
        QCoreApplication::translate("ts::WmsImageryProvider",
                                    "The tile could not be saved: %1")
            .arg(cache->errorString());
    return result;
  }
  result.outcome = Outcome::Downloaded;
  result.bytes = response.body.size();
  return result;
}

} // namespace ts
