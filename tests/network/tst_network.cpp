// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QFile>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#include "concurrency.h"
#include "mockhttpserver.h"
#include "ts/deps/sqliteconnection.h"
#include "ts/network/httpcache.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"

namespace
{

using ts::test::MockHttpRequest;
using ts::test::MockHttpResponse;
using ts::test::MockHttpServer;
using ts::test::mockResponse;

/** Settings for tests: no proxy, short delays. */
ts::NetworkSettings testSettings()
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.retryDelay = std::chrono::milliseconds(50);
  settings.maximumRetryDelay = std::chrono::milliseconds(2000);
  settings.timeout = std::chrono::milliseconds(5000);
  return settings;
}

ts::HttpResponse
get(ts::NetworkClient& client, const QUrl& url,
    std::vector<std::pair<QByteArray, QByteArray>> headers = {})
{
  const std::atomic<bool> notCancelled{false};
  return client.fetch(ts::HttpRequest{url, std::move(headers)}, notCancelled);
}

/** A response with the given headers. */
ts::HttpResponse
withHeaders(std::vector<std::pair<QByteArray, QByteArray>> headers)
{
  ts::HttpResponse response;
  response.error = ts::NetworkError::None;
  response.status = 200;
  response.headers = std::move(headers);
  return response;
}

/** 2024-05-01 12:00:00 UTC. */
constexpr long long nowS = 1714564800;
constexpr long long dayS = 86400;

// Size, then the byte to fill with, as QByteArray(int, char).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ts::CachedTile tileOf(int bytes, char fill, long long expiresS = 0)
{
  ts::CachedTile tile;
  tile.data = QByteArray(bytes, fill);
  tile.contentType = QByteArrayLiteral("image/png");
  tile.expiresS = expiresS;
  return tile;
}

} // namespace

/** Unit tests for ts/network. */
class TestNetwork : public QObject
{
  Q_OBJECT

private slots:
  // --- NetworkClient
  // ----------------------------------------------------------

  void getsAResponse()
  {
    MockHttpServer server([](const MockHttpRequest& request) {
      MockHttpResponse response = mockResponse(
          200, "hello " + request.url.query(QUrl::FullyEncoded).toLatin1());
      response.headers.emplace_back("X-Test", "Yes");
      return response;
    });
    ts::NetworkClient client(testSettings());
    const ts::HttpResponse response =
        get(client, server.url(QStringLiteral("/a?b=c")));
    QCOMPARE(response.error, ts::NetworkError::None);
    QCOMPARE(response.status, 200);
    QVERIFY(response.isSuccess());
    QCOMPARE(response.body, QByteArray("hello b=c"));
    QCOMPARE(response.header("x-test"), QByteArray("Yes"));
    QCOMPARE(response.header("content-type"), QByteArray("text/plain"));
    QCOMPARE(response.attempts, 1);
    QCOMPARE(server.requestCount(), 1);
    const MockHttpRequest sent = server.requests().front();
    QCOMPARE(sent.method, QByteArray("GET"));
    QVERIFY(sent.header("user-agent").startsWith("TerraSoar/"));
    QCOMPARE(client.inFlight(), 0);
  }

  void sendsRequestHeaders()
  {
    MockHttpServer server([](const MockHttpRequest& request) {
      return mockResponse(request.header("if-none-match") == "\"v1\"" ? 304
                                                                      : 200,
                          QByteArray());
    });
    ts::NetworkClient client(testSettings());
    const ts::HttpResponse response =
        get(client, server.url(),
            ts::revalidationHeaders("\"v1\"", "Wed, 01 May 2024 10:00:00 GMT"));
    QCOMPARE(response.status, 304);
    QVERIFY(response.isSuccess());
    QCOMPARE(server.requests().front().header("if-modified-since"),
             QByteArray("Wed, 01 May 2024 10:00:00 GMT"));
  }

  void httpErrorsAreResponses()
  {
    // A 404 is an answer: final, not retried.
    MockHttpServer server(
        [](const MockHttpRequest&) { return mockResponse(404, "gone"); });
    ts::NetworkClient client(testSettings());
    const ts::HttpResponse response = get(client, server.url());
    QCOMPARE(response.error, ts::NetworkError::None);
    QCOMPARE(response.status, 404);
    QVERIFY(!response.isSuccess());
    QCOMPARE(response.body, QByteArray("gone"));
    QCOMPARE(response.attempts, 1);
    QCOMPARE(server.requestCount(), 1);
  }

