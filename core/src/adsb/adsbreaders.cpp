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
#include "ts/adsb/adsbreaders.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

#include <QBuffer>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTime>
#include <QTimeZone>

#include <cpl_vsi.h>

#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

/** Beast's 12 MHz counter, ticks a second. */
constexpr double beastTicksPerSecond = 12.0e6;

/** The 48-bit counter's range. */
constexpr double counterRange = 281474976710656.0;

std::optional<double> toDouble(const QString& text)
{
  bool ok = false;
  const double value = text.trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> jsonNumber(const QJsonValue& value)
{
  if (value.isDouble()) {
    return value.toDouble();
  }
  return std::nullopt;
}

std::optional<AdsbEmergency> emergencyFromSquawk(const QString& squawk)
{
  if (squawk == QLatin1String("7500")) {
    return AdsbEmergency::UnlawfulInterference;
  }
  if (squawk == QLatin1String("7600")) {
    return AdsbEmergency::NoCommunications;
  }
  if (squawk == QLatin1String("7700")) {
    return AdsbEmergency::General;
  }
  return std::nullopt;
}

std::optional<AdsbEmergency> emergencyFromName(const QString& name)
{
  static const std::array<std::pair<const char*, AdsbEmergency>, 8> names = {{
      {"none", AdsbEmergency::None},
      {"general", AdsbEmergency::General},
      {"lifeguard", AdsbEmergency::Medical},
      {"minfuel", AdsbEmergency::MinimumFuel},
      {"nordo", AdsbEmergency::NoCommunications},
      {"unlawful", AdsbEmergency::UnlawfulInterference},
      {"downed", AdsbEmergency::Downed},
      {"reserved", AdsbEmergency::Reserved},
  }};
  for (const std::pair<const char*, AdsbEmergency>& entry : names) {
    if (name == QLatin1String(entry.first)) {
      return entry.second;
    }
  }
  return std::nullopt;
}

/** The first line of @p text (all of it if it has one line). */
QByteArray firstLine(const QByteArray& text)
{
  const qsizetype end = text.indexOf('\n');
  return (end < 0 ? text : text.left(end)).trimmed();
}

bool isHex(const QByteArray& text)
{
  return !text.isEmpty() && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
  });
}

/** A readsb aircraft object (or a trace's) into @p report. */
void readAircraftObject(const QJsonObject& ac, AircraftReport& report)
{
  const QJsonValue baro = ac.value(QLatin1String("alt_baro"));
  if (baro.isString() && baro.toString() == QLatin1String("ground")) {
    report.onGround = true;
  } else if (baro.isDouble()) {
    report.baroAltitudeFt = baro.toDouble();
    report.onGround = false;
  }
  report.geometricAltitudeFt = jsonNumber(ac.value(QLatin1String("alt_geom")));
  report.groundSpeedKt = jsonNumber(ac.value(QLatin1String("gs")));
  report.trackDeg = jsonNumber(ac.value(QLatin1String("track")));
  report.verticalRateFtMin = jsonNumber(ac.value(QLatin1String("baro_rate")));
  if (!report.verticalRateFtMin) {
    report.verticalRateFtMin = jsonNumber(ac.value(QLatin1String("geom_rate")));
  }
  const QString flight = ac.value(QLatin1String("flight")).toString().trimmed();
  if (!flight.isEmpty()) {
    report.callsign = flight;
  }
  const QString squawk = ac.value(QLatin1String("squawk")).toString();
  if (!squawk.isEmpty()) {
    report.squawk = squawk;
  }
  const QString emergency = ac.value(QLatin1String("emergency")).toString();
  if (!emergency.isEmpty()) {
    report.emergency = emergencyFromName(emergency);
  }
  const QString category = ac.value(QLatin1String("category")).toString();
  if (!category.isEmpty()) {
    report.category = category;
  }
  const QString registration = ac.value(QLatin1String("r")).toString();
  if (!registration.isEmpty()) {
    report.registration = registration;
  }
  const QString type = ac.value(QLatin1String("t")).toString();
  if (!type.isEmpty()) {
    report.aircraftType = type;
  }
}

/** Reads lines from a device, counting them. */
class LineReader
{
public:
  explicit LineReader(QIODevice& device) : m_device(device) {}

  /** Reads the next line (without its end) into @p line. */
  bool next(QByteArray& line)
  {
    if (m_device.atEnd()) {
      return false;
    }
    line = m_device.readLine();
    while (!line.isEmpty() && (line.endsWith('\n') || line.endsWith('\r'))) {
      line.chop(1);
    }
    ++m_line;
    return true;
  }

  int64_t line() const { return m_line; }

private:
  QIODevice& m_device;
  int64_t m_line = 0;
};

/** Calls @p progress every so many records; false: stop. */
class Progress
{
public:
  Progress(QIODevice& device, const std::function<bool(qint64)>& progress)
      : m_device(device), m_progress(progress)
  {
  }

