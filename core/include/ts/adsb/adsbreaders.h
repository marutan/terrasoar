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
 * Readers of recorded ADS-B data: SBS (BaseStation), raw Mode S messages
 * (AVR text and Beast binary), readsb/dump1090 JSON, tar1090 traces and
 * CSV with chosen columns.
 */
#ifndef TS_ADSB_ADSBREADERS_H
#define TS_ADSB_ADSBREADERS_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QChar>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "ts/adsb/adsbdecoder.h"
#include "ts/adsb/aircraftreport.h"

namespace ts
{

/** The formats read. */
enum class AdsbFormat : uint8_t {
  Unknown,    ///< Not known (detect it).
  Sbs,        ///< SBS-1/BaseStation CSV, port 30003's format.
  Raw,        ///< Mode S messages in hex: AVR ("*…;", "@…;") or "time,hex".
  Beast,      ///< Beast binary (port 30005).
  ReadsbJson, ///< readsb/dump1090 aircraft.json snapshots.
  Trace,      ///< A tar1090/readsb trace file (one aircraft).
  Csv,        ///< CSV with columns chosen by the user.
};

/** Returns the format's identifier ("sbs", "raw", …) for projects. */
QString adsbFormatId(AdsbFormat format);

/** Returns the format with identifier @p id, or Unknown. */
AdsbFormat adsbFormatFromId(const QString& id);

/** How a CSV file's altitude column is meant. */
enum class CsvAltitude : uint8_t {
  BaroFeet,        ///< Barometric (pressure) altitude, feet.
  BaroMetres,      ///< Barometric (pressure) altitude, metres.
  GeometricFeet,   ///< Above the WGS 84 ellipsoid, feet.
  GeometricMetres, ///< Above the WGS 84 ellipsoid, metres.
};

/** How a CSV file's speed column is meant. */
enum class CsvSpeed : uint8_t {
  Knots,             ///< Knots.
  MetresPerSecond,   ///< Metres a second.
  KilometresPerHour, ///< Kilometres an hour.
};

/** How a CSV file's times are written. */
enum class CsvTime : uint8_t {
  UnixSeconds,      ///< Seconds since 1970 (UTC), decimals allowed.
  UnixMilliseconds, ///< Milliseconds since 1970 (UTC).
  Iso8601,          ///< ISO 8601 date and time (UTC unless an offset).
};

/** Which columns of a CSV file hold what (−1: none). */
struct CsvColumns {
  /** The separator. */
  QChar separator = QLatin1Char(',');
  /** True if the first line names the columns. */
  bool header = true;
  /** Time (required). */
  int32_t time = -1;
  /** ICAO address in hex (required). */
  int32_t address = -1;
  /** Latitude, degrees (required). */
  int32_t latitude = -1;
  /** Longitude, degrees (required). */
  int32_t longitude = -1;
  /** Altitude. */
  int32_t altitude = -1;
  /** Track or heading, degrees. */
  int32_t heading = -1;
  /** Ground speed. */
  int32_t speed = -1;
  /** Callsign. */
  int32_t callsign = -1;
  /** Vertical rate, feet a minute. */
  int32_t verticalRate = -1;
  /** How times are written. */
  CsvTime timeFormat = CsvTime::UnixSeconds;
  /** What the altitude is. */
  CsvAltitude altitudeKind = CsvAltitude::BaroFeet;
  /** What the speed is. */
  CsvSpeed speedUnit = CsvSpeed::Knots;

  /** Returns true if the required columns are set. */
  bool isComplete() const;
  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form (missing members keep their defaults). */
  static CsvColumns fromJson(const QJsonObject& json);
};

/**
 * Guesses the columns from a CSV file's first line(s) @p head: by the
 * names in a header (time, icao/hex/address, lat, lon, alt…), the
 * separator from the first line.
 */
CsvColumns guessCsvColumns(const QByteArray& head);

/** Splits one CSV line (quotes allowed) into its fields. */
QStringList splitCsvLine(const QString& line, QChar separator);

/** How recordings are read. */
struct AdsbReadOptions {
  /** How raw messages are decoded (the receiver's position). */
  AdsbDecoderSettings decoder;
  /**
   * The time of the first message, Unix seconds, for formats whose times
   * are only a receiver's counter (Beast, AVR "@…;") or missing.
   */
  double startTimeS = 0.0;
  /**
   * True if SBS times are this computer's local time (dump1090 and readsb
   * write local time), false if UTC.
   */
  bool sbsLocalTime = true;
  /** The columns of a CSV file. */
  CsvColumns csv;
};

/** A line (or record) that couldn't be read. */
struct AdsbReadProblem {
  /** Its line (from 1), or record for binary formats. */
  int64_t line = 0;
  /** What is wrong (translated). */
  QString message;
};

/** What reading found. */
struct AdsbReadResult {
  /** The format read (detected, if Unknown was given). */
  AdsbFormat format = AdsbFormat::Unknown;
  /** Lines (or frames) read. */
  int64_t records = 0;
  /** Reports made. */
  int64_t reports = 0;
  /** Problems, the first maximumProblems of them. */
  std::vector<AdsbReadProblem> problems;
  /** Problems in all. */
  int64_t problemCount = 0;
  /** Raw messages refused (parity, unknown address): normal noise. */
  int64_t messagesRefused = 0;
  /** True if the messages had no times (all at the start time). */
  bool untimed = false;
  /** Why nothing could be read (translated), or empty. */
  QString error;

