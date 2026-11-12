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
  return QString::fromLatin1(QCryptographicHash::hash(sourceId.toUtf8(),
                                                      QCryptographicHash::Sha1)
                                 .toHex()) +
         QStringLiteral(".sqlite");
}

WmsImageryProvider::WmsImageryProvider(WmsSource source,
                                       std::shared_ptr<NetworkClient> network,
                                       WmsProviderSettings settings)
    : m_source(std::move(source)), m_network(std::move(network)),
      m_settings(std::move(settings)), m_cache(makeCache(m_source, m_settings))
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

ImageLoadResult WmsImageryProvider::staleOrFailed(TileKey key, bool haveCached)
{
  return haveCached ? fromCache(key, true)
                    : outcome(ImageLoadResult::Outcome::Failed);
}

bool WmsImageryProvider::leftAlone()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (std::chrono::steady_clock::now() < m_leftAloneUntil) {
    ++m_statistics.skippedOffline;
    return true;
  }
  return false;
}

ImageLoadResult WmsImageryProvider::revalidated(TileKey key,
                                                const CachedTile& cached,
                                                const HttpResponse& response,
                                                long long nowS)
{
  const HttpFreshness previous{true, cached.expiresS, cached.etag,
                               cached.lastModified};
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

void WmsImageryProvider::store(TileKey key, const HttpResponse& response,
                               long long nowS)
{
  if (!m_cache) {
    return;
  }
  const HttpFreshness freshness =
      httpFreshness(response, nowS, m_settings.defaultLifetimeS);
  if (freshness.storable) {
    m_cache->store(key,
                   CachedTile{response.body, response.header("content-type"),
                              freshness.expiresS, freshness.etag,
                              freshness.lastModified});
  }
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
  if (leftAlone()) {
    return staleOrFailed(key, cached.has_value());
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
  if (response.error == NetworkError::TooLarge) {
    noteServerError(QCoreApplication::translate(
        "ts::WmsImageryProvider",
        "The server sent a tile that is far too large."));
    return outcome(ImageLoadResult::Outcome::Failed);
  }
  if (response.error != NetworkError::None) {
    noteUnreachable(response.errorString);
    return staleOrFailed(key, cached.has_value());
  }
  if (response.status == 304 && cached) {
    return revalidated(key, *cached, response, nowS);
  }
  if (response.status < 200 || response.status >= 300) {
    noteServerError(statusErrorMessage(response));
    return staleOrFailed(key, cached.has_value());
  }
  QImage image = decode(response.body);
  if (image.isNull()) {
    noteServerError(notAnImageMessage(response));
    return staleOrFailed(key, cached.has_value());
  }
  store(key, response, nowS);
  noteSuccess();
  return imageResult(std::move(image));
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
  if (!m_cache) {
    result.errorString = QCoreApplication::translate(
        "ts::WmsImageryProvider", "There is no disk cache to keep tiles in.");
    return result;
  }
  const long long nowS = now();
  const std::optional<CachedTile> cached = m_cache->find(key);
  if (cached && nowS < cached->expiresS &&
      (region == 0 || m_cache->pin(region, key))) {
    result.outcome = Outcome::AlreadyCached;
    result.bytes = cached->data.size();
    return result;
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
    const HttpFreshness renewed =
        revalidatedFreshness(HttpFreshness{true, cached->expiresS, cached->etag,
                                           cached->lastModified},
                             response, nowS, m_settings.defaultLifetimeS);
    m_cache->refresh(key, renewed.expiresS, renewed.etag, renewed.lastModified);
    if (region == 0 || m_cache->pin(region, key)) {
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
      httpFreshness(response, nowS, m_settings.defaultLifetimeS);
  if (!freshness.storable) {
    result.errorString = QCoreApplication::translate(
        "ts::WmsImageryProvider",
        "The server doesn't allow its tiles to be kept (no-store).");
    return result;
  }
  if (!m_cache->store(key,
                      CachedTile{response.body, response.header("content-type"),
                                 freshness.expiresS, freshness.etag,
                                 freshness.lastModified},
                      region)) {
    result.errorString =
        QCoreApplication::translate("ts::WmsImageryProvider",
                                    "The tile could not be saved: %1")
            .arg(m_cache->errorString());
    return result;
  }
  result.outcome = Outcome::Downloaded;
  result.bytes = response.body.size();
  return result;
}

} // namespace ts