  bool ok()
  {
    if (!m_progress || ++m_count % 4096 != 0) {
      return true;
    }
    return m_progress(m_device.pos());
  }

private:
  QIODevice& m_device;
  const std::function<bool(qint64)>& m_progress;
  int64_t m_count = 0;
};

void readSbs(QIODevice& device, const AdsbReadOptions& options,
             const AircraftReportSink& sink, Progress& progress,
             AdsbReadResult& result)
{
  const SbsParser parser(options.sbsLocalTime);
  LineReader lines(device);
  QByteArray line;
  while (lines.next(line) && progress.ok()) {
    if (line.trimmed().isEmpty()) {
      continue;
    }
    ++result.records;
    QString error;
    const std::optional<AircraftReport> report = parser.parse(line, &error);
    if (report) {
      ++result.reports;
      sink(*report);
    } else if (!error.isEmpty()) {
      result.addProblem(lines.line(), error);
    }
  }
}

/** Sends a raw message through the decoder. */
void decodeTimed(AdsbDecoder& decoder, const QByteArray& bytes, double timeS,
                 const AircraftReportSink& sink, AdsbReadResult& result)
{
  const int64_t refused =
      decoder.counts().refused + decoder.counts().unknownAddress;
  const std::optional<AircraftReport> report = decoder.decode(bytes, timeS);
  result.messagesRefused +=
      decoder.counts().refused + decoder.counts().unknownAddress - refused;
  if (report) {
    ++result.reports;
    sink(*report);
  }
}

void readRaw(QIODevice& device, const AdsbReadOptions& options,
             const AircraftReportSink& sink, Progress& progress,
             AdsbReadResult& result)
{
  AdsbDecoder decoder(options.decoder);
  ReceiverClock clock(options.startTimeS);
  LineReader lines(device);
  QByteArray line;
  bool timed = false;
  while (lines.next(line) && progress.ok()) {
    if (line.trimmed().isEmpty()) {
      continue;
    }
    ++result.records;
    QString error;
    const std::optional<TimedMessage> message = parseRawLine(line, &error);
    if (!message) {
      if (!error.isEmpty()) {
        result.addProblem(lines.line(), error);
      }
      continue;
    }
    double timeS = options.startTimeS;
    if (message->timeS) {
      timeS = *message->timeS;
      timed = true;
    } else if (message->counter) {
      timeS = clock.timeOf(*message->counter);
      timed = true;
    }
    decodeTimed(decoder, message->bytes, timeS, sink, result);
  }
  result.untimed = result.records > 0 && !timed;
}

void readBeast(QIODevice& device, const AdsbReadOptions& options,
               const AircraftReportSink& sink, Progress& progress,
               AdsbReadResult& result)
{
  AdsbDecoder decoder(options.decoder);
  ReceiverClock clock(options.startTimeS);
  BeastParser parser;
  while (!device.atEnd() && progress.ok()) {
    const QByteArray chunk = device.read(65536);
    if (chunk.isEmpty()) {
      break;
    }
    for (const TimedMessage& message : parser.feed(chunk)) {
      ++result.records;
      const double timeS =
          message.counter ? clock.timeOf(*message.counter) : options.startTimeS;
      decodeTimed(decoder, message.bytes, timeS, sink, result);
    }
  }
  if (parser.skippedBytes() > 0) {
    result.addProblem(0, QCoreApplication::translate(
                             "ts::AdsbReader",
                             "%1 bytes were not Beast frames and were "
                             "skipped.")
                             .arg(parser.skippedBytes()));
  }
}

/** Reads one JSON value of snapshots: an object, or an array of them. */
void readSnapshots(const QJsonValue& value, const AircraftReportSink& sink,
                   int64_t line, AdsbReadResult& result)
{
  const std::function<void(const AircraftReport&)> counted =
      [&sink, &result](const AircraftReport& report) {
        ++result.reports;
        sink(report);
      };
  if (value.isArray()) {
    for (const QJsonValue& item : value.toArray()) {
      readSnapshots(item, sink, line, result);
    }
    return;
  }
  ++result.records;
  QString error;
  if (!readReadsbSnapshot(value.toObject(), counted, &error)) {
    result.addProblem(line, error);
  }
}

void readReadsb(QIODevice& device, const AircraftReportSink& sink,
                AdsbReadResult& result)
{
  const QByteArray data = device.readAll();
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
  if (parseError.error == QJsonParseError::NoError) {
    readSnapshots(document.isArray() ? QJsonValue(document.array())
                                     : QJsonValue(document.object()),
                  sink, 1, result);
    return;
  }
  // JSON lines: a snapshot on each.
  int64_t lineNumber = 0;
  for (const QByteArray& line : data.split('\n')) {
    ++lineNumber;
    if (line.trimmed().isEmpty()) {
      continue;
    }
    const QJsonDocument snapshot = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
      ++result.records;
      result.addProblem(lineNumber, parseError.errorString());
      continue;
    }
    readSnapshots(QJsonValue(snapshot.object()), sink, lineNumber, result);
  }
}

