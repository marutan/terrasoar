// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/network/networkclient.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>

namespace ts
{

namespace
{

/** How often fetch() looks at its cancellation flag. */
constexpr std::chrono::milliseconds cancelPollInterval{20};

HttpResponse cancelledResponse()
{
  HttpResponse response;
  response.error = NetworkError::Cancelled;
  response.errorString = QStringLiteral("cancelled");
  return response;
}

NetworkError classify(QNetworkReply::NetworkError error)
{
  switch (error) {
  case QNetworkReply::NoError:
    return NetworkError::None;
  case QNetworkReply::TimeoutError:
  case QNetworkReply::ProxyTimeoutError:
    return NetworkError::Timeout;
  case QNetworkReply::ConnectionRefusedError:
  case QNetworkReply::RemoteHostClosedError:
  case QNetworkReply::HostNotFoundError:
  case QNetworkReply::TemporaryNetworkFailureError:
  case QNetworkReply::NetworkSessionFailedError:
  case QNetworkReply::UnknownNetworkError:
  case QNetworkReply::ProxyConnectionRefusedError:
  case QNetworkReply::ProxyConnectionClosedError:
  case QNetworkReply::ProxyNotFoundError:
    return NetworkError::ConnectionFailed;
  default:
    return NetworkError::Other;
  }
}

/**
 * True if @p reply carries the server's answer, whatever its status. A
 * redirect not followed to its end is not: the connection closed on the
 * way (a connection failure, retried), or it led somewhere less safe.
 */
bool isAnswer(const QNetworkReply& reply)
{
  const int status =
      reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const bool cutShort =
      status >= 300 && status < 400 && reply.error() != QNetworkReply::NoError;
  return status > 0 && !cutShort;
}

bool isRetryable(const HttpResponse& response)
{
  if (response.error == NetworkError::Timeout ||
      response.error == NetworkError::ConnectionFailed) {
    return true;
  }
  return response.error == NetworkError::None &&
         (response.status == 408 || response.status == 429 ||
          response.status >= 500);
}

/** Seconds of a Retry-After header (delta-seconds form only), or -1. */
long long retryAfterS(const HttpResponse& response)
{
  bool ok = false;
  const long long seconds =
      response.header("retry-after").trimmed().toLongLong(&ok);
  return ok && seconds >= 0 ? seconds : -1;
}

} // namespace

class NetworkDispatcher;

/** The state shared by a client, its tickets and its network thread. */
class NetworkCore
{
public:
  explicit NetworkCore(NetworkSettings networkSettings)
      : settings(std::move(networkSettings))
  {
  }

  /** Asks the network thread to look at the queue and cancellations. */
  void wake();

  const NetworkSettings settings;
  std::mutex mutex;
  std::condition_variable ready;
  /** Requests not yet started, oldest first. Under mutex. */
  std::deque<std::shared_ptr<NetworkTicket>> pending;
  /** The network thread's dispatcher, once it runs. Under mutex. */
  NetworkDispatcher* dispatcher = nullptr;
  /** Under mutex. */
  bool stopping = false;
  std::atomic<bool> wakePending{false};
  std::atomic<int> inFlight{0};
  std::atomic<int> attemptsSent{0};
  std::atomic<int> peakInFlight{0};
};

/**
 * Lives on the network thread: starts requests, collects their replies,
 * retries and cancels.
 */
class NetworkDispatcher : public QObject
{
  Q_OBJECT

public:
  NetworkDispatcher(NetworkCore& core, QNetworkAccessManager& manager)
      : m_core(core), m_manager(manager)
  {
  }

  /** Starts what the limit allows and drops what was cancelled. */
  Q_INVOKABLE void drain();

  /** Finishes everything open as cancelled (the client is going away). */
  void abortAll();

private:
  struct Active {
    std::shared_ptr<NetworkTicket> ticket;
    int attempt = 1;
    bool tooLarge = false;
    bool cancelled = false;
  };

  void startAttempt(const std::shared_ptr<NetworkTicket>& ticket, int attempt);
  void onFinished(QNetworkReply* reply);
  void checkSize(QNetworkReply* reply, qint64 received, qint64 total);
  void complete(const std::shared_ptr<NetworkTicket>& ticket,
                HttpResponse response);