  void retriesWithBackOff()
  {
    // Two 503s, then success: three attempts, 50 + 100 ms apart.
    std::atomic<int> calls{0};
    MockHttpServer server([&calls](const MockHttpRequest&) {
      return ++calls <= 2 ? mockResponse(503, "busy") : mockResponse(200, "ok");
    });
    ts::NetworkClient client(testSettings());
    QElapsedTimer timer;
    timer.start();
    const ts::HttpResponse response = get(client, server.url());
    QCOMPARE(response.status, 200);
    QCOMPARE(response.body, QByteArray("ok"));
    QCOMPARE(response.attempts, 3);
    QVERIFY(timer.elapsed() >= 150);
    QCOMPARE(client.attemptsSent(), 3);

    // Always failing: the last answer after the last attempt.
    calls = -100;
    const ts::HttpResponse failed = get(client, server.url());
    QCOMPARE(failed.status, 503);
    QCOMPARE(failed.attempts, 3);
  }

  void honoursRetryAfterWithinLimit()
  {
    std::atomic<int> calls{0};
    MockHttpServer server([&calls](const MockHttpRequest&) {
      if (++calls == 1) {
        MockHttpResponse response = mockResponse(429, "slow down");
        response.headers.emplace_back("Retry-After", "1");
        return response;
      }
      return mockResponse(200, "ok");
    });
    ts::NetworkClient client(testSettings());
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(get(client, server.url()).status, 200);
    QVERIFY(timer.elapsed() >= 1000);
    // A server asking for an hour gets the limit (2 s here).
    calls = 0;
    server.setHandler([&calls](const MockHttpRequest&) {
      if (++calls == 1) {
        MockHttpResponse response = mockResponse(503, "maintenance");
        response.headers.emplace_back("Retry-After", "3600");
        return response;
      }
      return mockResponse(200, "ok");
    });
    timer.restart();
    QCOMPARE(get(client, server.url()).status, 200);
    QVERIFY(timer.elapsed() < 3500);
  }

  void connectionFailures()
  {
    // A port nobody listens on.
    quint16 closedPort = 0;
    {
      QTcpServer probe;
      QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
      closedPort = probe.serverPort();
    }
    ts::NetworkClient client(testSettings());
    const ts::HttpResponse refused = get(
        client, QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(closedPort)));
    QCOMPARE(refused.error, ts::NetworkError::ConnectionFailed);
    QCOMPARE(refused.attempts, 3);
    QVERIFY(!refused.errorString.isEmpty());

