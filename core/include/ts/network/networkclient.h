// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * HTTP requests on a network thread, for loader threads and the GUI.
 */
#ifndef TS_NETWORK_NETWORKCLIENT_H
#define TS_NETWORK_NETWORKCLIENT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QNetworkProxy>
#include <QString>
#include <QUrl>

namespace ts
{

class NetworkCore;

/** An HTTP GET request. */
struct HttpRequest {
  /** What to get (http or https). */
  QUrl url;
  /** Extra request headers, such as If-None-Match. */
  std::vector<std::pair<QByteArray, QByteArray>> headers;
};

/** Why a request did not give a response. */
enum class NetworkError {
  None,             ///< A response arrived (whatever its HTTP status).
  Cancelled,        ///< Cancelled before it finished.
  Timeout,          ///< No data for NetworkSettings::timeout.
  ConnectionFailed, ///< The server or proxy could not be reached.
  TooLarge,         ///< The response exceeded the size limit.
  Other,            ///< Anything else (TLS, protocol errors).
};

/** What came back. */
struct HttpResponse {
  /** NetworkError::None if a response arrived. */
  NetworkError error = NetworkError::Other;
  /** HTTP status (200, 304, 404, …); 0 without a response. */
  int status = 0;
  /** The body. */
  QByteArray body;
  /** Response headers; names in lower case. */
  std::vector<std::pair<QByteArray, QByteArray>> headers;
  /** What went wrong, for logs and the user (not translated). */
  QString errorString;
  /** How many attempts were made. */
  int attempts = 0;
  /** The URL that answered, after redirects. */
  QUrl finalUrl;

  /** Returns the value of header @p lowerCaseName, or an empty array. */
  QByteArray header(const QByteArray& lowerCaseName) const;

  /** Returns true if a response arrived with status 2xx or 304. */
  bool isSuccess() const
  {
    return error == NetworkError::None &&
           ((status >= 200 && status < 300) || status == 304);
  }
};

/** Limits and behaviour of a NetworkClient. */
struct NetworkSettings {
  /**
   * Most requests on the wire at once, all hosts together; the rest
   * wait in order. The roadmap's "network requests in flight" budget.
   */
  int maximumInFlight = 6;
  /** A request that receives nothing for this long fails (Timeout). */
  std::chrono::milliseconds timeout{15000};
  /**
   * Attempts per request. Timeouts, connection failures, HTTP 408, 429
   * and 5xx are retried; other answers are final.
   */
  int attempts = 3;
  /** Wait before the first retry; it doubles for each further one. */
  std::chrono::milliseconds retryDelay{500};
  /** Longest wait before a retry, also when a server asks for more. */
  std::chrono::milliseconds maximumRetryDelay{8000};
  /** Largest response body accepted, bytes. */
  qint64 maximumResponseBytes = qint64{16} * 1024 * 1024;
  /** The User-Agent header. */
  QByteArray userAgent = QByteArrayLiteral("TerraSoar/" TS_VERSION_STRING);
  /**
   * The proxy for every request. Without one, the system's settings are
   * used (QNetworkProxyFactory::systemProxyForQuery(): the proxy
   * environment variables on Linux, the system settings on Windows).
   * Tests set QNetworkProxy::NoProxy.
   */
  std::optional<QNetworkProxy> proxy;
};

/**
 * A request started with NetworkClient::start(), for a caller that
 * polls (the GUI thread) or waits.
 *
 * @threadsafe
 */
class NetworkTicket
{
public:
  /** Returns true once the response (or failure) is there. */
  bool isFinished() const;

  /** Returns the response; only meaningful once isFinished(). */
  HttpResponse response() const;

  /**
   * Waits until finished, at most @p timeout. Returns isFinished().
   */
  bool waitFor(std::chrono::milliseconds timeout) const;

  /**
   * Cancels the request: it finishes soon with NetworkError::Cancelled
   * (unless it had finished already) and its connection is dropped.
   */
  void cancel();

  /** Returns true if cancel() was called. */
  bool isCancelled() const { return m_cancelled.load(); }

  /** For NetworkClient: a request to make for the client @p core. */
  NetworkTicket(HttpRequest request, std::weak_ptr<NetworkCore> core);

  /** Returns the request. */
  const HttpRequest& request() const { return m_request; }

  /** For the client: sets the response and wakes waiters (first call only). */
  void finish(HttpResponse response);

private:
  const HttpRequest m_request;
  const std::weak_ptr<NetworkCore> m_core;
  std::atomic<bool> m_cancelled{false};
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_finishedCondition;
  bool m_finished = false;
  HttpResponse m_response;
};

/**
 * Makes HTTP requests on its own thread with a QNetworkAccessManager.
 *
 * Requests come from any thread. Tile loader threads call fetch(), which
 * blocks until the response arrives or the tile is cancelled; the GUI
 * thread calls start() and polls the ticket from a timer. Either way the
 * client keeps at most NetworkSettings::maximumInFlight requests on the
 * wire, retries what is worth retrying with growing pauses (honouring a
 * server's Retry-After within the limit), fails requests that stall, drops
 * the connection of a cancelled request at once, and refuses bodies over
 * the size limit. Redirects are followed unless they go from https to
 * http. See docs/design/network-and-cache.md.
 *
 * There is no cache here: tiles are cached by TileDiskCache, which also
 * keeps them for offline use.
 *
 * @threadsafe
 */
class NetworkClient
{
public:
  /** Starts the network thread. */
  explicit NetworkClient(const NetworkSettings& settings = {});

  /**
   * Cancels everything still open and stops the network thread. Blocked
   * fetch() calls return Cancelled.
   */
  ~NetworkClient();

  NetworkClient(const NetworkClient&) = delete;
  NetworkClient& operator=(const NetworkClient&) = delete;
  NetworkClient(NetworkClient&&) = delete;
  NetworkClient& operator=(NetworkClient&&) = delete;

  /** Returns the settings. */
  const NetworkSettings& settings() const { return m_settings; }

  /** Starts @p request; never blocks. */
  std::shared_ptr<NetworkTicket> start(const HttpRequest& request);

  /**
   * Makes @p request and waits for the response. When @p cancelled
   * becomes true the request is cancelled and fetch() returns at once
   * with NetworkError::Cancelled.
   */
  HttpResponse fetch(const HttpRequest& request,
                     const std::atomic<bool>& cancelled);

  /**
   * Returns how many requests hold a slot now: on the wire, or waiting
   * to retry.
   */
  int inFlight() const;

  /** Returns how many attempts were sent (retries included). */
  int attemptsSent() const;

  /**
   * Returns the most requests that held a slot at once since the client
   * was created (for tests of the limit).
   */
  int peakInFlight() const;

private:
  class Thread;

  NetworkSettings m_settings;
  std::shared_ptr<NetworkCore> m_core;
  std::unique_ptr<Thread> m_thread;
};

} // namespace ts

#endif // TS_NETWORK_NETWORKCLIENT_H
