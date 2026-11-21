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
#include "ts/ais/aisfeed.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

/** Longest wait before connecting again, milliseconds. */
constexpr int32_t longestRetryMs = 30000;

/** Longest wait between failed fetches, milliseconds. */
constexpr int32_t longestPollMs = 300000;

/** Most bytes kept waiting for a line's end. */
constexpr int32_t longestLine = 4096;

double nowS()
{
  return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

std::optional<double> numberIn(const QJsonObject& object, const char* name)
{
  const QJsonValue value = object.value(QLatin1String(name));
  return value.isDouble() ? std::optional<double>(value.toDouble())
                          : std::nullopt;
}

/** A Digitraffic location feature as a report, if it is one. */
std::optional<VesselReport> locationReport(const QJsonObject& feature)
{
  const QJsonObject properties =
      feature.value(QLatin1String("properties")).toObject();
  const QJsonArray coordinates = feature.value(QLatin1String("geometry"))
                                     .toObject()
                                     .value(QLatin1String("coordinates"))
                                     .toArray();
  const std::optional<double> mmsi = numberIn(properties, "mmsi");
  const std::optional<double> timeMs =
      numberIn(properties, "timestampExternal");
  if (!mmsi || !timeMs || coordinates.size() < 2) {
    return std::nullopt;
  }
  VesselReport r;
  r.mmsi = static_cast<uint32_t>(*mmsi);
  r.timeS = *timeMs / 1000.0;
  const double lon = coordinates.at(0).toDouble(181.0);
  const double lat = coordinates.at(1).toDouble(91.0);
  if (std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0) {
    r.latDeg = lat;
    r.lonDeg = lon;
  }
  const std::optional<double> sog = numberIn(properties, "sog");
  if (sog && *sog < 102.2) {
    r.speedKn = sog;
  }
  const std::optional<double> cog = numberIn(properties, "cog");
  if (cog && *cog < 360.0) {
    r.courseDeg = cog;
  }
  const std::optional<double> heading = numberIn(properties, "heading");
  if (heading && *heading < 360.0) {
    r.headingDeg = heading;
  }
  const std::optional<double> rot = numberIn(properties, "rot");
  if (rot && *rot > -128.0) {
    const double root = *rot / 4.733;
    r.turnDegMin = (*rot < 0.0 ? -1.0 : 1.0) * root * root;
  }
  const std::optional<double> status = numberIn(properties, "navStat");
  if (status && *status >= 0.0 && *status <= 15.0) {
    r.status = static_cast<AisNavStatus>(static_cast<int32_t>(*status));
  }
  return r;
}

/** A Digitraffic vessel's metadata as a report, if it is one. */
std::optional<VesselReport> vesselMetadata(const QJsonObject& v)
{
  const std::optional<double> mmsi = numberIn(v, "mmsi");
  const std::optional<double> timeMs = numberIn(v, "timestamp");
  if (!mmsi || !timeMs) {
    return std::nullopt;
  }
  VesselReport r;
  r.mmsi = static_cast<uint32_t>(*mmsi);
  r.timeS = *timeMs / 1000.0;
  const QString name = v.value(QLatin1String("name")).toString().trimmed();
  if (!name.isEmpty()) {
    r.name = name;
  }
  const QString callsign =
      v.value(QLatin1String("callSign")).toString().trimmed();
  if (!callsign.isEmpty()) {
    r.callsign = callsign;
  }
  const std::optional<double> imo = numberIn(v, "imo");
  if (imo && *imo > 0.0) {
    r.imo = static_cast<uint32_t>(*imo);
  }
  const std::optional<double> type = numberIn(v, "shipType");
  if (type && *type > 0.0 && *type < 100.0) {
    r.shipType = static_cast<int32_t>(*type);
  }
  const std::optional<double> draught = numberIn(v, "draught");
  if (draught && *draught > 0.0) {
    r.draughtM = *draught / 10.0;
  }
  const QString destination =
      v.value(QLatin1String("destination")).toString().trimmed();
  if (!destination.isEmpty()) {
    r.destination = destination;
  }
  // The ETA as AIS packs it: month, day, hour, minute in 20 bits.
  const uint32_t eta = static_cast<uint32_t>(numberIn(v, "eta").value_or(0.0));
  AisEta e;
  e.month = static_cast<int32_t>((eta >> 16U) & 0xFU);
  e.day = static_cast<int32_t>((eta >> 11U) & 0x1FU);
  e.hour = static_cast<int32_t>((eta >> 6U) & 0x1FU);
  e.minute = static_cast<int32_t>(eta & 0x3FU);
  if (e.month != 0 && e.day != 0 && e.hour < 24 && e.minute < 60) {
    r.eta = e;
  }
  AisDimensions d;
  d.toBow = static_cast<int32_t>(numberIn(v, "referencePointA").value_or(0.0));
  d.toStern =
      static_cast<int32_t>(numberIn(v, "referencePointB").value_or(0.0));
  d.toPort = static_cast<int32_t>(numberIn(v, "referencePointC").value_or(0.0));
  d.toStarboard =
      static_cast<int32_t>(numberIn(v, "referencePointD").value_or(0.0));
  if (d.isKnown()) {
    r.dimensions = d;
  }
  r.classB = false;
  return r;
}

} // namespace

