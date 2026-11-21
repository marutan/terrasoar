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
 * A live connection to an ADS-B receiver (dump1090, readsb): SBS, AVR or
 * Beast over TCP, or aircraft.json over HTTP, kept open, decoded into
 * reports and optionally recorded.
 */
#ifndef TS_ADSB_RECEIVERFEED_H
#define TS_ADSB_RECEIVERFEED_H

#include <cstdint>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QString>

#include "ts/adsb/adsbdecoder.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftlayer.h"
#include "ts/adsb/aircraftreport.h"

class QTcpSocket;
class QTimer;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * Reads a receiver's live output and turns it into aircraft reports,
 * timed as they arrive (a receiver's own counter isn't a clock).
 *
 * - TCP (SBS, AVR, Beast): the connection is made again after it drops or
 *   can't be made, waiting 1 s, then twice as long each time, up to 30 s.
 * - JSON: aircraft.json is fetched every AircraftSource::pollS (at least
 *   1 s) through the NetworkClient.
 *
 * A recording, if asked for, is written as it comes: SBS lines as they
 * are, raw messages as "time,hex" lines, JSON snapshots one a line, all
 * of which the recording readers read back.
 *
 * @guithread
 */
class ReceiverFeed : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ReceiverFeed)

public:
  /** How the connection is. */
  enum class State : uint8_t {
    Connecting,   ///< Connecting (or fetching the first time).
    Connected,    ///< Receiving.
    Reconnecting, ///< Lost or refused; trying again soon.
    Stopped,      ///< Not started, or stopped.
  };

  /**
   * Reads from @p source (a receiver's), fetching JSON with @p network
   * (may be null for TCP).
   */
  ReceiverFeed(const AircraftSource& source,
               std::shared_ptr<NetworkClient> network, QObject* parent);
  ~ReceiverFeed() override;

  /** Connects (and keeps connecting). */
  void start();

  /** Disconnects and stops trying. */
  void stop();

  /** Returns how the connection is. */
  State state() const { return m_state; }

  /** Returns why it isn't connected (untranslated system text), if known. */
  const QString& errorString() const { return m_error; }

  /** Returns why the recording can't be written, if it can't. */
  const QString& recordingError() const { return m_recordingError; }

  /** Returns the reports since the last call. */
  std::vector<AircraftReport> takeReports();

  /** Returns how many messages (or lines, or snapshots) came. */
  int64_t messages() const { return m_messages; }

  /** Returns how many times it connected. */
  int32_t connections() const { return m_connections; }

  /** Returns the next wait before connecting again, milliseconds. */
  int32_t retryDelayMs() const { return m_retryMs; }

signals:
  /** takeReports() has reports. */
  void reportsReady();

  /** state() changed. */
  void stateChanged();

private:
  void connectNow();
  void readSocket();
  void socketConnected();
  void socketLost();
  void poll();
  void checkPoll();
  void setState(State state);
  void scheduleRetry();
  void handleLine(const QByteArray& line, double nowS);
  void handleMessage(const QByteArray& bytes, double nowS);
  void handleSnapshot(const QByteArray& json);
  void record(const QByteArray& data);
  void add(const AircraftReport& report);

  AircraftSource m_source;
  std::shared_ptr<NetworkClient> m_network;
  QTcpSocket* m_socket = nullptr;
  QTimer* m_retry = nullptr;
  QTimer* m_pollTimer = nullptr;
  QTimer* m_pollCheck = nullptr;
  std::shared_ptr<NetworkTicket> m_ticket;
  AdsbDecoder m_decoder;
  SbsParser m_sbs;
  BeastParser m_beast;
  QByteArray m_pending;
  std::unique_ptr<QFile> m_recording;
  QString m_recordingError;
  std::vector<AircraftReport> m_reports;
  State m_state = State::Stopped;
  QString m_error;
  int32_t m_retryMs = 1000;
  int64_t m_messages = 0;
  int32_t m_connections = 0;
  bool m_running = false;
};

} // namespace ts

#endif // TS_ADSB_RECEIVERFEED_H