  NetworkCore& m_core;
  QNetworkAccessManager& m_manager;
  std::unordered_map<QNetworkReply*, Active> m_active;
  /** Tickets waiting to retry, holding their slots. */
  std::unordered_set<std::shared_ptr<NetworkTicket>> m_retrying;
};

void NetworkCore::wake()
{
  if (wakePending.exchange(true)) {
    return; // a wake-up is on its way already
  }
  const std::lock_guard<std::mutex> lock(mutex);
  if (dispatcher != nullptr) {
    // By name, not with a functor: the queued call is then made entirely
    // inside Qt. A functor's slot object would be written here and read on
    // the network thread, handed over by Qt's event queue, whose locking
    // ThreadSanitizer can't see (Qt isn't instrumented), so every wake-up
    // would be reported as a race.
    QMetaObject::invokeMethod(dispatcher, "drain", Qt::QueuedConnection);
  }
}

void NetworkDispatcher::drain()
{
  m_core.wakePending.store(false);
  // Cancelled requests on the wire: drop their connections. abort()
  // emits finished() at once, which edits m_active, so collect first.
  std::vector<QNetworkReply*> toAbort;
  for (std::pair<QNetworkReply* const, Active>& active : m_active) {
    if (active.second.ticket->isCancelled() && !active.second.cancelled) {
      active.second.cancelled = true;
      toAbort.push_back(active.first);
    }
  }
  for (QNetworkReply* reply : toAbort) {
    reply->abort();
  }
  std::vector<std::shared_ptr<NetworkTicket>> cancelledRetries;
  for (const std::shared_ptr<NetworkTicket>& ticket : m_retrying) {
    if (ticket->isCancelled()) {
      cancelledRetries.push_back(ticket);
    }
  }
  for (const std::shared_ptr<NetworkTicket>& ticket : cancelledRetries) {
    m_retrying.erase(ticket);
    complete(ticket, cancelledResponse());
  }
  // Waiting requests that were cancelled, or all of them when stopping,
  // finish now; they never started.
  std::vector<std::shared_ptr<NetworkTicket>> dropped;
  {
    const std::lock_guard<std::mutex> lock(m_core.mutex);
    std::deque<std::shared_ptr<NetworkTicket>> kept;
    for (const std::shared_ptr<NetworkTicket>& ticket : m_core.pending) {
      if (m_core.stopping || ticket->isCancelled()) {
        dropped.push_back(ticket);
      } else {
        kept.push_back(ticket);
      }
    }
    m_core.pending.swap(kept);
  }
  for (const std::shared_ptr<NetworkTicket>& ticket : dropped) {
    ticket->finish(cancelledResponse());
  }
  for (;;) {
    std::shared_ptr<NetworkTicket> next;
    {
      const std::lock_guard<std::mutex> lock(m_core.mutex);
      if (m_core.stopping || m_core.pending.empty() ||
          m_core.inFlight.load() >= m_core.settings.maximumInFlight) {
        return;
      }
      next = m_core.pending.front();
      m_core.pending.pop_front();
    }
    const int now = m_core.inFlight.fetch_add(1) + 1;
    int peak = m_core.peakInFlight.load();
    while (now > peak &&
           !m_core.peakInFlight.compare_exchange_weak(peak, now)) {
    }
    startAttempt(next, 1);
  }
}

void NetworkDispatcher::abortAll()
{
  std::vector<QNetworkReply*> replies;
  for (std::pair<QNetworkReply* const, Active>& active : m_active) {
    active.second.cancelled = true;
    replies.push_back(active.first);
  }
  for (QNetworkReply* reply : replies) {
    reply->abort();
  }
  std::unordered_set<std::shared_ptr<NetworkTicket>> retrying;
  retrying.swap(m_retrying);
  for (const std::shared_ptr<NetworkTicket>& ticket : retrying) {
    complete(ticket, cancelledResponse());
  }
}

void NetworkDispatcher::startAttempt(
    const std::shared_ptr<NetworkTicket>& ticket, int attempt)
{
  const HttpRequest& request = ticket->request();
  QNetworkRequest networkRequest(request.url);
  networkRequest.setRawHeader("User-Agent", m_core.settings.userAgent);
  for (const std::pair<QByteArray, QByteArray>& header : request.headers) {
    networkRequest.setRawHeader(header.first, header.second);
  }
  networkRequest.setTransferTimeout(
      static_cast<int>(m_core.settings.timeout.count()));
  networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                              QNetworkRequest::AlwaysNetwork);
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);
  // HTTP/1.1 only. With Qt 6.4, a server closing an HTTP/2 connection
  // while a redirect is followed (GitHub's release downloads) left the
  // next request on it hanging past its transfer timeout. Qt opens up to
  // six connections per host, as many as we keep in flight, so HTTP/2's
  // multiplexing would gain little.
  networkRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  QNetworkReply* reply = m_manager.get(networkRequest);
  m_core.attemptsSent.fetch_add(1);
  m_active[reply] = Active{ticket, attempt, false, false};
  connect(reply, &QNetworkReply::downloadProgress, this,
          [this, reply](qint64 received, qint64 total) {
            checkSize(reply, received, total);
          });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply]() { onFinished(reply); });
}