void readTrace(QIODevice& device, const AircraftReportSink& sink,
               AdsbReadResult& result)
{
  QJsonParseError parseError{};
  const QJsonDocument document =
      QJsonDocument::fromJson(device.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    result.error = parseError.errorString();
    return;
  }
  const QJsonObject trace = document.object();
  const std::optional<uint32_t> address =
      parseIcaoAddress(trace.value(QLatin1String("icao")).toString());
  const std::optional<double> start =
      jsonNumber(trace.value(QLatin1String("timestamp")));
  if (!address || !start) {
    result.error = QCoreApplication::translate(
        "ts::AdsbReader", "Not a trace file: no ICAO address or timestamp.");
    return;
  }
  const QString registration = trace.value(QLatin1String("r")).toString();
  const QString type = trace.value(QLatin1String("t")).toString();
  int64_t index = 0;
  for (const QJsonValue& item : trace.value(QLatin1String("trace")).toArray()) {
    ++index;
    ++result.records;
    const QJsonArray point = item.toArray();
    const std::optional<double> dt = jsonNumber(point.at(0));
    const std::optional<double> lat = jsonNumber(point.at(1));
    const std::optional<double> lon = jsonNumber(point.at(2));
    if (!dt || !lat || !lon || std::fabs(*lat) > 90.0) {
      result.addProblem(
          index,
          QCoreApplication::translate(
              "ts::AdsbReader", "A trace point without a time or position."));
      continue;
    }
    AircraftReport report;
    report.address = *address;
    report.timeS = *start + *dt;
    report.position = CprPosition{*lat, normaliseLonDeg(*lon)};
    const int32_t flags = point.at(6).toInt();
    const QJsonValue altitude = point.at(3);
    if (altitude.isString() && altitude.toString() == QLatin1String("ground")) {
      report.onGround = true;
    } else if (altitude.isDouble()) {
      report.onGround = false;
      if ((flags & 8) != 0) {
        report.geometricAltitudeFt = altitude.toDouble();
      } else {
        report.baroAltitudeFt = altitude.toDouble();
      }
    }
    report.groundSpeedKt = jsonNumber(point.at(4));
    report.trackDeg = jsonNumber(point.at(5));
    report.verticalRateFtMin = jsonNumber(point.at(7));
    if (point.size() > 8 && point.at(8).isObject()) {
      readAircraftObject(point.at(8).toObject(), report);
    }
    if (point.size() > 10 && !report.geometricAltitudeFt) {
      report.geometricAltitudeFt = jsonNumber(point.at(10));
    }
    if (!registration.isEmpty()) {
      report.registration = registration;
    }
    if (!type.isEmpty()) {
      report.aircraftType = type;
    }
    ++result.reports;
    sink(report);
  }
}

std::optional<double> parseCsvTime(const QString& text, CsvTime format)
{
  switch (format) {
  case CsvTime::UnixSeconds:
    return toDouble(text);
  case CsvTime::UnixMilliseconds: {
    const std::optional<double> ms = toDouble(text);
    return ms ? std::optional<double>(*ms / 1000.0) : std::nullopt;
  }
  case CsvTime::Iso8601:
    break;
  }
  QDateTime time = QDateTime::fromString(text.trimmed(), Qt::ISODateWithMs);
  if (!time.isValid()) {
    return std::nullopt;
  }
  if (time.timeSpec() == Qt::LocalTime) {
    time = QDateTime(time.date(), time.time(), QTimeZone::utc());
  }
  return static_cast<double>(time.toMSecsSinceEpoch()) / 1000.0;
}

