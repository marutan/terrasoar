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
 * Readers of recorded AIS data: NMEA 0183 logs and CSV (NOAA
 * MarineCadastre's columns, or columns chosen by the user).
 */
#ifndef TS_AIS_AISREADERS_H
#define TS_AIS_AISREADERS_H

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QChar>
#include <QIODevice>
#include <QJsonObject>
#include <QString>

#include "ts/ais/aismessage.h"
#include "ts/ais/vesseltracks.h"

namespace ts
{

/** The formats read. */
enum class AisFormat : uint8_t {
  Unknown, ///< Not known (detect it).
  Nmea,    ///< NMEA 0183 sentences (!AIVDM, !AIVDO), with or without times.
  Csv,     ///< CSV: NOAA MarineCadastre's columns, or chosen ones.
};

/** Returns the format's identifier ("nmea", "csv") for projects. */
QString aisFormatId(AisFormat format);

/** Returns the format with identifier @p id, or Unknown. */
AisFormat aisFormatFromId(const QString& id);

/** Which columns of a CSV file hold what (−1: none). */
struct AisCsvColumns {
  /** The separator. */
  QChar separator = QLatin1Char(',');
  /** True if the first line names the columns. */
  bool header = true;
  /** Time (required): Unix seconds or ISO 8601 (UTC unless an offset). */
  int32_t time = -1;
  /** MMSI (required). */
  int32_t mmsi = -1;
  /** Latitude, degrees (required). */
  int32_t latitude = -1;
  /** Longitude, degrees (required). */
  int32_t longitude = -1;
  /** Speed over ground, knots. */
  int32_t speed = -1;
  /** Course over ground, degrees. */
  int32_t course = -1;
  /** True heading, degrees (511: not available). */
  int32_t heading = -1;
  /** Name. */
  int32_t name = -1;
  /** Callsign. */
  int32_t callsign = -1;
  /** IMO number ("IMO1234567" or digits). */
  int32_t imo = -1;
  /** AIS ship type number. */
  int32_t shipType = -1;
  /** Navigational status number. */
  int32_t status = -1;
  /** Length, metres. */
  int32_t length = -1;
  /** Width (beam), metres. */
  int32_t width = -1;
  /** Draught, metres. */
  int32_t draught = -1;
  /** Transceiver class ("A" or "B"). */
  int32_t transceiver = -1;

  /** Returns true if the required columns are set. */
  bool isComplete() const;
  /** Returns the JSON form. */
  QJsonObject toJson() const;
  /** Reads the JSON form (missing members keep their defaults). */
  static AisCsvColumns fromJson(const QJsonObject& json);
};

/**
 * Guesses a CSV file's columns from its first line: NOAA MarineCadastre's
 * names (MMSI, BaseDateTime, LAT, LON, SOG, COG, Heading, VesselName, …)
 * and common others.
 */
AisCsvColumns guessAisCsvColumns(const QByteArray& head);

/** How recordings are read. */
struct AisReadOptions {
  /**
   * The time of the first sentence of a log without times, Unix seconds;
   * its sentences are then a second apart, in their order.
   */
  double startTimeS = 0.0;
  /** The columns of a CSV file. */
  AisCsvColumns csv;
};

/** A line that couldn't be read. */
struct AisReadProblem {
  /** Its line (from 1). */
  int64_t line = 0;
  /** What is wrong (translated). */
  QString message;
};

/** What reading found. */
struct AisReadResult {
  /** The format read. */
  AisFormat format = AisFormat::Unknown;
  /** Lines (or sentences) read. */
  int64_t records = 0;
  /** Reports made. */
  int64_t reports = 0;
  /** Problems, the first maximumProblems of them. */
  std::vector<AisReadProblem> problems;
  /** Problems in all. */
  int64_t problemCount = 0;
  /** Sentences whose checksum was wrong (dropped). */
  int64_t checksumErrors = 0;
  /** Messages of types not read, by type: counted, not errors. */
  std::map<int32_t, int64_t> otherMessages;
  /** Multi-sentence messages never completed. */
  int64_t incomplete = 0;
  /** True if the sentences had no times (a second apart from the start). */
  bool untimed = false;
  /** Why nothing could be read (translated), or empty. */
  QString error;

  /** Most problems kept. */
  static constexpr int64_t maximumProblems = 100;
  /** Records a problem at @p line. */
  void addProblem(int64_t line, const QString& message);
};

/** Receives each report as it is read. */
using VesselReportSink = std::function<void(const VesselReport&)>;

/**
 * Reads NMEA AIS lines one at a time, joining multi-sentence messages
 * (files and live receivers). A line's time is its tag block's (c:), a
 * time before the sentence (Unix seconds or ISO 8601), or a Unix time
 * after its checksum; without any, the caller's.
 *
 * @reentrant
 */
class NmeaAisParser
{
public:
  /** What one line gave. */
  struct Result {
    /** The report of the message the line completed, if any. */
    std::optional<VesselReport> report;
    /** The line's own time, if it had one. */
    std::optional<double> timeS;
    /** Why it wasn't a sentence (None if it was). */
    AisSentenceError error = AisSentenceError::None;
    /** The type of a message completed but not read (0: none). */
    int32_t otherType = 0;
  };

  /** Reads @p line, timing a message without a time at @p fallbackS. */
  Result parse(const QByteArray& line, double fallbackS);

  /** Returns how many incomplete messages were dropped. */
  int64_t incomplete() const { return m_assembler.dropped(); }

private:
  AisAssembler m_assembler;
};

/** Returns the format of data starting with @p head, or Unknown. */
AisFormat detectAisFormat(const QByteArray& head);

/**
 * Reads @p device in @p format (detected if Unknown), sending each report
 * to @p sink; @p progress, if set, is called now and then with the bytes
 * read, and returns false to stop.
 */
AisReadResult readAis(QIODevice& device, AisFormat format,
                      const AisReadOptions& options,
                      const VesselReportSink& sink,
                      const std::function<bool(qint64)>& progress = {});

/** The same for @p data in memory (gzip-compressed or not). */
AisReadResult readAisData(const QByteArray& data, AisFormat format,
                          const AisReadOptions& options,
                          const VesselReportSink& sink);

/** The same for the file @p path (gzip-compressed or not). */
AisReadResult readAisFile(const QString& path, AisFormat format,
                          const AisReadOptions& options,
                          const VesselReportSink& sink,
                          const std::function<bool(qint64)>& progress = {});

} // namespace ts

#endif // TS_AIS_AISREADERS_H