std::optional<std::vector<VesselReport>>
readDigitrafficLocations(const QByteArray& json)
{
  const QJsonDocument document = QJsonDocument::fromJson(json);
  const QJsonObject root = document.object();
  if (root.value(QLatin1String("type")).toString() !=
      QLatin1String("FeatureCollection")) {
    return std::nullopt;
  }
  const QJsonArray features = root.value(QLatin1String("features")).toArray();
  std::vector<VesselReport> reports;
  reports.reserve(static_cast<std::size_t>(features.size()));
  // An index loop: QJsonArray's iterator differs between Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < features.size(); ++i) {
    const std::optional<VesselReport> r =
        locationReport(features.at(i).toObject());
    if (r) {
      reports.push_back(*r);
    }
  }
  return reports;
}

std::optional<std::vector<VesselReport>>
readDigitrafficVessels(const QByteArray& json)
{
  const QJsonDocument document = QJsonDocument::fromJson(json);
  if (!document.isArray()) {
    return std::nullopt;
  }
  const QJsonArray vessels = document.array();
  std::vector<VesselReport> reports;
  reports.reserve(static_cast<std::size_t>(vessels.size()));
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < vessels.size(); ++i) {
    const std::optional<VesselReport> r =
        vesselMetadata(vessels.at(i).toObject());
    if (r) {
      reports.push_back(*r);
    }
  }
  return reports;
}

AisFeed::AisFeed(VesselSource source, std::shared_ptr<NetworkClient> network,
                 QObject* parent)
    : QObject(parent), m_source(std::move(source)),
      m_network(std::move(network)), m_retry(new QTimer(this))
{
  m_retry->setSingleShot(true);
  connect(m_retry, &QTimer::timeout, this, &AisFeed::connectNow);
}

AisFeed::~AisFeed()
{
  stop();
}

void AisFeed::start()
{
  if (m_running) {
    return;
  }
  m_running = true;
  if (!m_source.recordPath.isEmpty()) {
    m_recording = std::make_unique<QFile>(m_source.recordPath);
    if (!m_recording->open(QIODevice::WriteOnly | QIODevice::Append)) {
      m_error = m_recording->errorString();
      m_recording.reset();
    }
  }
  connectNow();
}

void AisFeed::stop()
{
  m_running = false;
  m_retry->stop();
  for (QTimer* timer : {m_poll, m_pollCheck}) {
    if (timer != nullptr) {
      timer->stop();
    }
  }
  if (m_ticket) {
    m_ticket->cancel();
    m_ticket.reset();
  }
  if (m_tcp != nullptr) {
    m_tcp->disconnect(this);
    m_tcp->abort();
    m_tcp->deleteLater();
    m_tcp = nullptr;
  }
  if (m_udp != nullptr) {
    m_udp->disconnect(this);
    m_udp->deleteLater();
    m_udp = nullptr;
  }
  m_recording.reset();
  setState(State::Stopped);
}