void readCsv(QIODevice& device, const AdsbReadOptions& options,
             const AircraftReportSink& sink, Progress& progress,
             AdsbReadResult& result)
{
  const CsvColumns& c = options.csv;
  if (!c.isComplete()) {
    result.error = QCoreApplication::translate(
        "ts::AdsbReader", "Choose the time, address, latitude and longitude "
                          "columns.");
    return;
  }
  LineReader lines(device);
  QByteArray line;
  if (c.header) {
    lines.next(line);
  }
  const int32_t needed =
      std::max({c.time, c.address, c.latitude, c.longitude, c.altitude,
                c.heading, c.speed, c.callsign, c.verticalRate}) +
      1;
  const std::function<QString(const QStringList&, int32_t)> field =
      [](const QStringList& fields, int32_t column) {
        return column >= 0 && column < fields.size() ? fields.at(column)
                                                     : QString();
      };
  while (lines.next(line) && progress.ok()) {
    if (line.trimmed().isEmpty()) {
      continue;
    }
    ++result.records;
    const QStringList fields =
        splitCsvLine(QString::fromUtf8(line), c.separator);
    if (fields.size() < needed) {
      result.addProblem(
          lines.line(),
          QCoreApplication::translate("ts::AdsbReader", "Too few columns."));
      continue;
    }
    const std::optional<double> time =
        parseCsvTime(field(fields, c.time), c.timeFormat);
    const std::optional<uint32_t> address =
        parseIcaoAddress(field(fields, c.address));
    const std::optional<double> lat = toDouble(field(fields, c.latitude));
    const std::optional<double> lon = toDouble(field(fields, c.longitude));
    if (!time) {
      result.addProblem(lines.line(),
                        QCoreApplication::translate("ts::AdsbReader",
                                                    "The time can't be read."));
      continue;
    }
    if (!address) {
      result.addProblem(lines.line(), QCoreApplication::translate(
                                          "ts::AdsbReader",
                                          "The ICAO address can't be read."));
      continue;
    }
    if (!lat || !lon || std::fabs(*lat) > 90.0) {
      result.addProblem(lines.line(),
                        QCoreApplication::translate(
                            "ts::AdsbReader", "The position can't be read."));
      continue;
    }
    AircraftReport report;
    report.timeS = *time;
    report.address = *address;
    report.position = CprPosition{*lat, normaliseLonDeg(*lon)};
    if (const std::optional<double> altitude =
            toDouble(field(fields, c.altitude))) {
      switch (c.altitudeKind) {
      case CsvAltitude::BaroFeet:
        report.baroAltitudeFt = *altitude;
        break;
      case CsvAltitude::BaroMetres:
        report.baroAltitudeFt = *altitude / feetToMetres;
        break;
      case CsvAltitude::GeometricFeet:
        report.geometricAltitudeFt = *altitude;
        break;
      case CsvAltitude::GeometricMetres:
        report.geometricAltitudeFt = *altitude / feetToMetres;
        break;
      }
    }
    report.trackDeg = toDouble(field(fields, c.heading));
    if (const std::optional<double> speed = toDouble(field(fields, c.speed))) {
      report.groundSpeedKt = c.speedUnit == CsvSpeed::Knots ? *speed
                             : c.speedUnit == CsvSpeed::MetresPerSecond
                                 ? *speed / knotsToMetresPerSecond
                                 : *speed / 3.6 / knotsToMetresPerSecond;
    }
    report.verticalRateFtMin = toDouble(field(fields, c.verticalRate));
    const QString callsign = field(fields, c.callsign).trimmed();
    if (!callsign.isEmpty()) {
      report.callsign = callsign;
    }
    ++result.reports;
    sink(report);
  }
}

} // namespace

void AdsbReadResult::addProblem(int64_t line, const QString& message)
{
  ++problemCount;
  if (static_cast<int64_t>(problems.size()) < maximumProblems) {
    problems.push_back(AdsbReadProblem{line, message});
  }
}

QString adsbFormatId(AdsbFormat format)
{
  switch (format) {
  case AdsbFormat::Unknown:
    break;
  case AdsbFormat::Sbs:
    return QStringLiteral("sbs");
  case AdsbFormat::Raw:
    return QStringLiteral("raw");
  case AdsbFormat::Beast:
    return QStringLiteral("beast");
  case AdsbFormat::ReadsbJson:
    return QStringLiteral("readsb-json");
  case AdsbFormat::Trace:
    return QStringLiteral("trace");
  case AdsbFormat::Csv:
    return QStringLiteral("csv");
  }
  return QString();
}

AdsbFormat adsbFormatFromId(const QString& id)
{
  for (const AdsbFormat format :
       {AdsbFormat::Sbs, AdsbFormat::Raw, AdsbFormat::Beast,
        AdsbFormat::ReadsbJson, AdsbFormat::Trace, AdsbFormat::Csv}) {
    if (adsbFormatId(format) == id) {
      return format;
    }
  }
  return AdsbFormat::Unknown;
}

bool CsvColumns::isComplete() const
{
  return time >= 0 && address >= 0 && latitude >= 0 && longitude >= 0;
}

QJsonObject CsvColumns::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("separator"), QString(separator));
  json.insert(QStringLiteral("header"), header);
  json.insert(QStringLiteral("time"), time);
  json.insert(QStringLiteral("address"), address);
  json.insert(QStringLiteral("latitude"), latitude);
  json.insert(QStringLiteral("longitude"), longitude);
  json.insert(QStringLiteral("altitude"), altitude);
  json.insert(QStringLiteral("heading"), heading);
  json.insert(QStringLiteral("speed"), speed);
  json.insert(QStringLiteral("callsign"), callsign);
  json.insert(QStringLiteral("verticalRate"), verticalRate);
  json.insert(QStringLiteral("timeFormat"), static_cast<int32_t>(timeFormat));
  json.insert(QStringLiteral("altitudeKind"),
              static_cast<int32_t>(altitudeKind));
  json.insert(QStringLiteral("speedUnit"), static_cast<int32_t>(speedUnit));
  return json;
}

