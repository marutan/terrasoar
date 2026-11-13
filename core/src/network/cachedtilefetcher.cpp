// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/network/cachedtilefetcher.h"

#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>

#include "ts/network/httpcache.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"

namespace ts
{

namespace
{

std::unique_ptr<TileDiskCache> makeCache(const QString& sourceId,
                                         const WebTileSettings& settings)
{
  if (settings.cacheDirectory.isEmpty()) {
    return nullptr;
  }
  return std::make_unique<TileDiskCache>(
      QDir(settings.cacheDirectory).filePath(webCacheFileName(sourceId)),
      settings.cacheBytes);
}

} // namespace

QString webCacheFileName(const QString& sourceId)
{
  return QString::fromLatin1(QCryptographicHash::hash(sourceId.toUtf8(),
                                                      QCryptographicHash::Sha1)
                                 .toHex()) +
         QStringLiteral(".sqlite");
}

CachedTileFetcher::CachedTileFetcher(
    const QString& sourceId, QString host,
    std::shared_ptr<NetworkClient> network, WebTileSettings settings,
    Messages messages, std::function<void(const ProviderStatus&)> report)
    : m_host(std::move(host)), m_network(std::move(network)),
      m_settings(std::move(settings)), m_messages(std::move(messages)),
      m_report(std::move(report)), m_cache(makeCache(sourceId, m_settings))
{
}

CachedTileFetcher::~CachedTileFetcher() = default;

WebTileStatistics CachedTileFetcher::statistics() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_statistics;
}

void CachedTileFetcher::countRequest()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  ++m_statistics.requests;
}

long long CachedTileFetcher::now() const
{
  return m_settings.clock ? m_settings.clock()
                          : QDateTime::currentSecsSinceEpoch();
}

FetchOutcome CachedTileFetcher::fromCache(
    TileKey key, bool stale,
    const std::function<bool(const QByteArray&)>& accept)
{
  // The caller found it in the cache moments ago; read it again.
  const std::optional<CachedTile> cached =
      m_cache ? m_cache->find(key) : std::nullopt;
  if (!cached) {
    return FetchOutcome::Failed;
  }
  if (!accept(cached->data)) {
    m_cache->remove(key);
    return FetchOutcome::Failed;
  }
  if (stale) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.servedStale;
  }
  return FetchOutcome::Data;
}

void CachedTileFetcher::noteSuccess()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_unreachableInARow = 0;
  }
  if (m_report) {
    m_report(ProviderStatus{});
  }
}

void CachedTileFetcher::noteUnreachable(const QString& detail)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.failures;
    if (++m_unreachableInARow >= m_settings.failuresBeforeOffline) {
      m_leftAloneUntil =
          std::chrono::steady_clock::now() + m_settings.offlineRetry;
    }
  }
  if (m_report) {
    m_report(ProviderStatus{
        ProviderStatus::State::Offline,
        QCoreApplication::translate("ts::CachedTileFetcher",
                                    "Can't reach %1 (%2). Saved tiles are "
                                    "shown where there are some.")
            .arg(m_host, detail)});
  }
}

void CachedTileFetcher::noteServerError(const QString& message)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    ++m_statistics.failures;
    m_unreachableInARow = 0;
  }
  if (m_report) {
    m_report(ProviderStatus{ProviderStatus::State::Error, message});
  }
}

FetchOutcome CachedTileFetcher::staleOrFailed(
    TileKey key, bool haveCached,
    const std::function<bool(const QByteArray&)>& accept)
{
  return haveCached ? fromCache(key, true, accept) : FetchOutcome::Failed;
}

bool CachedTileFetcher::leftAlone()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (std::chrono::steady_clock::now() < m_leftAloneUntil) {
    ++m_statistics.skippedOffline;
    return true;
  }
  return false;
}

FetchOutcome CachedTileFetcher::revalidated(
    TileKey key, const CachedTile& cached, const HttpResponse& response,
    long long nowS, const std::function<bool(const QByteArray&)>& accept)
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
  return fromCache(key, false, accept);
}

void CachedTileFetcher::store(TileKey key, const HttpResponse& response,
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

FetchOutcome
CachedTileFetcher::fetch(TileKey key, const QUrl& url,
                         const std::function<bool(const QByteArray&)>& accept,
                         const std::atomic<bool>& cancelled)
{
  const long long nowS = now();
  const std::optional<CachedTile> cached =
      m_cache ? m_cache->find(key) : std::nullopt;
  if (cached && nowS < cached->expiresS) {
    if (accept(cached->data)) {
      const std::lock_guard<std::mutex> lock(m_mutex);
      ++m_statistics.cacheHits;
      return FetchOutcome::Data;
    }
    m_cache->remove(key);
  }
  if (leftAlone()) {
    return staleOrFailed(key, cached.has_value(), accept);
  }

  HttpRequest request;
  request.url = url;
  if (cached) {
    request.headers = revalidationHeaders(cached->etag, cached->lastModified);
  }
  countRequest();
  const HttpResponse response = m_network->fetch(request, cancelled);
  if (response.error == NetworkError::Cancelled || cancelled.load()) {
    return FetchOutcome::Cancelled;
  }
  if (response.error == NetworkError::TooLarge) {
    noteServerError(QCoreApplication::translate(
        "ts::CachedTileFetcher",
        "The server sent a tile that is far too large."));
    return FetchOutcome::Failed;
  }
  if (response.error != NetworkError::None) {
    noteUnreachable(response.errorString);
    return staleOrFailed(key, cached.has_value(), accept);
  }
  if (response.status == 304 && cached) {
    return revalidated(key, *cached, response, nowS, accept);
  }
  if (m_messages.notFoundIsNoData &&
      (response.status == 404 || response.status == 204)) {
    noteSuccess();
    return FetchOutcome::NoData;
  }
  if (response.status < 200 || response.status >= 300) {
    noteServerError(m_messages.statusError(response));
    return staleOrFailed(key, cached.has_value(), accept);
  }
  if (!accept(response.body)) {
    noteServerError(m_messages.notATile(response));
    return staleOrFailed(key, cached.has_value(), accept);
  }
  store(key, response, nowS);
  noteSuccess();
  return FetchOutcome::Data;
}

} // namespace ts