    // A server that hangs up without answering.
    MockHttpServer server([](const MockHttpRequest&) {
      MockHttpResponse response;
      response.hangUp = true;
      return response;
    });
    const ts::HttpResponse hungUp = get(client, server.url());
    QCOMPARE(hungUp.error, ts::NetworkError::ConnectionFailed);
    QCOMPARE(hungUp.attempts, 3);
    // Qt itself resends a request once when the server hangs up before
    // answering, so the server may see more than three.
    QVERIFY(server.requestCount() >= 3);
  }

  void timesOut()
  {
    MockHttpServer server([](const MockHttpRequest&) {
      MockHttpResponse response;
      response.stall = true;
      return response;
    });
    ts::NetworkSettings settings = testSettings();
    settings.timeout = std::chrono::milliseconds(200);
    settings.attempts = 2;
    ts::NetworkClient client(settings);
    QElapsedTimer timer;
    timer.start();
    const ts::HttpResponse response = get(client, server.url());
    QCOMPARE(response.error, ts::NetworkError::Timeout);
    QCOMPARE(response.attempts, 2);
    QVERIFY(timer.elapsed() >= 400);
    QVERIFY(timer.elapsed() < 4000);
  }

  void refusesOversizedResponses()
  {
    constexpr int threeMiB = 3 * 1024 * 1024;
    MockHttpServer server([](const MockHttpRequest&) {
      return mockResponse(200, QByteArray(threeMiB, 'x'), "image/png");
    });
    ts::NetworkSettings settings = testSettings();
    settings.maximumResponseBytes = qint64{1024} * 1024;
    ts::NetworkClient client(settings);
    const ts::HttpResponse response = get(client, server.url());
    QCOMPARE(response.error, ts::NetworkError::TooLarge);
    QVERIFY(response.body.isEmpty());
    QCOMPARE(response.attempts, 1); // not worth retrying
  }

  void followsRedirects()
  {
    MockHttpServer server([](const MockHttpRequest& request) {
      if (request.url.path() == QLatin1String("/old")) {
        MockHttpResponse response = mockResponse(302, QByteArray());
        response.headers.emplace_back("Location", "/new");
        return response;
      }
      return mockResponse(200, request.url.path().toLatin1());
    });
    ts::NetworkClient client(testSettings());
    const ts::HttpResponse response =
        get(client, server.url(QStringLiteral("/old")));
    QCOMPARE(response.status, 200);
    QCOMPARE(response.body, QByteArray("/new"));
    QCOMPARE(response.finalUrl.path(), QStringLiteral("/new"));
  }

  void cancellationDropsTheConnection()
  {
    // A slow answer; the caller gives up after 100 ms. fetch() returns at
    // once and the server sees the client hang up.
    MockHttpServer server([](const MockHttpRequest&) {
      MockHttpResponse response = mockResponse(200, "late");
      response.delayMs = 5000;
      return response;
    });
    ts::NetworkClient client(testSettings());
    std::atomic<bool> cancelled{false};
    std::thread canceller([&cancelled]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      cancelled.store(true);
    });
    QElapsedTimer timer;
    timer.start();
    const ts::HttpResponse response =
        client.fetch(ts::HttpRequest{server.url(), {}}, cancelled);
    canceller.join();
    QCOMPARE(response.error, ts::NetworkError::Cancelled);
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(
        QTest::qWaitFor([&]() { return server.abandonedCount() == 1; }, 3000));
    QVERIFY(QTest::qWaitFor([&]() { return client.inFlight() == 0; }, 3000));
    QCOMPARE(server.requestCount(), 1);
  }

  void ticketsCanBePolledAndCancelled()
  {
    MockHttpServer server([](const MockHttpRequest& request) {
      MockHttpResponse response = mockResponse(200, "done");
      response.delayMs =
          request.url.path() == QLatin1String("/slow") ? 5000 : 0;
      return response;
    });
    ts::NetworkClient client(testSettings());
    const std::shared_ptr<ts::NetworkTicket> quick =
        client.start(ts::HttpRequest{server.url(QStringLiteral("/quick")), {}});
    const std::shared_ptr<ts::NetworkTicket> slow =
        client.start(ts::HttpRequest{server.url(QStringLiteral("/slow")), {}});
    QVERIFY(QTest::qWaitFor([&]() { return quick->isFinished(); }, 5000));
    QCOMPARE(quick->response().body, QByteArray("done"));
    QVERIFY(!slow->isFinished());
    slow->cancel();
    QVERIFY(slow->isCancelled());
    QVERIFY(slow->waitFor(std::chrono::milliseconds(3000)));
    QCOMPARE(slow->response().error, ts::NetworkError::Cancelled);
  }

  void limitsRequestsInFlight()
  {
    // 12 requests, at most 3 on the wire; all complete.
    MockHttpServer server([](const MockHttpRequest&) {
      MockHttpResponse response = mockResponse(200, "ok");
      response.delayMs = 100;
      return response;
    });
    ts::NetworkSettings settings = testSettings();
    settings.maximumInFlight = 3;
    ts::NetworkClient client(settings);
    std::vector<std::shared_ptr<ts::NetworkTicket>> tickets;
    tickets.reserve(12);
    for (int i = 0; i < 12; ++i) {
      tickets.push_back(client.start(
          ts::HttpRequest{server.url(QStringLiteral("/%1").arg(i)), {}}));
    }
    for (const std::shared_ptr<ts::NetworkTicket>& ticket : tickets) {
      QVERIFY(ticket->waitFor(std::chrono::milliseconds(10000)));
      QCOMPARE(ticket->response().status, 200);
    }
    QCOMPARE(client.peakInFlight(), 3);
    QVERIFY(server.peakWaiting() <= 3);
    QCOMPARE(server.requestCount(), 12);
    // Cancelled while still queued: finished at once, never sent.
    server.resetLog();
    std::vector<std::shared_ptr<ts::NetworkTicket>> more;
    more.reserve(9);
    for (int i = 0; i < 9; ++i) {
      more.push_back(client.start(
          ts::HttpRequest{server.url(QStringLiteral("/%1").arg(i)), {}}));
    }
    for (std::size_t i = 3; i < more.size(); ++i) {
      more[i]->cancel();
    }
    for (std::size_t i = 3; i < more.size(); ++i) {
      QVERIFY(more[i]->waitFor(std::chrono::milliseconds(3000)));
      QCOMPARE(more[i]->response().error, ts::NetworkError::Cancelled);
    }
    for (std::size_t i = 0; i < 3; ++i) {
      QVERIFY(more[i]->waitFor(std::chrono::milliseconds(5000)));
    }
    QCOMPARE(server.requestCount(), 3);
  }

  void usesTheProxy()
  {
    // The proxy gets the absolute URL; the origin is never contacted.
    MockHttpServer proxy([](const MockHttpRequest& request) {
      return mockResponse(200, "via proxy: " + request.target);
    });
    ts::NetworkSettings settings = testSettings();
    settings.proxy = QNetworkProxy(QNetworkProxy::HttpProxy,
                                   QStringLiteral("127.0.0.1"), proxy.port());
    ts::NetworkClient client(settings);
    const ts::HttpResponse response =
        get(client, QUrl(QStringLiteral("http://wms.example.invalid/x?y=1")));
    QCOMPARE(response.status, 200);
    QCOMPARE(response.body,
             QByteArray("via proxy: http://wms.example.invalid/x?y=1"));
  }

  void destructionCancelsWhatIsOpen()
  {
    MockHttpServer server([](const MockHttpRequest&) {
      MockHttpResponse response;
      response.stall = true;
      return response;
    });
    std::vector<std::shared_ptr<ts::NetworkTicket>> tickets;
    {
      ts::NetworkSettings settings = testSettings();
      settings.maximumInFlight = 2;
      ts::NetworkClient client(settings);
      for (int i = 0; i < 5; ++i) {
        tickets.push_back(client.start(ts::HttpRequest{server.url(), {}}));
      }
      QVERIFY(
          QTest::qWaitFor([&]() { return server.requestCount() == 2; }, 3000));
    }
    for (const std::shared_ptr<ts::NetworkTicket>& ticket : tickets) {
      QVERIFY(ticket->isFinished());
      QCOMPARE(ticket->response().error, ts::NetworkError::Cancelled);
    }
  }

  void concurrentFetches()
  {
    // Many loader-like threads sharing one client (under TSan in CI).
    MockHttpServer server([](const MockHttpRequest& request) {
      return mockResponse(200, request.url.path().toLatin1());
    });
    ts::NetworkClient client(testSettings());
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&client, &server](int thread) {
          bool ok = true;
          for (int i = 0; i < 8; ++i) {
            const QString path = QStringLiteral("/%1/%2").arg(thread).arg(i);
            const std::atomic<bool> notCancelled{false};
            const ts::HttpResponse response = client.fetch(
                ts::HttpRequest{server.url(path), {}}, notCancelled);
            ok = ok && response.status == 200 &&
                 response.body == path.toLatin1();
          }
          return ok;
        });
    QCOMPARE(failures, 0);
    QVERIFY(client.peakInFlight() <= client.settings().maximumInFlight);
  }

  // --- HTTP freshness
  // ---------------------------------------------------------

  void parsesHttpDates()
  {
    // RFC 9110's example, in its three formats.
    const long long expected = 784111777;
    QCOMPARE(ts::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT"),
             std::optional<long long>(expected));
    QCOMPARE(ts::parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT"),
             std::optional<long long>(expected));
    QCOMPARE(ts::parseHttpDate("Sun Nov  6 08:49:37 1994"),
             std::optional<long long>(expected));
    QCOMPARE(ts::parseHttpDate("Wed, 01 May 2024 12:00:00 GMT"),
             std::optional<long long>(nowS));
    QCOMPARE(ts::parseHttpDate("Thu, 29 Feb 2024 23:59:59 GMT"),
             std::optional<long long>(1709251199));
    QVERIFY(!ts::parseHttpDate(""));
    QVERIFY(!ts::parseHttpDate("0"));
    QVERIFY(!ts::parseHttpDate("-1"));
    QVERIFY(!ts::parseHttpDate("tomorrow"));
    QVERIFY(!ts::parseHttpDate("Sun, 06 Nov 1994 25:49:37 GMT"));
  }

  void freshnessFromHeaders()
  {
    const long long week = 7 * dayS;
    // Nothing said: the default.
    ts::HttpFreshness f = ts::httpFreshness(withHeaders({}), nowS, week);
    QVERIFY(f.storable);
    QCOMPARE(f.expiresS, nowS + week);
    // max-age, less Age.
    f = ts::httpFreshness(
        withHeaders(
            {{"cache-control", "public, max-age=3600"}, {"age", "600"}}),
        nowS, week);
    QCOMPARE(f.expiresS, nowS + 3000);
    // max-age wins over Expires.
    f = ts::httpFreshness(
        withHeaders({{"cache-control", "max-age=60"},
                     {"expires", "Thu, 02 May 2024 12:00:00 GMT"}}),
        nowS, week);
    QCOMPARE(f.expiresS, nowS + 60);
    // Expires relative to the server's Date (clocks differ).
    f = ts::httpFreshness(
        withHeaders({{"date", "Wed, 01 May 2024 11:00:00 GMT"},
                     {"expires", "Wed, 01 May 2024 13:00:00 GMT"}}),
        nowS, week);
    QCOMPARE(f.expiresS, nowS + 7200);
    // An invalid Expires: stale at once.
    f = ts::httpFreshness(withHeaders({{"expires", "0"}}), nowS, week);
    QCOMPARE(f.expiresS, nowS);
    // no-cache: kept, revalidated every time.
    f = ts::httpFreshness(
        withHeaders({{"cache-control", "no-cache"}, {"etag", "\"a\""}}), nowS,
        week);
    QVERIFY(f.storable);
    QCOMPARE(f.expiresS, nowS);
    QCOMPARE(f.etag, QByteArray("\"a\""));
    // no-store: not kept.
    f = ts::httpFreshness(withHeaders({{"cache-control", "No-Store"}}), nowS,
                          week);
    QVERIFY(!f.storable);
    // Last-Modified heuristic: a tenth of the age, capped at the default.
    f = ts::httpFreshness(
        withHeaders({{"last-modified", "Mon, 22 Apr 2024 12:00:00 GMT"}}), nowS,
        week);
    QCOMPARE(f.expiresS, nowS + 9 * dayS / 10);
    QCOMPARE(f.lastModified, QByteArray("Mon, 22 Apr 2024 12:00:00 GMT"));
    f = ts::httpFreshness(
        withHeaders({{"last-modified", "Sun, 01 Jan 2023 00:00:00 GMT"}}), nowS,
        week);
    QCOMPARE(f.expiresS, nowS + week);
    // A 304 keeps the old validators if it sends none.
    ts::HttpFreshness previous;
    previous.etag = "\"v1\"";
    previous.lastModified = "Mon, 22 Apr 2024 12:00:00 GMT";
    f = ts::revalidatedFreshness(
        previous, withHeaders({{"cache-control", "max-age=10"}}), nowS, week);
    QCOMPARE(f.expiresS, nowS + 10);
    QCOMPARE(f.etag, previous.etag);
    QCOMPARE(f.lastModified, previous.lastModified);
    QVERIFY(ts::revalidationHeaders({}, {}).empty());
    QCOMPARE(ts::revalidationHeaders("\"x\"", {}).size(), std::size_t{1});
  }

  // --- TileDiskCache
  // ----------------------------------------------------------

  void cacheStoresAndFinds()
  {
    QTemporaryDir dir;
    ts::TileDiskCache cache(dir.filePath(QStringLiteral("sub/dir/c.sqlite")),
                            1024LL * 1024);
    const ts::TileKey key{5, 17, 9};
    QVERIFY(!cache.find(key));
    ts::CachedTile tile = tileOf(1000, 'a', nowS + 60);
    tile.etag = "\"e\"";
    tile.lastModified = "Mon, 22 Apr 2024 12:00:00 GMT";
    QVERIFY(cache.store(key, tile));
    const std::optional<ts::CachedTile> found = cache.find(key);
    QVERIFY(found.has_value());
    QCOMPARE(found.value_or(ts::CachedTile{}).data, tile.data);
    QCOMPARE(found.value_or(ts::CachedTile{}).contentType,
             QByteArray("image/png"));
    QCOMPARE(found.value_or(ts::CachedTile{}).expiresS, nowS + 60);
    QCOMPARE(found.value_or(ts::CachedTile{}).etag, tile.etag);
    QCOMPARE(found.value_or(ts::CachedTile{}).lastModified, tile.lastModified);
    QCOMPARE(cache.sizeBytes(), 1000LL);
    QCOMPARE(cache.count(), 1);
    // Replacing keeps one copy.
    QVERIFY(cache.store(key, tileOf(400, 'b')));
    QCOMPARE(cache.sizeBytes(), 400LL);
    QCOMPARE(cache.count(), 1);
    // Refresh after a 304.
    QVERIFY(cache.refresh(key, nowS + 99, "\"f\"", {}));
    QCOMPARE(cache.find(key).value_or(ts::CachedTile{}).expiresS, nowS + 99);
    QCOMPARE(cache.find(key).value_or(ts::CachedTile{}).etag,
             QByteArray("\"f\""));
    QVERIFY(!cache.refresh(ts::TileKey{5, 0, 0}, nowS, {}, {}));
    // Keys far apart don't collide.
    QVERIFY(cache.store(ts::TileKey{24, (1 << 25) - 1, (1 << 24) - 1},
                        tileOf(10, 'z')));
    QCOMPARE(cache.count(), 2);
    cache.remove(key);
    QVERIFY(!cache.find(key));
    QCOMPARE(cache.sizeBytes(), 10LL);
    cache.clear();
    QCOMPARE(cache.count(), 0);
    QCOMPARE(cache.sizeBytes(), 0LL);
    QCOMPARE(cache.recoveries(), 0);
  }

  void cacheEvictsLeastRecentlyUsed()
  {
    // 10 KB tiles into a 50 KB cache: the oldest go, unless used lately.
    QTemporaryDir dir;
    ts::TileDiskCache cache(dir.filePath(QStringLiteral("c.sqlite")),
                            50LL * 1000);
    for (int i = 0; i < 5; ++i) {
      QVERIFY(cache.store(ts::TileKey{3, i, 0}, tileOf(10000, 'a')));
    }
    QCOMPARE(cache.count(), 5);
    QVERIFY(cache.find(ts::TileKey{3, 0, 0})); // 0 is now the newest
    QVERIFY(cache.store(ts::TileKey{3, 5, 0}, tileOf(10000, 'b')));
    // Over the limit: down to 45 KB, so the two least recently used go.
    QCOMPARE(cache.sizeBytes(), 40000LL);
    QVERIFY(cache.find(ts::TileKey{3, 0, 0}));
    QVERIFY(!cache.find(ts::TileKey{3, 1, 0}));
    QVERIFY(!cache.find(ts::TileKey{3, 2, 0}));
    QVERIFY(cache.find(ts::TileKey{3, 3, 0}));
    QVERIFY(cache.find(ts::TileKey{3, 5, 0}));
    QVERIFY(cache.sizeBytes() <= cache.maximumBytes());
  }

  void cacheSurvivesRestart()
  {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("c.sqlite"));
    {
      ts::TileDiskCache cache(path, 50LL * 1000);
      QVERIFY(cache.store(ts::TileKey{1, 0, 0}, tileOf(10000, 'x', nowS)));
      QVERIFY(cache.store(ts::TileKey{1, 1, 0}, tileOf(10000, 'y', nowS)));
      QVERIFY(cache.find(ts::TileKey{1, 0, 0}));
    }
    ts::TileDiskCache cache(path, 50LL * 1000);
    QCOMPARE(cache.count(), 2);
    QCOMPARE(cache.sizeBytes(), 20000LL);
    QCOMPARE(cache.find(ts::TileKey{1, 1, 0}).value_or(ts::CachedTile{}).data,
             QByteArray(10000, 'y'));
    // The order of use survived too: 1/0/0 was used last before the
    // restart, then 1/1/0 now. Filling up to 55 KB evicts 1/0/0 only.
    for (int i = 2; i < 5; ++i) {
      QVERIFY(cache.store(ts::TileKey{1, i, 0}, tileOf(10000, 'z')));
    }
    QVERIFY(cache.store(ts::TileKey{2, 0, 0}, tileOf(5000, 'w')));
    QCOMPARE(cache.sizeBytes(), 45000LL);
    QVERIFY(!cache.find(ts::TileKey{1, 0, 0}));
    QVERIFY(cache.find(ts::TileKey{1, 1, 0}));
  }

  void cacheRecoversFromDamage_data()
  {
    QTest::addColumn<int>("damage");
    QTest::newRow("garbage") << 0;
    QTest::newRow("truncated") << 1;
    QTest::newRow("overwritten pages") << 2;
    QTest::newRow("empty file") << 3;
  }

  void cacheRecoversFromDamage()
  {
    // A damaged file is replaced by an empty cache that works.
    QFETCH(int, damage);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("c.sqlite"));
    {
      ts::TileDiskCache cache(path, 10LL * 1000 * 1000);
      for (int i = 0; i < 200; ++i) {
        QVERIFY(cache.store(ts::TileKey{8, i, 3}, tileOf(5000, 'q')));
      }
    }
    // Fold the WAL into the main file so that damaging it counts.
    QFile file(path);
    QVERIFY(file.exists());
    QVERIFY(file.open(QIODevice::ReadWrite));
    const qint64 size = file.size();
    if (damage == 0) {
      file.resize(0);
      file.write(QByteArray(4096, '\x5a'));
    } else if (damage == 1) {
      file.resize(size / 3);
    } else if (damage == 2) {
      for (qint64 at = 0; at < size; at += qint64{4096} * 3) {
        file.seek(at);
        file.write(QByteArray(512, '\x01'));
      }
    } else {
      file.resize(0);
    }
    file.close();
    for (const char* suffix : {"-wal", "-shm"}) {
      QFile::remove(path + QLatin1String(suffix));
    }

    ts::TileDiskCache cache(path, 10LL * 1000 * 1000);
    // Reading may meet the damage now or later; either way it recovers.
    for (int i = 0; i < 200; ++i) {
      cache.find(ts::TileKey{8, i, 3});
    }
    QVERIFY(cache.store(ts::TileKey{0, 0, 0}, tileOf(100, 'n')));
    QCOMPARE(cache.find(ts::TileKey{0, 0, 0}).value_or(ts::CachedTile{}).data,
             QByteArray(100, 'n'));
    if (damage != 3) {
      QVERIFY2(cache.recoveries() >= 1, qPrintable(cache.errorString()));
    }
  }

  void cacheOfAnotherVersionStartsOver()
  {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("c.sqlite"));
    {
      ts::TileDiskCache cache(path, 1000LL * 1000);
      QVERIFY(cache.store(ts::TileKey{1, 0, 0}, tileOf(10, 'a')));
    }
    {
      ts::SqliteConnection db(path, ts::SqliteConnection::OpenMode::ReadWrite);
      QVERIFY(db.execute(QStringLiteral("PRAGMA user_version=99")));
    }
    ts::TileDiskCache cache(path, 1000LL * 1000);
    QCOMPARE(cache.count(), 0);
    QVERIFY(cache.store(ts::TileKey{1, 0, 0}, tileOf(10, 'b')));
  }

  void cacheSharedBetweenThreads()
  {
    QTemporaryDir dir;
    ts::TileDiskCache cache(dir.filePath(QStringLiteral("c.sqlite")),
                            200LL * 1000);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&cache](int thread) {
          bool ok = true;
          for (int i = 0; i < 40; ++i) {
            const ts::TileKey key{10, thread, i};
            ok = ok && cache.store(
                           key, tileOf(1000, static_cast<char>('a' + i % 26)));
            const std::optional<ts::CachedTile> found = cache.find(key);
            // It may have been evicted by another thread meanwhile.
            ok = ok && (!found || found->data.size() == 1000);
          }
          return ok;
        });
    QCOMPARE(failures, 0);
    QVERIFY(cache.sizeBytes() <= cache.maximumBytes());
  }

  void benchmarkCacheFind()
  {
    QTemporaryDir dir;
    ts::TileDiskCache cache(dir.filePath(QStringLiteral("c.sqlite")),
                            100LL * 1000 * 1000);
    for (int i = 0; i < 256; ++i) {
      cache.store(ts::TileKey{8, i, 0}, tileOf(20000, 'b'));
    }
    int i = 0;
    QBENCHMARK
    {
      const std::optional<ts::CachedTile> tile =
          cache.find(ts::TileKey{8, i++ % 256, 0});
      QVERIFY(tile.has_value());
    }
  }
};

QTEST_GUILESS_MAIN(TestNetwork)
#include "tst_network.moc"