CsvColumns CsvColumns::fromJson(const QJsonObject& json)
{
  CsvColumns c;
  const QString separator = json.value(QLatin1String("separator")).toString();
  if (separator.size() == 1) {
    c.separator = separator.at(0);
  }
  c.header = json.value(QLatin1String("header")).toBool(c.header);
  const std::function<int32_t(const char*, int32_t)> column =
      [&json](const char* name, int32_t fallback) {
        return json.value(QLatin1String(name)).toInt(fallback);
      };
  c.time = column("time", c.time);
  c.address = column("address", c.address);
  c.latitude = column("latitude", c.latitude);
  c.longitude = column("longitude", c.longitude);
  c.altitude = column("altitude", c.altitude);
  c.heading = column("heading", c.heading);
  c.speed = column("speed", c.speed);
  c.callsign = column("callsign", c.callsign);
  c.verticalRate = column("verticalRate", c.verticalRate);
  c.timeFormat =
      static_cast<CsvTime>(std::clamp(column("timeFormat", 0), 0, 2));
  c.altitudeKind =
      static_cast<CsvAltitude>(std::clamp(column("altitudeKind", 0), 0, 3));
  c.speedUnit = static_cast<CsvSpeed>(std::clamp(column("speedUnit", 0), 0, 2));
  return c;
}

QStringList splitCsvLine(const QString& line, QChar separator)
{
  QStringList fields;
  QString field;
  bool quoted = false;
  for (int32_t i = 0; i < line.size(); ++i) {
    const QChar c = line.at(i);
    if (quoted) {
      if (c == QLatin1Char('"')) {
        if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
          field.append(c);
          ++i;
        } else {
          quoted = false;
        }
      } else {
        field.append(c);
      }
    } else if (c == QLatin1Char('"')) {
      quoted = true;
    } else if (c == separator) {
      fields.append(field);
      field.clear();
    } else {
      field.append(c);
    }
  }
  fields.append(field);
  return fields;
}

CsvColumns guessCsvColumns(const QByteArray& head)
{
  CsvColumns c;
  const QByteArray first = firstLine(head);
  int32_t best = 0;
  for (const char separator : {',', ';', '\t', '|'}) {
    const int32_t count = static_cast<int32_t>(first.count(separator));
    if (count > best) {
      best = count;
      c.separator = QLatin1Char(separator);
    }
  }
  const QStringList names = splitCsvLine(QString::fromUtf8(first), c.separator);
  bool named = false;
  for (int32_t i = 0; i < names.size(); ++i) {
    const QString name = names.at(i).trimmed().toLower();
    const std::function<bool(std::initializer_list<const char*>)> is =
        [&name](std::initializer_list<const char*> options) {
          return std::any_of(options.begin(), options.end(),
                             [&name](const char* option) {
                               return name == QLatin1String(option);
                             });
        };
    int32_t* target = nullptr;
    if (is({"time", "timestamp", "ts", "datetime", "date_time", "unixtime",
            "time_s", "time_ms"})) {
      target = &c.time;
      if (name.endsWith(QLatin1String("ms"))) {
        c.timeFormat = CsvTime::UnixMilliseconds;
      }
    } else if (is({"icao", "icao24", "hex", "address", "icao_address", "modes",
                   "mode_s"})) {
      target = &c.address;
    } else if (is({"lat", "latitude"})) {
      target = &c.latitude;
    } else if (is({"lon", "lng", "long", "longitude"})) {
      target = &c.longitude;
    } else if (is({"alt", "altitude", "baroaltitude", "alt_baro", "altitude_ft",
                   "alt_ft", "baro_altitude"})) {
      target = &c.altitude;
    } else if (is({"geoaltitude", "alt_geom", "geo_altitude"})) {
      if (c.altitude < 0) {
        target = &c.altitude;
        c.altitudeKind = CsvAltitude::GeometricFeet;
      }
    } else if (is({"heading", "track", "trk", "hdg"})) {
      target = &c.heading;
    } else if (is({"speed", "gs", "gs_kt", "speed_kt", "groundspeed",
                   "ground_speed", "velocity"})) {
      target = &c.speed;
      if (name == QLatin1String("velocity")) {
        c.speedUnit = CsvSpeed::MetresPerSecond; // OpenSky's state vectors
      }
    } else if (is({"callsign", "flight", "call_sign", "ident"})) {
      target = &c.callsign;
    } else if (is({"vertical_rate", "vertrate", "vr", "vr_ftmin", "baro_rate",
                   "verticalrate"})) {
      target = &c.verticalRate;
    }
    if (target != nullptr && *target < 0) {
      *target = i;
      named = true;
    }
  }
  c.header = named;
  // OpenSky's state vectors give altitudes in metres.
  if (names.contains(QStringLiteral("baroaltitude")) && c.altitude >= 0 &&
      c.altitudeKind == CsvAltitude::BaroFeet) {
    c.altitudeKind = CsvAltitude::BaroMetres;
  }
  if (names.contains(QStringLiteral("geoaltitude")) &&
      c.altitudeKind == CsvAltitude::GeometricFeet) {
    c.altitudeKind = CsvAltitude::GeometricMetres;
  }
  return c;
}

