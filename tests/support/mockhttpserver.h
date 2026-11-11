// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A local HTTP server for network tests: no internet needed.
 */
#ifndef TS_TESTS_MOCKHTTPSERVER_H
#define TS_TESTS_MOCKHTTPSERVER_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace ts::test
{

/** A request the server received. */
struct MockHttpRequest {
  /** "GET". */
  QByteArray method;
  /** The request target as sent: "/path?query", or an absolute URL when
   *  the server is used as a proxy. */
  QByteArray target;
  /** The target as a URL (relative unless sent through a proxy). */
  QUrl url;
  /** Headers, names in lower case. */
  std::map<QByteArray, QByteArray> headers;

  /** Returns header @p lowerCaseName, or an empty array. */
  QByteArray header(const QByteArray& lowerCaseName) const
  {
    const std::map<QByteArray, QByteArray>::const_iterator it =
        headers.find(lowerCaseName);
    return it == headers.end() ? QByteArray() : it->second;
  }
};

/** What to answer. */
struct MockHttpResponse {
  /** Status code. */
  int status = 200;
  /** Headers besides Content-Length and Connection. */
  std::vector<std::pair<QByteArray, QByteArray>> headers;
  /** Body. */
  QByteArray body;
  /** Wait this long before answering. */
  int delayMs = 0;
  /** Close the connection without answering. */
  bool hangUp = false;
  /** Never answer (until the client gives up). */
  bool stall = false;
};

/**
 * An HTTP/1.1 server on 127.0.0.1 and a port of its own, running on its
 * own thread so that a test may block in a request to it. Each request
 * goes to a handler (called on the server's thread; it must only touch
 * thread-safe data) and the connection closes after the answer.
 *
 * It records every request, how many were waiting for their answer at
 * once, and how many clients hung up before the answer came (a
 * cancelled request).
 *
 * @threadsafe
 */
class MockHttpServer
{
public:
  /** What answers requests. */
  using Handler = std::function<MockHttpResponse(const MockHttpRequest&)>;

  /** Starts listening; @p handler answers. */
  explicit MockHttpServer(Handler handler = {}) : m_handler(std::move(handler))
  {
    m_thread = std::make_unique<Thread>(*this);
    m_thread->start();
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this]() { return m_listening; });
  }

  /** Stops the server. */
  ~MockHttpServer()
  {
    m_thread->quit();
    m_thread->wait();
  }

  MockHttpServer(const MockHttpServer&) = delete;
  MockHttpServer& operator=(const MockHttpServer&) = delete;
  MockHttpServer(MockHttpServer&&) = delete;
  MockHttpServer& operator=(MockHttpServer&&) = delete;

  /** Replaces the handler. */
  void setHandler(Handler handler)
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_handler = std::move(handler);
  }

  /** Returns the port. */
  quint16 port() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_port;
  }

  /** Returns http://127.0.0.1:port plus @p pathAndQuery. */
  QUrl url(const QString& pathAndQuery = QStringLiteral("/")) const
  {
    return QUrl(
        QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(pathAndQuery));
  }

  /** Returns the requests received so far. */
  std::vector<MockHttpRequest> requests() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
  }

  /** Returns how many requests arrived. */
  int requestCount() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_requests.size());
  }

  /** Returns how many clients hung up before their answer was sent. */
  int abandonedCount() const { return m_abandoned.load(); }

  /** Returns the most requests that waited for an answer at once. */
  int peakWaiting() const { return m_peakWaiting.load(); }

  /** Forgets the requests and counters. */
  void resetLog()
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.clear();
    m_abandoned.store(0);
    m_peakWaiting.store(0);
  }