void AisFeed::setState(State state)
{
  if (state != m_state) {
    m_state = state;
    emit stateChanged();
  }
}

void AisFeed::connectNow()
{
  if (!m_running) {
    return;
  }
  if (m_source.origin == VesselSource::Origin::Feed) {
    const QUrl url = m_source.feedUrl();
    if (url.scheme() == QLatin1String("tcp")) {
      connectTcp(url.host(), static_cast<uint16_t>(url.port(5631)));
      return;
    }
    if (m_poll == nullptr) {
      m_poll = new QTimer(this);
      m_poll->setSingleShot(true);
      connect(m_poll, &QTimer::timeout, this, &AisFeed::fetch);
      m_pollCheck = new QTimer(this);
      m_pollCheck->setInterval(50);
      connect(m_pollCheck, &QTimer::timeout, this, &AisFeed::checkFetch);
    }
    setState(State::Connecting);
    fetch();
    return;
  }
  if (m_source.udp) {
    listenUdp();
    return;
  }
  connectTcp(m_source.host, m_source.port);
}

void AisFeed::connectTcp(const QString& host, uint16_t port)
{
  if (m_tcp != nullptr) {
    m_tcp->disconnect(this);
    m_tcp->abort();
    m_tcp->deleteLater();
  }
  m_tcp = new QTcpSocket(this);
  connect(m_tcp, &QTcpSocket::connected, this, [this]() {
    ++m_connections;
    m_retryMs = 1000;
    m_error.clear();
    setState(State::Connected);
  });
  connect(m_tcp, &QTcpSocket::readyRead, this, &AisFeed::readTcp);
  connect(m_tcp, &QTcpSocket::disconnected, this, &AisFeed::socketLost);
  connect(m_tcp, &QAbstractSocket::errorOccurred, this, &AisFeed::socketLost);
  m_pending.clear();
  setState(State::Connecting);
  m_tcp->connectToHost(host, port);
}

void AisFeed::listenUdp()
{
  m_udp = new QUdpSocket(this);
  if (!m_udp->bind(QHostAddress::Any, m_source.port)) {
    m_error = m_udp->errorString();
    m_udp->deleteLater();
    m_udp = nullptr;
    scheduleRetry();
    return;
  }
  connect(m_udp, &QUdpSocket::readyRead, this, &AisFeed::readUdp);
  ++m_connections;
  setState(State::Connected);
}

void AisFeed::socketLost()
{
  if (m_tcp == nullptr || !m_running) {
    return;
  }
  m_error = m_tcp->errorString();
  m_tcp->disconnect(this);
  m_tcp->abort();
  m_tcp->deleteLater();
  m_tcp = nullptr;
  scheduleRetry();
}

void AisFeed::scheduleRetry()
{
  setState(State::Reconnecting);
  m_retry->start(m_retryMs);
  m_retryMs = std::min(m_retryMs * 2, longestRetryMs);
}

void AisFeed::readTcp()
{
  if (m_tcp == nullptr) {
    return;
  }
  const std::size_t before = m_reports.size();
  m_pending.append(m_tcp->readAll());
  // int32_t: QByteArray takes int on Qt 5.
  int32_t start = 0;
  int32_t end = 0;
  while ((end = static_cast<int32_t>(m_pending.indexOf('\n', start))) >= 0) {
    handleLine(m_pending.mid(start, end - start).trimmed());
    start = end + 1;
  }
  m_pending.remove(0, start);
  if (m_pending.size() > longestLine) {
    m_pending.clear();
  }
  if (m_reports.size() > before) {
    emit reportsReady();
  }
}

void AisFeed::readUdp()
{
  const std::size_t before = m_reports.size();
  while (m_udp != nullptr && m_udp->hasPendingDatagrams()) {
    QByteArray datagram;
    datagram.resize(static_cast<int32_t>(m_udp->pendingDatagramSize()));
    m_udp->readDatagram(datagram.data(), datagram.size());
    for (const QByteArray& line : datagram.split('\n')) {
      handleLine(line.trimmed());
    }
  }
  if (m_reports.size() > before) {
    emit reportsReady();
  }
}