  /** Most problems kept. */
  static constexpr int64_t maximumProblems = 100;
  /** Records a problem at @p line. */
  void addProblem(int64_t line, const QString& message);
};

/** Receives each report as it is read. */
using AircraftReportSink = std::function<void(const AircraftReport&)>;

/**
 * Returns the format of data starting with @p head (its first few
 * kilobytes, uncompressed), or Unknown.
 */
AdsbFormat detectAdsbFormat(const QByteArray& head);

/**
 * Reads @p device in @p format (detected if Unknown), sending each report
 * to @p sink. @p progress, if set, is called now and then with the bytes
 * read so far; it returns false to stop.
 */
AdsbReadResult readAdsb(QIODevice& device, AdsbFormat format,
                        const AdsbReadOptions& options,
                        const AircraftReportSink& sink,
                        const std::function<bool(qint64)>& progress = {});

/** The same for @p data in memory (gzip-compressed or not). */
AdsbReadResult readAdsbData(const QByteArray& data, AdsbFormat format,
                            const AdsbReadOptions& options,
                            const AircraftReportSink& sink);

/**
 * The same for the file @p path; a gzip-compressed file (.gz, or tar1090's
 * compressed traces) is uncompressed first.
 */
AdsbReadResult readAdsbFile(const QString& path, AdsbFormat format,
                            const AdsbReadOptions& options,
                            const AircraftReportSink& sink,
                            const std::function<bool(qint64)>& progress = {});

/** Returns @p data uncompressed if it is gzip, else as it is. */
QByteArray gunzipIfCompressed(const QByteArray& data, bool* ok = nullptr);

/**
 * Reads SBS lines one at a time (files and port 30003).
 *
 * @reentrant (a parser keeps the last date it read: one per thread)
 */
class SbsParser
{
public:
  /** Creates a parser reading times as local time or UTC. */
  explicit SbsParser(bool localTime = true) : m_localTime(localTime) {}

  /**
   * Reads @p line. Returns the report; nothing for lines that carry no
   * aircraft data (SEL, ID, CLK, …), with @p error set for lines that
   * are not SBS at all.
   */
  std::optional<AircraftReport> parse(const QByteArray& line,
                                      QString* error = nullptr) const;

private:
  /** The Unix time of an SBS date and time, if they can be read. */
  std::optional<double> sbsTime(const QByteArray& date,
                                const QByteArray& time) const;

  bool m_localTime;
  /** The last date and hour read, and when that hour began. */
  mutable QByteArray m_lastDate;
  mutable int32_t m_lastHour = -1;
  mutable double m_hourStartS = 0.0;
};

/** A raw message and its receiver time. */
struct TimedMessage {
  /** The bytes (7 or 14). */
  QByteArray bytes;
  /** The receiver's 12 MHz counter, if given. */
  std::optional<uint64_t> counter;
  /** A Unix time, if given. */
  std::optional<double> timeS;
};

/**
 * Reads one line of raw messages: "*HEX;" (AVR), "@CCCCCCCCCCCCHEX;"
 * (AVR with a 12 MHz counter), or "TIME,HEX" / "TIME HEX" with a Unix
 * time. Returns nothing (and @p error, if the line isn't blank) if it is
 * none of those; Mode A/C replies (4 digits) are skipped quietly.
 */
std::optional<TimedMessage> parseRawLine(const QByteArray& line,
                                         QString* error = nullptr);

/**
 * Reads Beast binary frames from a stream in pieces (files and port
 * 30005): 0x1a, a type ('1' Mode A/C, '2' Mode S short, '3' Mode S long,
 * '4' status), a 6-byte 12 MHz counter, a signal byte and the message,
 * with 0x1a doubled inside.
 *
 * @reentrant
 */
class BeastParser
{
public:
  /** Adds @p data and returns the Mode S messages completed by it. */
  std::vector<TimedMessage> feed(const QByteArray& data);

  /** Returns how many bytes were skipped as not Beast. */
  int64_t skippedBytes() const { return m_skipped; }

private:
  QByteArray m_pending;
  int64_t m_skipped = 0;
};

/**
 * Turns receiver times into Unix times: the 12 MHz counter from the first
 * one counted from a start time (and its wrap at 2⁴⁸).
 *
 * @reentrant
 */
class ReceiverClock
{
public:
  /** Counts from @p startTimeS. */
  explicit ReceiverClock(double startTimeS) : m_startTimeS(startTimeS) {}

  /** Returns the Unix time of @p counter. */
  double timeOf(uint64_t counter);

private:
  double m_startTimeS;
  std::optional<uint64_t> m_first;
  uint64_t m_last = 0;
  double m_wraps = 0.0;
};

/**
 * Reads readsb/dump1090 aircraft.json (one snapshot, or an "ac" list as
 * airplanes.live and adsb.lol give it), sending a report for each
 * aircraft. Returns the snapshot's time, or nothing (and @p error) if it
 * isn't one.
 */
std::optional<double> readReadsbSnapshot(const QJsonObject& snapshot,
                                         const AircraftReportSink& sink,
                                         QString* error = nullptr);

} // namespace ts

#endif // TS_ADSB_ADSBREADERS_H