private:
  /** Runs the QTcpServer; every socket lives on this thread. */
  class Thread : public QThread
  {
  public:
    explicit Thread(MockHttpServer& owner) : m_owner(owner) {}

  protected:
    void run() override
    {
      QTcpServer server;
      server.listen(QHostAddress::LocalHost, 0);
      QObject::connect(
          &server, &QTcpServer::newConnection, &server, [this, &server]() {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
              accept(socket);
            }
          });
      {
        const std::lock_guard<std::mutex> lock(m_owner.m_mutex);
        m_owner.m_port = server.serverPort();
        m_owner.m_listening = true;
      }
      m_owner.m_condition.notify_all();
      exec();
    }

  private:
    void accept(QTcpSocket* socket)
    {
      std::shared_ptr<QByteArray> buffer = std::make_shared<QByteArray>();
      QObject::connect(socket, &QTcpSocket::disconnected, socket,
                       &QObject::deleteLater);
      QObject::connect(
          socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            const int end = static_cast<int>(buffer->indexOf("\r\n\r\n"));
            if (end >= 0 && !socket->property("tsSeen").toBool()) {
              socket->setProperty("tsSeen", true);
              handle(socket, buffer->left(end));
            }
          });
    }

    void handle(QTcpSocket* socket, const QByteArray& head)
    {
      MockHttpRequest request;
      const QList<QByteArray> lines = head.split('\n');
      const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
      request.method = requestLine.value(0);
      request.target = requestLine.value(1);
      request.url = QUrl::fromEncoded(request.target);
      for (int i = 1; i < lines.size(); ++i) {
        const QByteArray& line = lines.at(i);
        const int colon = static_cast<int>(line.indexOf(':'));
        if (colon > 0) {
          request.headers[line.left(colon).trimmed().toLower()] =
              line.mid(colon + 1).trimmed();
        }
      }
      Handler handler;
      {
        const std::lock_guard<std::mutex> lock(m_owner.m_mutex);
        m_owner.m_requests.push_back(request);
        handler = m_owner.m_handler;
      }
      const MockHttpResponse response =
          handler ? handler(request)
                  : MockHttpResponse{404, {}, {}, 0, false, false};
      const int waiting = m_owner.m_waiting.fetch_add(1) + 1;
      int peak = m_owner.m_peakWaiting.load();
      while (waiting > peak &&
             !m_owner.m_peakWaiting.compare_exchange_weak(peak, waiting)) {
      }
      QPointer<QTcpSocket> guarded(socket);
      std::shared_ptr<bool> answered = std::make_shared<bool>(false);
      QObject::connect(socket, &QTcpSocket::disconnected, socket,
                       [this, answered]() {
                         if (!*answered) {
                           *answered = true;
                           m_owner.m_waiting.fetch_sub(1);
                           m_owner.m_abandoned.fetch_add(1);
                         }
                       });
      if (response.stall) {
        return;
      }
      QTimer::singleShot(
          std::max(response.delayMs, 0), socket,
          [this, guarded, answered, response]() {
            if (!guarded || *answered ||
                guarded->state() != QAbstractSocket::ConnectedState) {
              return;
            }
            *answered = true;
            m_owner.m_waiting.fetch_sub(1);
            if (response.hangUp) {
              guarded->abort();
              return;
            }
            QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) +
                             " Mock\r\nContent-Length: " +
                             QByteArray::number(response.body.size()) +
                             "\r\nConnection: close\r\n";
            for (const std::pair<QByteArray, QByteArray>& header :
                 response.headers) {
              out += header.first + ": " + header.second + "\r\n";
            }
            out += "\r\n";
            out += response.body;
            guarded->write(out);
            guarded->disconnectFromHost();
          });
    }

    MockHttpServer& m_owner;
  };

  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  Handler m_handler;
  bool m_listening = false;
  quint16 m_port = 0;
  std::vector<MockHttpRequest> m_requests;
  std::atomic<int> m_waiting{0};
  std::atomic<int> m_peakWaiting{0};
  std::atomic<int> m_abandoned{0};
  std::unique_ptr<Thread> m_thread;
};

/** Returns a response of @p status with @p body and @p contentType. */
inline MockHttpResponse
mockResponse(int status, const QByteArray& body,
             const QByteArray& contentType = QByteArrayLiteral("text/plain"))
{
  MockHttpResponse response;
  response.status = status;
  response.body = body;
  response.headers.emplace_back(QByteArrayLiteral("Content-Type"), contentType);
  return response;
}

} // namespace ts::test

#endif // TS_TESTS_MOCKHTTPSERVER_H