void AisFeed::handleLine(const QByteArray& line)
{
  if (line.isEmpty()) {
    return;
  }
  ++m_messages;
  const double now = nowS();
  record(line, now);
  const NmeaAisParser::Result result = m_parser.parse(line, now);
  if (result.report) {
    VesselReport report = *result.report;
    // Timed as it came, unless the line had its own (tag block) time.
    if (!result.timeS) {
      report.timeS = now;
    }
    m_reports.push_back(report);
  }
}

void AisFeed::record(const QByteArray& line, double timeS)
{
  if (!m_recording) {
    return;
  }
  if (line.startsWith('\\')) {
    m_recording->write(line + '\n');
    return;
  }
  const QByteArray block =
      "c:" + QByteArray::number(static_cast<qint64>(std::floor(timeS)));
  m_recording->write('\\' + block + '*' +
                     QByteArray::number(nmeaChecksum(block), 16)
                         .rightJustified(2, '0')
                         .toUpper() +
                     '\\' + line + '\n');
}

void AisFeed::fetch()
{
  if (!m_running || m_ticket) {
    return;
  }
  if (!m_network) {
    m_error = QStringLiteral("no network");
    setState(State::Reconnecting);
    return;
  }
  // Metadata first, then every tenth fetch; locations changed since the
  // last.
  m_fetchingVessels = m_fetches % 10 == 0;
  ++m_fetches;
  QUrl url = m_source.feedUrl();
  QString path = url.path();
  while (path.endsWith(QLatin1Char('/'))) {
    path.chop(1);
  }
  url.setPath(path + (m_fetchingVessels
                          ? QStringLiteral("/api/ais/v1/vessels")
                          : QStringLiteral("/api/ais/v1/locations")));
  if (!m_fetchingVessels && m_lastLocationMs > 0) {
    url.setQuery(QStringLiteral("from=%1").arg(m_lastLocationMs));
  }
  HttpRequest request;
  request.url = url;
  request.headers.emplace_back(
      QByteArrayLiteral("Digitraffic-User"),
      QByteArrayLiteral("TerraSoar/" TS_VERSION_STRING));
  m_ticket = m_network->start(request);
  m_pollCheck->start();
}

void AisFeed::checkFetch()
{
  if (!m_ticket || !m_ticket->isFinished()) {
    return;
  }
  m_pollCheck->stop();
  const HttpResponse response = m_ticket->response();
  m_ticket.reset();
  std::optional<std::vector<VesselReport>> reports;
  if (response.isSuccess()) {
    reports = m_fetchingVessels ? readDigitrafficVessels(response.body)
                                : readDigitrafficLocations(response.body);
  }
  const std::optional<AisFeedEntry> entry = aisFeedEntry(m_source.feed);
  const int32_t pollMs =
      static_cast<int32_t>((entry ? entry->pollS : 30.0) * 1000.0);
  if (!reports) {
    m_error = response.errorString.isEmpty()
                  ? QStringLiteral("HTTP %1").arg(response.status)
                  : response.errorString;
    // Back off: the wait doubles, up to five minutes.
    m_retryMs = std::min(std::max(m_retryMs * 2, pollMs), longestPollMs);
    setState(State::Reconnecting);
    m_poll->start(m_retryMs);
    return;
  }
  ++m_messages;
  m_retryMs = 1000;
  if (m_state != State::Connected) {
    ++m_connections;
  }
  m_error.clear();
  setState(State::Connected);
  for (const VesselReport& r : *reports) {
    if (!m_fetchingVessels) {
      m_lastLocationMs =
          std::max(m_lastLocationMs,
                   static_cast<int64_t>(std::llround(r.timeS * 1000.0)));
    }
    m_reports.push_back(r);
  }
  if (!reports->empty()) {
    emit reportsReady();
  }
  // Straight on to the locations after the metadata.
  m_poll->start(m_fetchingVessels ? 0 : pollMs);
}

std::vector<VesselReport> AisFeed::takeReports()
{
  if (m_recording) {
    m_recording->flush();
  }
  std::vector<VesselReport> reports;
  reports.swap(m_reports);
  return reports;
}

} // namespace ts