void NetworkDispatcher::checkSize(QNetworkReply* reply, qint64 received,
                                  qint64 total)
{
  const qint64 limit = m_core.settings.maximumResponseBytes;
  if (received <= limit && total <= limit) {
    return;
  }
  const std::unordered_map<QNetworkReply*, Active>::iterator it =
      m_active.find(reply);
  if (it != m_active.end() && !it->second.tooLarge) {
    it->second.tooLarge = true;
    reply->abort();
  }
}

void NetworkDispatcher::onFinished(QNetworkReply* reply)
{
  const std::unordered_map<QNetworkReply*, Active>::iterator it =
      m_active.find(reply);
  reply->deleteLater();
  if (it == m_active.end()) {
    return;
  }
  const Active active = it->second;
  m_active.erase(it);

  HttpResponse response;
  response.attempts = active.attempt;
  response.finalUrl = reply->url();
  const QVariant status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  if (active.cancelled || active.ticket->isCancelled()) {
    complete(active.ticket, cancelledResponse());
    return;
  }
  if (active.tooLarge) {
    response.error = NetworkError::TooLarge;
    response.errorString = QStringLiteral("response larger than %1 bytes")
                               .arg(m_core.settings.maximumResponseBytes);
  } else if (isAnswer(*reply)) {
    // A response, whatever its status.
    response.error = NetworkError::None;
    response.status = status.toInt();
    response.body = reply->readAll();
    for (const QNetworkReply::RawHeaderPair& header : reply->rawHeaderPairs()) {
      response.headers.emplace_back(header.first.toLower(), header.second);
    }
    if (response.status >= 400) {
      response.errorString =
          QStringLiteral("HTTP %1 %2")
              .arg(response.status)
              .arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute)
                       .toString());
    }
  } else if (reply->error() == QNetworkReply::OperationCanceledError) {
    // Not cancelled by us and not too large: the transfer timeout.
    response.error = NetworkError::Timeout;
    response.errorString = QStringLiteral("no data for %1 ms")
                               .arg(m_core.settings.timeout.count());
  } else {
    response.error = classify(reply->error());
    response.errorString = reply->errorString();
  }

  bool stopping = false;
  {
    const std::lock_guard<std::mutex> lock(m_core.mutex);
    stopping = m_core.stopping;
  }
  if (isRetryable(response) && active.attempt < m_core.settings.attempts &&
      !stopping) {
    // Wait, holding the slot, then try again.
    long long delayMs = m_core.settings.retryDelay.count();
    for (int i = 1; i < active.attempt; ++i) {
      delayMs *= 2;
    }
    const long long askedS = retryAfterS(response);
    if (askedS >= 0) {
      delayMs = std::max(delayMs, askedS * 1000);
    }
    delayMs =
        std::min<long long>(delayMs, m_core.settings.maximumRetryDelay.count());
    const std::shared_ptr<NetworkTicket> ticket = active.ticket;
    const int next = active.attempt + 1;
    m_retrying.insert(ticket);
    QTimer::singleShot(static_cast<int>(delayMs), this, [this, ticket, next]() {
      if (m_retrying.erase(ticket) == 0) {
        return; // cancelled meanwhile
      }
      if (ticket->isCancelled()) {
        complete(ticket, cancelledResponse());
        return;
      }
      startAttempt(ticket, next);
    });
    return;
  }
  complete(active.ticket, response);
}

void NetworkDispatcher::complete(const std::shared_ptr<NetworkTicket>& ticket,
                                 HttpResponse response)
{
  ticket->finish(std::move(response));
  m_core.inFlight.fetch_sub(1);
  drain();
}