AdsbFormat detectAdsbFormat(const QByteArray& head)
{
  if (head.size() >= 2 && head.at(0) == '\x1a' && head.at(1) >= '1' &&
      head.at(1) <= '4') {
    return AdsbFormat::Beast;
  }
  const QByteArray text = head.trimmed();
  if (text.startsWith('{') || text.startsWith('[')) {
    if (text.contains("\"trace\"")) {
      return AdsbFormat::Trace;
    }
    if (text.contains("\"aircraft\"") || text.contains("\"ac\"")) {
      return AdsbFormat::ReadsbJson;
    }
    return AdsbFormat::Unknown;
  }
  const QByteArray first = firstLine(text);
  for (const char* type : {"MSG,", "SEL,", "ID,", "AIR,", "STA,", "CLK,"}) {
    if (first.startsWith(type)) {
      return AdsbFormat::Sbs;
    }
  }
  if (parseRawLine(first)) {
    return AdsbFormat::Raw;
  }
  if (first.contains(',') || first.contains(';') || first.contains('\t')) {
    return AdsbFormat::Csv;
  }
  return AdsbFormat::Unknown;
}

QByteArray gunzipIfCompressed(const QByteArray& data, bool* ok)
{
  if (ok != nullptr) {
    *ok = true;
  }
  if (data.size() < 2 || static_cast<uint8_t>(data.at(0)) != 0x1FU ||
      static_cast<uint8_t>(data.at(1)) != 0x8BU) {
    return data;
  }
  static std::atomic<int64_t> counter{0};
  const QByteArray name = QByteArrayLiteral("/vsimem/ts-adsb-") +
                          QByteArray::number(static_cast<qint64>(++counter)) +
                          QByteArrayLiteral(".gz");
  // GDAL reads the buffer in place (not owned, not written).
  VSILFILE* memory = VSIFileFromMemBuffer(
      name.constData(),
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      reinterpret_cast<GByte*>(const_cast<char*>(data.constData())),
      static_cast<vsi_l_offset>(data.size()), FALSE);
  if (memory == nullptr) {
    if (ok != nullptr) {
      *ok = false;
    }
    return QByteArray();
  }
  VSIFCloseL(memory);
  const QByteArray gzip = QByteArrayLiteral("/vsigzip/") + name;
  VSILFILE* file = VSIFOpenL(gzip.constData(), "rb");
  QByteArray out;
  bool good = file != nullptr;
  if (file != nullptr) {
    std::array<char, 65536> buffer{};
    size_t read = 0;
    while ((read = VSIFReadL(buffer.data(), 1, buffer.size(), file)) > 0) {
      out.append(buffer.data(), static_cast<int32_t>(read));
    }
    good = VSIFEofL(file) != 0;
    VSIFCloseL(file);
  }
  VSIUnlink(name.constData());
  if (ok != nullptr) {
    *ok = good;
  }
  return good ? out : QByteArray();
}

AdsbReadResult readAdsb(QIODevice& device, AdsbFormat format,
                        const AdsbReadOptions& options,
                        const AircraftReportSink& sink,
                        const std::function<bool(qint64)>& progress)
{
  AdsbReadResult result;
  if (format == AdsbFormat::Unknown) {
    format = detectAdsbFormat(device.peek(8192));
  }
  result.format = format;
  Progress steps(device, progress);
  switch (format) {
  case AdsbFormat::Unknown:
    result.error = QCoreApplication::translate(
        "ts::AdsbReader", "The file's format isn't one Terra Soar reads.");
    break;
  case AdsbFormat::Sbs:
    readSbs(device, options, sink, steps, result);
    break;
  case AdsbFormat::Raw:
    readRaw(device, options, sink, steps, result);
    break;
  case AdsbFormat::Beast:
    readBeast(device, options, sink, steps, result);
    break;
  case AdsbFormat::ReadsbJson:
    readReadsb(device, sink, result);
    break;
  case AdsbFormat::Trace:
    readTrace(device, sink, result);
    break;
  case AdsbFormat::Csv:
    readCsv(device, options, sink, steps, result);
    break;
  }
  if (result.error.isEmpty() && result.reports == 0 && result.records == 0) {
    result.error = QCoreApplication::translate(
        "ts::AdsbReader", "The file has no aircraft data.");
  }
  return result;
}

AdsbReadResult readAdsbData(const QByteArray& data, AdsbFormat format,
                            const AdsbReadOptions& options,
                            const AircraftReportSink& sink)
{
  bool ok = false;
  QByteArray plain = gunzipIfCompressed(data, &ok);
  if (!ok) {
    AdsbReadResult result;
    result.error = QCoreApplication::translate(
        "ts::AdsbReader", "The compressed data can't be read.");
    return result;
  }
  QBuffer buffer(&plain);
  buffer.open(QIODevice::ReadOnly);
  return readAdsb(buffer, format, options, sink);
}

AdsbReadResult readAdsbFile(const QString& path, AdsbFormat format,
                            const AdsbReadOptions& options,
                            const AircraftReportSink& sink,
                            const std::function<bool(qint64)>& progress)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    AdsbReadResult result;
    result.error = file.errorString();
    return result;
  }
  const QByteArray magic = file.peek(2);
  if (magic.size() == 2 && static_cast<uint8_t>(magic.at(0)) == 0x1FU &&
      static_cast<uint8_t>(magic.at(1)) == 0x8BU) {
    return readAdsbData(file.readAll(), format, options, sink);
  }
  return readAdsb(file, format, options, sink, progress);
}

