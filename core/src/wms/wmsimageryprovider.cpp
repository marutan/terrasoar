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

std::unique_ptr<TileDiskCache> makeCache(const WmsSource& source,
                                         const WmsProviderSettings& settings)
{
  if (settings.cacheDirectory.isEmpty()) {
    return nullptr;
  }
  return std::make_unique<TileDiskCache>(
      QDir(settings.cacheDirectory).filePath(wmsCacheFileName(source.id())),
      settings.cacheBytes);
}

} // namespace

QString wmsCacheFileName(const QString& sourceId)
{
  return QString::fromLatin1(QCryptographicHash::hash(sourceId.toUtf8(),
                                                      QCryptographicHash::Sha1)
                                 .toHex()) +
         QStringLiteral(".sqlite");
}

WmsImageryProvider::WmsImageryProvider(WmsSource source,
                                       std::shared_ptr<NetworkClient> network,
                                       const WmsProviderSettings& settings)
    : m_source(std::move(source)), m_network(std::move(network)),
      m_settings(settings), m_cache(makeCache(m_source, m_settings))
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
  return m_settings.loaderThreads;
}

TileImageProjection WmsImageryProvider::imageProjection() const
{
  return m_source.imageProjection();
}

WmsStatistics WmsImageryProvider::statistics() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_statistics;
}

long long WmsImageryProvider::now() const
{
  return m_settings.clock ? m_settings.clock()
                          : QDateTime::currentSecsSinceEpoch();
}

QImage WmsImageryProvider::loadImage(TileKey key,
                                     const std::atomic<bool>& cancelled)
{
  return loadTileImage(key, cancelled).image;
}

ImageLoadResult WmsImageryProvider::fromCache(TileKey key, bool stale)
{
  // The caller found it in the cache moments ago; read it again.
  const std::optional<CachedTile> cached =
      m_cache ? m_cache->find(key) : std::nullopt;
  if (!cached) {
    return outcome(ImageLoadResult::Outcome::Failed);
  }
  QImage image = decode(cached->data);
  if (image.isNull()) {
    m_cache->remove(key);
    return outcome(ImageLoadResult::Outcome::Failed);
  }
  if (stale) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.servedStale;
  }
  return imageResult(std::move(image));
}

void WmsImageryProvider::noteSuccess()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_unreachableInARow = 0;
  }
  reportStatus(ProviderStatus{});
}

void WmsImageryProvider::noteUnreachable(const QString& detail)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.failures;
    if (++m_unreachableInARow >= m_settings.failuresBeforeOffline) {
      m_leftAloneUntil =
          std::chrono::steady_clock::now() + m_settings.offlineRetry;
    }
  }
  reportStatus(ProviderStatus{
      ProviderStatus::State::Offline,
      QCoreApplication::translate("ts::WmsImageryProvider",
                                  "Can't reach %1 (%2). Saved tiles are "
                                  "shown where there are some.")
          .arg(m_source.url.host(), detail)});
}

void WmsImageryProvider::noteServerError(const QString& message)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.failures;
    m_unreachableInARow = 0;
  }
  reportStatus(ProviderStatus{ProviderStatus::State::Error, message});
}

ImageLoadResult
WmsImageryProvider::loadTileImage(TileKey key,
                                  const std::atomic<bool>& cancelled)
{
  const QUrl url = getMapUrl(m_source, key);
  if (url.isEmpty()) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  const long long nowS = now();
  const std::optional<CachedTile> cached =
      m_cache ? m_cache->find(key) : std::nullopt;
  if (cached && nowS < cached->expiresS) {
    QImage image = decode(cached->data);
    if (!image.isNull()) {
      const std::lock_guard<std::mutex> lock(m_mutex);
      ++m_statistics.cacheHits;
      return imageResult(std::move(image));
    }
    m_cache->remove(key);
  }
  bool leftAlone = false;
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    leftAlone = std::chrono::steady_clock::now() < m_leftAloneUntil;
    if (leftAlone) {
      ++m_statistics.skippedOffline;
    }
  }
  if (leftAlone) {
    return cached ? fromCache(key, true)
                  : outcome(ImageLoadResult::Outcome::Failed);
  }

  HttpRequest request;
  request.url = url;
  if (cached) {
    request.headers = revalidationHeaders(cached->etag, cached->lastModified);
  }
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.requests;
  }
  const HttpResponse response = m_network->fetch(request, cancelled);
  if (response.error == NetworkError::Cancelled || cancelled.load()) {
    return outcome(ImageLoadResult::Outcome::Cancelled);
  }
  if (response.error != NetworkError::None) {
    if (response.error == NetworkError::TooLarge) {
      noteServerError(QCoreApplication::translate(
          "ts::WmsImageryProvider",
          "The server sent a tile that is far too large."));
      return outcome(ImageLoadResult::Outcome::Failed);
    }
    noteUnreachable(response.errorString);
    return cached ? fromCache(key, true)
                  : outcome(ImageLoadResult::Outcome::Failed);
  }
  if (response.status == 304 && cached) {
    const HttpFreshness previous{true, cached->expiresS, cached->etag,
                                 cached->lastModified};
    const HttpFreshness renewed = revalidatedFreshness(
        previous, response, nowS, m_settings.defaultLifetimeS);
    m_cache->refresh(key, renewed.expiresS, renewed.etag, renewed.lastModified);
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      ++m_statistics.revalidated;
    }
    noteSuccess();
    return fromCache(key, false);
  }
  if (response.status < 200 || response.status >= 300) {
    const QString exception = serviceException(response.body);
    noteServerError(
        exception.isEmpty()
            ? QCoreApplication::translate("ts::WmsImageryProvider",
                                          "The server answered %1.")
                  .arg(response.errorString)
            : QCoreApplication::translate("ts::WmsImageryProvider",
                                          "The server reported an error: %1")
                  .arg(exception));
    return cached ? fromCache(key, true)
                  : outcome(ImageLoadResult::Outcome::Failed);
  }
  QImage image = decode(response.body);
  if (image.isNull()) {
    // WMS servers report errors with status 200 and an XML document.
    const QString exception = serviceException(response.body);
    noteServerError(
        exception.isEmpty()
            ? QCoreApplication::translate("ts::WmsImageryProvider",
                                          "The server sent something that "
                                          "isn't an image (%1).")
                  .arg(QString::fromLatin1(response.header("content-type")))
            : QCoreApplication::translate("ts::WmsImageryProvider",
                                          "The server reported an error: %1")
                  .arg(exception));
    return cached ? fromCache(key, true)
                  : outcome(ImageLoadResult::Outcome::Failed);
  }
  if (m_cache) {
    const HttpFreshness freshness =
        httpFreshness(response, nowS, m_settings.defaultLifetimeS);
    if (freshness.storable) {
      m_cache->store(key,
                     CachedTile{response.body, response.header("content-type"),
                                freshness.expiresS, freshness.etag,
                                freshness.lastModified});
    }
  }
  noteSuccess();
  return imageResult(std::move(image));
}

} // namespace ts
