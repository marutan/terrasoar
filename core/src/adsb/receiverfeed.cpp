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
#include "ts/adsb/receiverfeed.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTcpSocket>
#include <QTimer>

#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

/** Longest wait before connecting again, milliseconds. */
constexpr int32_t longestRetryMs = 30000;

/** Most bytes kept waiting for a line's end (a stream that isn't text). */
constexpr int32_t longestLine = 4096;

double nowS()
{
  return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

} // namespace

ReceiverFeed::ReceiverFeed(const AircraftSource& source,
                           std::shared_ptr<NetworkClient> network,
                           QObject* parent)
    : QObject(parent), m_source(source), m_network(std::move(network)),
      m_retry(new QTimer(this)), m_decoder([&source]() {
        AdsbDecoderSettings settings;
        settings.receiver = source.receiverPosition;
        return settings;
      }()),
      m_sbs(true)
{
  m_retry->setSingleShot(true);
  connect(m_retry, &QTimer::timeout, this, &ReceiverFeed::connectNow);
}

ReceiverFeed::~ReceiverFeed()
{
  stop();
}

void ReceiverFeed::start()
{
  if (m_running) {
    return;
  }
  m_running = true;
  if (!m_source.recordPath.isEmpty()) {
    m_recording = std::make_unique<QFile>(m_source.recordPath);
    if (!m_recording->open(QIODevice::WriteOnly | QIODevice::Append)) {
      m_recordingError = m_recording->errorString();
      m_recording.reset();
    }
  }
  connectNow();
}

void ReceiverFeed::stop()
{
  m_running = false;
  m_retry->stop();
  if (m_pollTimer != nullptr) {
    m_pollTimer->stop();
  }
  if (m_pollCheck != nullptr) {
    m_pollCheck->stop();
  }
  if (m_ticket) {
    m_ticket->cancel();
    m_ticket.reset();
  }
  if (m_socket != nullptr) {
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
  }
  if (m_recording) {
    m_recording->close();
    m_recording.reset();
  }
  setState(State::Stopped);
}

void ReceiverFeed::setState(State state)
{
  if (state != m_state) {
    m_state = state;
    emit stateChanged();
  }
}

void ReceiverFeed::connectNow()
{
  if (!m_running) {
    return;
  }
  if (m_source.protocol == ReceiverProtocol::Json) {
    if (m_pollTimer == nullptr) {
      m_pollTimer = new QTimer(this);
      connect(m_pollTimer, &QTimer::timeout, this, &ReceiverFeed::poll);
      m_pollCheck = new QTimer(this);
      m_pollCheck->setInterval(50);
      connect(m_pollCheck, &QTimer::timeout, this, &ReceiverFeed::checkPoll);
    }
    m_pollTimer->setInterval(
        static_cast<int32_t>(std::max(m_source.pollS, 1.0) * 1000.0));
    m_pollTimer->start();
    setState(State::Connecting);
    poll();
    return;
  }
  if (m_socket != nullptr) {
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
  }
  m_socket = new QTcpSocket(this);
  connect(m_socket, &QTcpSocket::connected, this,
          &ReceiverFeed::socketConnected);
  connect(m_socket, &QTcpSocket::readyRead, this, &ReceiverFeed::readSocket);
  connect(m_socket, &QTcpSocket::disconnected, this, &ReceiverFeed::socketLost);
  connect(m_socket, &QAbstractSocket::errorOccurred, this,
          &ReceiverFeed::socketLost);
  m_pending.clear();
  m_beast = BeastParser();
  m_decoder.clear();
  setState(State::Connecting);
  m_socket->connectToHost(m_source.host, m_source.port);
}

void ReceiverFeed::socketConnected()
{
  ++m_connections;
  m_retryMs = 1000;
  m_error.clear();
  setState(State::Connected);
}

void ReceiverFeed::socketLost()
{
  if (m_socket == nullptr || !m_running) {
    return;
  }
  m_error = m_socket->errorString();
  m_socket->disconnect(this);
  m_socket->abort();
  m_socket->deleteLater();
  m_socket = nullptr;
  scheduleRetry();
}