std::optional<AircraftReport> SbsParser::parse(const QByteArray& line,
                                               QString* error) const
{
  const QList<QByteArray> f = line.trimmed().split(',');
  const std::function<std::optional<AircraftReport>(const QString&)> fail =
      [error](const QString& message) {
        if (error != nullptr) {
          *error = message;
        }
        return std::optional<AircraftReport>();
      };
  if (f.size() < 10) {
    return fail(
        QCoreApplication::translate("ts::AdsbReader", "Not an SBS line."));
  }
  const QByteArray type = f.at(0);
  if (type != "MSG") {
    if (type == "SEL" || type == "ID" || type == "AIR" || type == "STA" ||
        type == "CLK") {
      return std::nullopt;
    }
    return fail(
        QCoreApplication::translate("ts::AdsbReader", "Not an SBS line."));
  }
  if (f.size() < 22) {
    return fail(QCoreApplication::translate(
        "ts::AdsbReader", "An SBS line with too few fields."));
  }
  AircraftReport report;
  const std::optional<uint32_t> address =
      parseIcaoAddress(QString::fromLatin1(f.at(4)));
  if (!address) {
    return fail(QCoreApplication::translate("ts::AdsbReader",
                                            "The ICAO address can't be read."));
  }
  report.address = *address;
  const QDate date = QDate::fromString(QString::fromLatin1(f.at(6)),
                                       QStringLiteral("yyyy/MM/dd"));
  QString timeText = QString::fromLatin1(f.at(7));
  const QTime time =
      QTime::fromString(timeText, timeText.contains(QLatin1Char('.'))
                                      ? QStringLiteral("HH:mm:ss.zzz")
                                      : QStringLiteral("HH:mm:ss"));
  if (!date.isValid() || !time.isValid()) {
    return fail(QCoreApplication::translate("ts::AdsbReader",
                                            "The date or time can't be read."));
  }
  const QDateTime when = m_localTime ? QDateTime(date, time)
                                     : QDateTime(date, time, QTimeZone::utc());
  report.timeS = static_cast<double>(when.toMSecsSinceEpoch()) / 1000.0;
  const QString callsign = QString::fromLatin1(f.at(10)).trimmed();
  if (!callsign.isEmpty()) {
    report.callsign = callsign;
  }
  const std::function<std::optional<double>(int32_t)> number = [&f](int32_t i) {
    return toDouble(QString::fromLatin1(f.at(i)));
  };
  report.baroAltitudeFt = number(11);
  report.groundSpeedKt = number(12);
  report.trackDeg = number(13);
  const std::optional<double> lat = number(14);
  const std::optional<double> lon = number(15);
  if (lat && lon && std::fabs(*lat) <= 90.0) {
    report.position = CprPosition{*lat, normaliseLonDeg(*lon)};
  }
  report.verticalRateFtMin = number(16);
  const QString squawk = QString::fromLatin1(f.at(17)).trimmed();
  if (!squawk.isEmpty()) {
    report.squawk = squawk;
    report.emergency = emergencyFromSquawk(squawk);
  }
  const QByteArray emergency = f.at(19).trimmed();
  if ((emergency == "-1" || emergency == "1") && !report.emergency) {
    report.emergency = AdsbEmergency::General;
  }
  const QByteArray ground = f.at(21).trimmed();
  if (ground == "-1" || ground == "1") {
    report.onGround = true;
  } else if (ground == "0") {
    report.onGround = false;
  }
  return report;
}

std::optional<TimedMessage> parseRawLine(const QByteArray& line, QString* error)
{
  const QByteArray text = line.trimmed();
  const std::function<std::optional<TimedMessage>()> fail = [error, &text]() {
    if (error != nullptr && !text.isEmpty()) {
      *error = QCoreApplication::translate("ts::AdsbReader",
                                           "Not a Mode S message.");
    }
    return std::optional<TimedMessage>();
  };
  if (text.isEmpty()) {
    return fail();
  }
  TimedMessage message;
  QByteArray hex;
  if (text.startsWith('*') || text.startsWith('@')) {
    if (!text.endsWith(';')) {
      return fail();
    }
    hex = text.mid(1, text.size() - 2);
    if (text.startsWith('@')) {
      if (hex.size() < 12 || !isHex(hex.left(12))) {
        return fail();
      }
      bool ok = false;
      message.counter = hex.left(12).toULongLong(&ok, 16);
      hex = hex.mid(12);
    }
  } else {
    // "TIME,HEX", "TIME HEX" or "TIME *HEX;".
    int32_t split = static_cast<int32_t>(text.indexOf(','));
    if (split < 0) {
      split = static_cast<int32_t>(text.indexOf(' '));
    }
    if (split <= 0) {
      return fail();
    }
    const std::optional<double> time =
        toDouble(QString::fromLatin1(text.left(split)));
    if (!time) {
      return fail();
    }
    message.timeS = *time;
    hex = text.mid(split + 1).trimmed();
    if (hex.startsWith('*') && hex.endsWith(';')) {
      hex = hex.mid(1, hex.size() - 2);
    }
  }
  if (hex.size() == 4 && isHex(hex)) {
    // A Mode A/C reply: not used.
    return std::nullopt;
  }
  message.bytes = modeSFromHex(hex);
  if (message.bytes.isEmpty()) {
    return fail();
  }
  return message;
}

