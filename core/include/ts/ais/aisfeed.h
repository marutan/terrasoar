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

/**
 * @file
 * Live AIS: NMEA from a receiver over TCP or UDP, and the open online
 * feeds (Digitraffic over REST, Kystverket over TCP), as vessel reports.
 */
#ifndef TS_AIS_AISFEED_H
#define TS_AIS_AISFEED_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QString>
#include <QUrl>

#include "ts/ais/aisreaders.h"
#include "ts/ais/vessellayer.h"
#include "ts/ais/vesseltracks.h"

class QTcpSocket;
class QTimer;
class QUdpSocket;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * Reads Digitraffic's AIS REST answers: the locations (GeoJSON features,
 * with "not available" values left out) and the vessels' metadata, as
 * vessel reports. Returns nothing for a document that isn't one.
 */
std::optional<std::vector<VesselReport>>
readDigitrafficLocations(const QByteArray& json);

/** The same for Digitraffic's vessel metadata. */
std::optional<std::vector<VesselReport>>
readDigitrafficVessels(const QByteArray& json);

/**
 * Reads a live AIS source on the GUI thread and turns it into vessel
 * reports:
 *
 * - **TCP** (a receiver, or Kystverket's feed): NMEA lines, connecting
 *   again after the connection drops or can't be made (1 s, then twice as
 *   long each time, up to 30 s).
 * - **UDP**: NMEA datagrams on a port (AIS-catcher, rtl-ais).
 * - **Digitraffic**: the locations changed since the last fetch, every
 *   AisFeedEntry::pollS, and the vessels' metadata every ten fetches; on
 *   failure the wait doubles, up to five minutes.
 *
 * Reports are timed as the messages arrive (NMEA without tag blocks), or
 * by the feed's own times. A recording, if asked for, keeps each NMEA line
 * with a tag block of its arrival time, which reads back as a recording.
 *
 * @guithread
 */
class AisFeed : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AisFeed)

public:
  /** How the connection is. */
  enum class State : uint8_t {
    Connecting,   ///< Connecting (or fetching the first time).
    Connected,    ///< Receiving.
    Reconnecting, ///< Lost or refused; trying again soon.
    Stopped,      ///< Not started, or stopped.
  };

  /**
   * Reads @p source (a receiver's or a feed's), fetching with @p network
   * (needed for Digitraffic).
   */
  AisFeed(VesselSource source, std::shared_ptr<NetworkClient> network,
          QObject* parent);
  ~AisFeed() override;

  /** Starts (and keeps trying). */
  void start();

  /** Stops. */
  void stop();

  /** Returns how the connection is. */
  State state() const { return m_state; }

  /** Returns why it isn't connected (untranslated), if known. */
  const QString& errorString() const { return m_error; }

  /** Returns the reports since the last call. */
  std::vector<VesselReport> takeReports();

  /** Returns how many sentences (or answers) came. */
  int64_t messages() const { return m_messages; }

  /** Returns how many times it connected. */
  int32_t connections() const { return m_connections; }

  /** Returns the current wait before trying again, milliseconds. */
  int32_t retryDelayMs() const { return m_retryMs; }

signals:
  /** takeReports() has reports. */
  void reportsReady();

  /** state() changed. */
  void stateChanged();

private:
  void setState(State state);
  void connectNow();
  void connectTcp(const QString& host, uint16_t port);
  void listenUdp();
  void readTcp();
  void readUdp();
  void socketLost();
  void scheduleRetry();
  void handleLine(const QByteArray& line);
  void fetch();
  void checkFetch();
  void record(const QByteArray& line, double timeS);

  VesselSource m_source;
  std::shared_ptr<NetworkClient> m_network;
  QTcpSocket* m_tcp = nullptr;
  QUdpSocket* m_udp = nullptr;
  QTimer* m_retry = nullptr;
  QTimer* m_poll = nullptr;
  QTimer* m_pollCheck = nullptr;
  std::shared_ptr<NetworkTicket> m_ticket;
  bool m_fetchingVessels = false;
  int64_t m_fetches = 0;
  int64_t m_lastLocationMs = 0;
  NmeaAisParser m_parser;
  QByteArray m_pending;
  std::unique_ptr<QFile> m_recording;
  std::vector<VesselReport> m_reports;
  State m_state = State::Stopped;
  QString m_error;
  int32_t m_retryMs = 1000;
  int64_t m_messages = 0;
  int32_t m_connections = 0;
  bool m_running = false;
};

} // namespace ts

#endif // TS_AIS_AISFEED_H