void ReceiverFeed::scheduleRetry()
{
  setState(State::Reconnecting);
  m_retry->start(m_retryMs);
  m_retryMs = std::min(m_retryMs * 2, longestRetryMs);
}

void ReceiverFeed::readSocket()
{
  if (m_socket == nullptr) {
    return;
  }
  const QByteArray data = m_socket->readAll();
  const double now = nowS();
  const std::size_t before = m_reports.size();
  if (m_source.protocol == ReceiverProtocol::Beast) {
    for (const TimedMessage& message : m_beast.feed(data)) {
      handleMessage(message.bytes, now);
    }
  } else {
    m_pending.append(data);
    // int32_t: QByteArray takes int on Qt 5.
    int32_t start = 0;
    int32_t end = 0;
    while ((end = static_cast<int32_t>(m_pending.indexOf('\n', start))) >= 0) {
      handleLine(m_pending.mid(start, end - start).trimmed(), now);
      start = end + 1;
    }
    m_pending.remove(0, start);
    if (m_pending.size() > longestLine) {
      m_pending.clear();
    }
  }
  if (m_reports.size() > before) {
    emit reportsReady();
  }
}

void ReceiverFeed::handleLine(const QByteArray& line, double now)
{
  if (line.isEmpty()) {
    return;
  }
  ++m_messages;
  if (m_source.protocol == ReceiverProtocol::Sbs) {
    record(line + '\n');
    const std::optional<AircraftReport> report = m_sbs.parse(line);
    if (report) {
      // Timed as it came: the receiver's clock may not be this one's.
      AircraftReport arrived = *report;
      arrived.timeS = now;
      add(arrived);
    }
    return;
  }
  const std::optional<TimedMessage> message = parseRawLine(line);
  if (message) {
    handleMessage(message->bytes, now);
  }
}

void ReceiverFeed::handleMessage(const QByteArray& bytes, double now)
{
  if (m_source.protocol == ReceiverProtocol::Beast) {
    ++m_messages;
  }
  record(QByteArray::number(now, 'f', 3) + ',' + bytes.toHex().toUpper() +
         '\n');
  const std::optional<AircraftReport> report = m_decoder.decode(bytes, now);
  if (report) {
    add(*report);
  }
}

void ReceiverFeed::poll()
{
  if (!m_running || m_ticket || !m_network) {
    if (!m_network) {
      m_error = QStringLiteral("no network");
      scheduleRetry();
    }
    return;
  }
  HttpRequest request;
  request.url = m_source.url;
  m_ticket = m_network->start(request);
  m_pollCheck->start();
}

void ReceiverFeed::checkPoll()
{
  if (!m_ticket || !m_ticket->isFinished()) {
    return;
  }
  m_pollCheck->stop();
  const HttpResponse response = m_ticket->response();
  m_ticket.reset();
  if (!response.isSuccess()) {
    m_error = response.errorString.isEmpty()
                  ? QStringLiteral("HTTP %1").arg(response.status)
                  : response.errorString;
    setState(State::Reconnecting);
    return;
  }
  if (m_state != State::Connected) {
    ++m_connections;
  }
  m_error.clear();
  setState(State::Connected);
  handleSnapshot(response.body);
}

void ReceiverFeed::handleSnapshot(const QByteArray& json)
{
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError) {
    m_error = error.errorString();
    return;
  }
  ++m_messages;
  record(document.toJson(QJsonDocument::Compact) + '\n');
  const std::size_t before = m_reports.size();
  readReadsbSnapshot(document.object(),
                     [this](const AircraftReport& report) { add(report); });
  if (m_reports.size() > before) {
    emit reportsReady();
  }
}

void ReceiverFeed::record(const QByteArray& data)
{
  if (m_recording) {
    m_recording->write(data);
  }
}

void ReceiverFeed::add(const AircraftReport& report)
{
  m_reports.push_back(report);
}

std::vector<AircraftReport> ReceiverFeed::takeReports()
{
  if (m_recording) {
    m_recording->flush();
  }
  std::vector<AircraftReport> reports;
  reports.swap(m_reports);
  return reports;
}

} // namespace ts