/** Runs the QNetworkAccessManager and dispatcher, created on it. */
class NetworkClient::Thread : public QThread
{
  Q_OBJECT

public:
  explicit Thread(std::shared_ptr<NetworkCore> core, QObject* parent = nullptr)
      : QThread(parent), m_core(std::move(core))
  {
    setObjectName(QStringLiteral("Terra Soar network"));
  }

protected:
  void run() override
  {
    QNetworkAccessManager manager;
    const std::optional<QNetworkProxy>& proxy = m_core->settings.proxy;
    if (proxy) {
      manager.setProxy(*proxy);
    } else {
      static std::once_flag systemProxy;
      std::call_once(systemProxy, []() {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
      });
    }
    NetworkDispatcher dispatcher(*m_core, manager);
    {
      const std::lock_guard<std::mutex> lock(m_core->mutex);
      m_core->dispatcher = &dispatcher;
    }
    m_core->ready.notify_all();
    dispatcher.drain();
    exec();
    {
      const std::lock_guard<std::mutex> lock(m_core->mutex);
      m_core->dispatcher = nullptr;
    }
    dispatcher.abortAll();
  }

private:
  std::shared_ptr<NetworkCore> m_core;
};

// --- HttpResponse
// -------------------------------------------------------------

QByteArray HttpResponse::header(const QByteArray& lowerCaseName) const
{
  for (const std::pair<QByteArray, QByteArray>& entry : headers) {
    if (entry.first == lowerCaseName) {
      return entry.second;
    }
  }
  return QByteArray();
}

// --- NetworkTicket
// ------------------------------------------------------------

NetworkTicket::NetworkTicket(HttpRequest request,
                             std::weak_ptr<NetworkCore> core)
    : m_request(std::move(request)), m_core(std::move(core))
{
}

bool NetworkTicket::isFinished() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_finished;
}

HttpResponse NetworkTicket::response() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_response;
}

bool NetworkTicket::waitFor(std::chrono::milliseconds timeout) const
{
  std::unique_lock<std::mutex> lock(m_mutex);
  return m_finishedCondition.wait_for(lock, timeout,
                                      [this]() { return m_finished; });
}

void NetworkTicket::cancel()
{
  if (m_cancelled.exchange(true)) {
    return;
  }
  if (const std::shared_ptr<NetworkCore> core = m_core.lock()) {
    core->wake();
  }
}

void NetworkTicket::finish(HttpResponse response)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_finished) {
      return;
    }
    m_finished = true;
    m_response = std::move(response);
  }
  m_finishedCondition.notify_all();
}

// --- NetworkClient
// ------------------------------------------------------------

NetworkClient::NetworkClient(const NetworkSettings& settings)
    : m_settings(settings), m_core(std::make_shared<NetworkCore>(settings)),
      m_thread(std::make_unique<Thread>(m_core))
{
  m_thread->start();
  std::unique_lock<std::mutex> lock(m_core->mutex);
  m_core->ready.wait(lock, [this]() { return m_core->dispatcher != nullptr; });
}

NetworkClient::~NetworkClient()
{
  {
    const std::lock_guard<std::mutex> lock(m_core->mutex);
    m_core->stopping = true;
  }
  m_thread->quit();
  m_thread->wait();
  // Whatever never started.
  std::deque<std::shared_ptr<NetworkTicket>> pending;
  {
    const std::lock_guard<std::mutex> lock(m_core->mutex);
    pending.swap(m_core->pending);
  }
  for (const std::shared_ptr<NetworkTicket>& ticket : pending) {
    ticket->finish(cancelledResponse());
  }
}

std::shared_ptr<NetworkTicket> NetworkClient::start(const HttpRequest& request)
{
  std::shared_ptr<NetworkTicket> ticket =
      std::make_shared<NetworkTicket>(request, m_core);
  {
    const std::lock_guard<std::mutex> lock(m_core->mutex);
    if (m_core->stopping) {
      ticket->finish(cancelledResponse());
      return ticket;
    }
    m_core->pending.push_back(ticket);
  }
  m_core->wake();
  return ticket;
}

HttpResponse NetworkClient::fetch(const HttpRequest& request,
                                  const std::atomic<bool>& cancelled)
{
  const std::shared_ptr<NetworkTicket> ticket = start(request);
  while (!ticket->waitFor(cancelPollInterval)) {
    if (cancelled.load()) {
      ticket->cancel();
      return cancelledResponse();
    }
  }
  return ticket->response();
}

int NetworkClient::inFlight() const
{
  return m_core->inFlight.load();
}

int NetworkClient::attemptsSent() const
{
  return m_core->attemptsSent.load();
}

int NetworkClient::peakInFlight() const
{
  return m_core->peakInFlight.load();
}

} // namespace ts

#include "networkclient.moc"