std::vector<TimedMessage> BeastParser::feed(const QByteArray& data)
{
  m_pending.append(data);
  std::vector<TimedMessage> messages;
  qsizetype i = 0;
  const qsizetype n = m_pending.size();
  while (i < n) {
    if (m_pending.at(i) != '\x1a') {
      ++i;
      ++m_skipped;
      continue;
    }
    if (i + 1 >= n) {
      break;
    }
    const char type = m_pending.at(i + 1);
    qsizetype length = 0;
    switch (type) {
    case '1':
      length = 2;
      break;
    case '2':
      length = 7;
      break;
    case '3':
    case '4':
      length = 14;
      break;
    default:
      // Not a frame start (or a doubled 0x1a out of step): skip it.
      i += 1;
      ++m_skipped;
      continue;
    }
    // Unescape 6 + 1 + length bytes.
    QByteArray frame;
    frame.reserve(7 + length);
    qsizetype j = i + 2;
    bool broken = false;
    while (frame.size() < 7 + length && j < n) {
      const char c = m_pending.at(j);
      if (c == '\x1a') {
        if (j + 1 >= n) {
          break;
        }
        if (m_pending.at(j + 1) != '\x1a') {
          broken = true; // a new frame started early
          break;
        }
        ++j;
      }
      frame.append(c);
      ++j;
    }
    if (broken) {
      m_skipped += j - i;
      i = j;
      continue;
    }
    if (frame.size() < 7 + length) {
      break; // more data needed
    }
    if (type == '2' || type == '3') {
      TimedMessage message;
      uint64_t counter = 0;
      for (qsizetype k = 0; k < 6; ++k) {
        counter = (counter << 8U) | static_cast<uint8_t>(frame.at(k));
      }
      message.counter = counter;
      message.bytes = frame.mid(7);
      messages.push_back(std::move(message));
    }
    i = j;
  }
  m_pending.remove(0, i);
  return messages;
}

double ReceiverClock::timeOf(uint64_t counter)
{
  if (!m_first) {
    m_first = counter;
    m_last = counter;
  }
  if (counter + (uint64_t{1} << 40U) < m_last) {
    // Wrapped at 2⁴⁸ (a big step back, not reordering).
    m_wraps += 1.0;
  }
  m_last = counter;
  const double ticks = static_cast<double>(counter) -
                       static_cast<double>(*m_first) + m_wraps * counterRange;
  return m_startTimeS + ticks / beastTicksPerSecond;
}

std::optional<double> readReadsbSnapshot(const QJsonObject& snapshot,
                                         const AircraftReportSink& sink,
                                         QString* error)
{
  std::optional<double> now = jsonNumber(snapshot.value(QLatin1String("now")));
  QJsonValue list = snapshot.value(QLatin1String("aircraft"));
  if (!list.isArray()) {
    list = snapshot.value(QLatin1String("ac"));
  }
  if (!now || !list.isArray()) {
    if (error != nullptr) {
      *error = QCoreApplication::translate(
          "ts::AdsbReader",
          "Not an aircraft.json snapshot: no \"now\" or aircraft.");
    }
    return std::nullopt;
  }
  // airplanes.live and adsb.lol give milliseconds.
  if (*now > 1.0e11) {
    *now /= 1000.0;
  }
  for (const QJsonValue& item : list.toArray()) {
    const QJsonObject ac = item.toObject();
    const std::optional<uint32_t> address =
        parseIcaoAddress(ac.value(QLatin1String("hex")).toString());
    if (!address) {
      continue; // non-ICAO ("~…") or broken
    }
    const double seen =
        jsonNumber(ac.value(QLatin1String("seen"))).value_or(0.0);
    AircraftReport report;
    report.address = *address;
    report.timeS = *now - seen;
    readAircraftObject(ac, report);
    const std::optional<double> lat =
        jsonNumber(ac.value(QLatin1String("lat")));
    const std::optional<double> lon =
        jsonNumber(ac.value(QLatin1String("lon")));
    const std::optional<double> seenPos =
        jsonNumber(ac.value(QLatin1String("seen_pos")));
    if (lat && lon && std::fabs(*lat) <= 90.0) {
      AircraftReport position = report;
      position.timeS = *now - seenPos.value_or(seen);
      position.position = CprPosition{*lat, normaliseLonDeg(*lon)};
      if (position.timeS == report.timeS) {
        sink(position);
        continue;
      }
      // The position is older than the rest: send it on its own first,
      // then the rest.
      position.groundSpeedKt.reset();
      position.trackDeg.reset();
      position.verticalRateFtMin.reset();
      sink(position);
    }
    if (report.hasData()) {
      sink(report);
    }
  }
  return *now;
}

} // namespace ts
