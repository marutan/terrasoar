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
#include "ts/ais/aisreaders.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QStringList>
#include <QTimeZone>

#include "ts/adsb/adsbreaders.h"
#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

std::optional<double> toNumber(const QString& text)
{
  bool ok = false;
  const double value = text.trimmed().toDouble(&ok);
  return ok && std::isfinite(value) ? std::optional<double>(value)
                                    : std::nullopt;
}

/**
 * Reads a time: Unix seconds (milliseconds if very large) or ISO 8601
 * (with a space or a T; UTC unless an offset is given).
 */
std::optional<double> parseTime(QString text)
{
  text = text.trimmed();
  if (text.isEmpty()) {
    return std::nullopt;
  }
  const std::optional<double> number = toNumber(text);
  if (number) {
    return *number > 1.0e11 ? *number / 1000.0 : *number;
  }
  if (text.size() > 10 && text.at(10) == QLatin1Char(' ')) {
    text[10] = QLatin1Char('T');
  }
  QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
  if (!time.isValid()) {
    time = QDateTime::fromString(text, Qt::ISODate);
  }
  if (!time.isValid()) {
    return std::nullopt;
  }
  if (time.timeSpec() == Qt::LocalTime) {
    time = QDateTime(time.date(), time.time(), QTimeZone::utc());
  }
  return static_cast<double>(time.toMSecsSinceEpoch()) / 1000.0;
}

/** Reads lines from a device, counting them. */
class Lines
{
public:
  explicit Lines(QIODevice& device) : m_device(&device) {}

  bool next(QByteArray& line)
  {
    if (m_device->atEnd()) {
      return false;
    }
    line = m_device->readLine().trimmed();
    ++m_line;
    return true;
  }

  int64_t line() const { return m_line; }

  bool keepGoing(const std::function<bool(qint64)>& progress) const
  {
    return !progress || m_line % 4096 != 0 || progress(m_device->pos());
  }

private:
  QIODevice* m_device;
  int64_t m_line = 0;
};

/** The parts of a log line: a time before, the sentence, a time after. */
struct SplitLine {
  QByteArray sentence;
  std::optional<double> timeS;
};

SplitLine splitLine(const QByteArray& line)
{
  SplitLine split;
  const int32_t start = static_cast<int32_t>(line.indexOf('!'));
  if (line.startsWith('\\') || start < 0) {
    split.sentence = line;
    return split;
  }
  QByteArray before = line.left(start).trimmed();
  while (!before.isEmpty() && (before.endsWith(',') || before.endsWith(';') ||
                               before.endsWith(':'))) {
    before.chop(1);
  }
  if (!before.isEmpty()) {
    split.timeS = parseTime(QString::fromLatin1(before));
  }
  const int32_t star = static_cast<int32_t>(line.indexOf('*', start));
  if (star < 0 || star + 3 > line.size()) {
    split.sentence = line.mid(start);
    return split;
  }
  split.sentence = line.mid(start, star + 3 - start);
  QByteArray after = line.mid(star + 3).trimmed();
  if (after.startsWith(',') || after.startsWith(';')) {
    after = after.mid(1);
  }
  if (!split.timeS && !after.isEmpty()) {
    split.timeS = parseTime(QString::fromLatin1(after));
  }
  return split;
}

void readNmea(QIODevice& device, const AisReadOptions& options,
              const VesselReportSink& sink,
              const std::function<bool(qint64)>& progress,
              AisReadResult& result)
{
  NmeaAisParser parser;
  Lines lines(device);
  QByteArray line;
  std::optional<double> lastTime;
  int64_t sentences = 0;
  while (lines.next(line) && lines.keepGoing(progress)) {
    if (line.isEmpty()) {
      continue;
    }
    ++result.records;
    // Without times: a second a sentence from the start, in order; with
    // some, the last time read stands for lines without one.
    const double fallback =
        lastTime ? *lastTime
                 : options.startTimeS + static_cast<double>(sentences);
    const NmeaAisParser::Result read = parser.parse(line, fallback);
    if (read.error == AisSentenceError::Checksum) {
      ++result.checksumErrors;
      continue;
    }
    if (read.error != AisSentenceError::None) {
      result.addProblem(lines.line(),
                        read.error == AisSentenceError::NotAis
                            ? QCoreApplication::translate(
                                  "ts::AisReader", "Not an AIS sentence.")
                            : QCoreApplication::translate(
                                  "ts::AisReader", "A broken AIS sentence."));
      continue;
    }
    ++sentences;
    if (read.timeS) {
      lastTime = read.timeS;
    }
    if (read.otherType != 0) {
      ++result.otherMessages[read.otherType];
    }
    if (read.report) {
      ++result.reports;
      sink(*read.report);
    }
  }
  result.untimed = sentences > 0 && !lastTime;
  result.incomplete = parser.incomplete();
}

/** The field in @p column of @p fields, or empty. */
QString csvField(const QStringList& fields, int32_t column)
{
  return column >= 0 && column < fields.size() ? fields.at(column).trimmed()
                                               : QString();
}

/** Length and width as dimensions, the antenna in the middle. */
std::optional<AisDimensions> dimensionsOf(std::optional<double> length,
                                          std::optional<double> width)
{
  if (!length || !width || *length <= 0.0 || *width <= 0.0) {
    return std::nullopt;
  }
  AisDimensions d;
  d.toBow = static_cast<int32_t>(std::lround(*length / 2.0));
  d.toStern = static_cast<int32_t>(std::lround(*length)) - d.toBow;
  d.toPort = static_cast<int32_t>(std::lround(*width / 2.0));
  d.toStarboard = static_cast<int32_t>(std::lround(*width)) - d.toPort;
  return d;
}

/** The static fields of a CSV row into @p report. */
void readCsvStatic(const QStringList& f, const AisCsvColumns& c,
                   VesselReport& report)
{
  const QString name = csvField(f, c.name);
  if (!name.isEmpty()) {
    report.name = name;
  }
  const QString callsign = csvField(f, c.callsign);
  if (!callsign.isEmpty()) {
    report.callsign = callsign;
  }
  QString imo = csvField(f, c.imo);
  if (imo.startsWith(QLatin1String("IMO"), Qt::CaseInsensitive)) {
    imo = imo.mid(3);
  }
  bool ok = false;
  const uint32_t imoNumber = imo.toUInt(&ok);
  if (ok && imoNumber != 0) {
    report.imo = imoNumber;
  }
  const std::optional<double> type = toNumber(csvField(f, c.shipType));
  if (type && *type > 0.0 && *type < 100.0) {
    report.shipType = static_cast<int32_t>(*type);
  }
  const std::optional<double> status = toNumber(csvField(f, c.status));
  if (status && *status >= 0.0 && *status <= 15.0) {
    report.status = static_cast<AisNavStatus>(static_cast<int32_t>(*status));
  }
  report.dimensions = dimensionsOf(toNumber(csvField(f, c.length)),
                                   toNumber(csvField(f, c.width)));
  const std::optional<double> draught = toNumber(csvField(f, c.draught));
  if (draught && *draught > 0.0) {
    report.draughtM = draught;
  }
  const QString transceiver = csvField(f, c.transceiver);
  if (!transceiver.isEmpty()) {
    report.classB =
        transceiver.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0;
  }
}

/** One CSV row as a report, or nothing and why. */
std::optional<VesselReport> csvReport(const QStringList& f,
                                      const AisCsvColumns& c, QString& error)
{
  const std::optional<double> time = parseTime(csvField(f, c.time));
  bool mmsiOk = false;
  const uint32_t mmsi = csvField(f, c.mmsi).toUInt(&mmsiOk);
  const std::optional<double> lat = toNumber(csvField(f, c.latitude));
  const std::optional<double> lon = toNumber(csvField(f, c.longitude));
  if (!time) {
    error =
        QCoreApplication::translate("ts::AisReader", "The time can't be read.");
    return std::nullopt;
  }
  if (!mmsiOk || mmsi == 0) {
    error =
        QCoreApplication::translate("ts::AisReader", "The MMSI can't be read.");
    return std::nullopt;
  }
  if (!lat || !lon || std::fabs(*lat) > 90.0 || std::fabs(*lon) > 180.0) {
    error = QCoreApplication::translate("ts::AisReader",
                                        "The position can't be read.");
    return std::nullopt;
  }
  VesselReport report;
  report.timeS = *time;
  report.mmsi = mmsi;
  report.latDeg = lat;
  report.lonDeg = normaliseLonDeg(*lon);
  const std::optional<double> speed = toNumber(csvField(f, c.speed));
  if (speed && *speed >= 0.0 && *speed < 102.2) {
    report.speedKn = speed;
  }
  const std::optional<double> course = toNumber(csvField(f, c.course));
  if (course && *course >= 0.0 && *course < 360.0) {
    report.courseDeg = course;
  }
  const std::optional<double> heading = toNumber(csvField(f, c.heading));
  if (heading && *heading >= 0.0 && *heading < 360.0) {
    report.headingDeg = heading;
  }
  readCsvStatic(f, c, report);
  return report;
}

void readCsv(QIODevice& device, const AisReadOptions& options,
             const VesselReportSink& sink,
             const std::function<bool(qint64)>& progress, AisReadResult& result)
{
  const AisCsvColumns& c = options.csv;
  if (!c.isComplete()) {
    result.error = QCoreApplication::translate(
        "ts::AisReader",
        "Choose the time, MMSI, latitude and longitude columns.");
    return;
  }
  Lines lines(device);
  QByteArray line;
  if (c.header) {
    lines.next(line);
  }
  while (lines.next(line) && lines.keepGoing(progress)) {
    if (line.isEmpty()) {
      continue;
    }
    ++result.records;
    QString error;
    const std::optional<VesselReport> report =
        csvReport(splitCsvLine(QString::fromUtf8(line), c.separator), c, error);
    if (!report) {
      result.addProblem(lines.line(), error);
      continue;
    }
    ++result.reports;
    sink(*report);
  }
}

/** A column and the header names that mean it. */
struct ColumnNames {
  int32_t AisCsvColumns::*column;
  std::vector<const char*> names;
};

int32_t columnNamed(const QStringList& names,
                    const std::vector<const char*>& options)
{
  for (int32_t i = 0; i < names.size(); ++i) {
    for (const char* option : options) {
      if (names.at(i) == QLatin1String(option)) {
        return i;
      }
    }
  }
  return -1;
}

} // namespace

void AisReadResult::addProblem(int64_t line, const QString& message)
{
  ++problemCount;
  if (static_cast<int64_t>(problems.size()) < maximumProblems) {
    problems.push_back(AisReadProblem{line, message});
  }
}

QString aisFormatId(AisFormat format)
{
  switch (format) {
  case AisFormat::Unknown:
    break;
  case AisFormat::Nmea:
    return QStringLiteral("nmea");
  case AisFormat::Csv:
    return QStringLiteral("csv");
  }
  return QString();
}

AisFormat aisFormatFromId(const QString& id)
{
  if (id == QLatin1String("nmea")) {
    return AisFormat::Nmea;
  }
  if (id == QLatin1String("csv")) {
    return AisFormat::Csv;
  }
  return AisFormat::Unknown;
}

bool AisCsvColumns::isComplete() const
{
  return time >= 0 && mmsi >= 0 && latitude >= 0 && longitude >= 0;
}

namespace
{

/** The columns, with their JSON names. */
const std::vector<std::pair<const char*, int32_t AisCsvColumns::*>>&
columnMembers()
{
  static const std::vector<std::pair<const char*, int32_t AisCsvColumns::*>>
      members = {
          {"time", &AisCsvColumns::time},
          {"mmsi", &AisCsvColumns::mmsi},
          {"latitude", &AisCsvColumns::latitude},
          {"longitude", &AisCsvColumns::longitude},
          {"speed", &AisCsvColumns::speed},
          {"course", &AisCsvColumns::course},
          {"heading", &AisCsvColumns::heading},
          {"name", &AisCsvColumns::name},
          {"callsign", &AisCsvColumns::callsign},
          {"imo", &AisCsvColumns::imo},
          {"shipType", &AisCsvColumns::shipType},
          {"status", &AisCsvColumns::status},
          {"length", &AisCsvColumns::length},
          {"width", &AisCsvColumns::width},
          {"draught", &AisCsvColumns::draught},
          {"transceiver", &AisCsvColumns::transceiver},
      };
  return members;
}

} // namespace

QJsonObject AisCsvColumns::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("separator"), QString(separator));
  json.insert(QStringLiteral("header"), header);
  for (const std::pair<const char*, int32_t AisCsvColumns::*>& member :
       columnMembers()) {
    json.insert(QLatin1String(member.first), this->*member.second);
  }
  return json;
}

AisCsvColumns AisCsvColumns::fromJson(const QJsonObject& json)
{
  AisCsvColumns c;
  const QString separator = json.value(QLatin1String("separator")).toString();
  if (separator.size() == 1) {
    c.separator = separator.at(0);
  }
  c.header = json.value(QLatin1String("header")).toBool(c.header);
  for (const std::pair<const char*, int32_t AisCsvColumns::*>& member :
       columnMembers()) {
    c.*member.second =
        json.value(QLatin1String(member.first)).toInt(c.*member.second);
  }
  return c;
}

AisCsvColumns guessAisCsvColumns(const QByteArray& head)
{
  AisCsvColumns c;
  const int32_t end = static_cast<int32_t>(head.indexOf('\n'));
  const QByteArray first = (end < 0 ? head : head.left(end)).trimmed();
  int32_t best = 0;
  for (const char separator : {',', ';', '\t', '|'}) {
    const int32_t count = static_cast<int32_t>(first.count(separator));
    if (count > best) {
      best = count;
      c.separator = QLatin1Char(separator);
    }
  }
  QStringList names = splitCsvLine(QString::fromUtf8(first), c.separator);
  for (QString& name : names) {
    name = name.trimmed().toLower();
  }
  static const std::vector<ColumnNames> table = {
      {&AisCsvColumns::time,
       {"basedatetime", "time", "timestamp", "datetime", "date_time", "ts",
        "time_utc"}},
      {&AisCsvColumns::mmsi, {"mmsi"}},
      {&AisCsvColumns::latitude, {"lat", "latitude"}},
      {&AisCsvColumns::longitude, {"lon", "lng", "long", "longitude"}},
      {&AisCsvColumns::speed, {"sog", "speed", "speed_kn"}},
      {&AisCsvColumns::course, {"cog", "course"}},
      {&AisCsvColumns::heading, {"heading", "hdg", "true_heading"}},
      {&AisCsvColumns::name, {"vesselname", "name", "shipname", "vessel_name"}},
      {&AisCsvColumns::callsign, {"callsign", "call_sign"}},
      {&AisCsvColumns::imo, {"imo"}},
      {&AisCsvColumns::shipType,
       {"vesseltype", "shiptype", "ship_type", "type"}},
      {&AisCsvColumns::status, {"status", "navstatus", "nav_status"}},
      {&AisCsvColumns::length, {"length"}},
      {&AisCsvColumns::width, {"width", "beam"}},
      {&AisCsvColumns::draught, {"draft", "draught"}},
      {&AisCsvColumns::transceiver, {"transceiverclass", "class"}},
  };
  bool named = false;
  for (const ColumnNames& entry : table) {
    c.*entry.column = columnNamed(names, entry.names);
    named = named || c.*entry.column >= 0;
  }
  c.header = named;
  return c;
}

NmeaAisParser::Result NmeaAisParser::parse(const QByteArray& line,
                                           double fallbackS)
{
  Result result;
  const SplitLine split = splitLine(line.trimmed());
  const std::optional<AisSentence> sentence =
      parseAisSentence(split.sentence, &result.error);
  if (!sentence) {
    return result;
  }
  result.timeS = sentence->tagTimeS ? sentence->tagTimeS : split.timeS;
  const std::optional<AisAssembler::Payload> whole = m_assembler.add(*sentence);
  if (!whole) {
    return result;
  }
  int32_t type = 0;
  const std::optional<AisMessage> message =
      decodeAis(whole->payload, whole->fillBits, &type);
  if (!message) {
    result.otherType = type;
    return result;
  }
  const double timeS =
      whole->tagTimeS.value_or(result.timeS.value_or(fallbackS));
  result.report = vesselReport(*message, timeS);
  return result;
}

AisFormat detectAisFormat(const QByteArray& head)
{
  if (head.contains("VDM,") || head.contains("VDO,")) {
    return AisFormat::Nmea;
  }
  const int32_t end = static_cast<int32_t>(head.indexOf('\n'));
  const QByteArray first = (end < 0 ? head : head.left(end)).toLower();
  if (first.contains("mmsi") &&
      (first.contains(',') || first.contains(';') || first.contains('\t'))) {
    return AisFormat::Csv;
  }
  return AisFormat::Unknown;
}

AisReadResult readAis(QIODevice& device, AisFormat format,
                      const AisReadOptions& options,
                      const VesselReportSink& sink,
                      const std::function<bool(qint64)>& progress)
{
  AisReadResult result;
  if (format == AisFormat::Unknown) {
    format = detectAisFormat(device.peek(8192));
  }
  result.format = format;
  switch (format) {
  case AisFormat::Unknown:
    result.error = QCoreApplication::translate(
        "ts::AisReader", "The file's format isn't one Terra Soar reads.");
    return result;
  case AisFormat::Nmea:
    readNmea(device, options, sink, progress, result);
    break;
  case AisFormat::Csv:
    readCsv(device, options, sink, progress, result);
    break;
  }
  if (result.error.isEmpty() && result.records == 0) {
    result.error = QCoreApplication::translate("ts::AisReader",
                                               "The file has no AIS data.");
  }
  return result;
}

AisReadResult readAisData(const QByteArray& data, AisFormat format,
                          const AisReadOptions& options,
                          const VesselReportSink& sink)
{
  bool ok = false;
  QByteArray plain = gunzipIfCompressed(data, &ok);
  if (!ok) {
    AisReadResult result;
    result.error = QCoreApplication::translate(
        "ts::AisReader", "The compressed data can't be read.");
    return result;
  }
  QBuffer buffer(&plain);
  buffer.open(QIODevice::ReadOnly);
  return readAis(buffer, format, options, sink);
}

AisReadResult readAisFile(const QString& path, AisFormat format,
                          const AisReadOptions& options,
                          const VesselReportSink& sink,
                          const std::function<bool(qint64)>& progress)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    AisReadResult result;
    result.error = file.errorString();
    return result;
  }
  const QByteArray magic = file.peek(2);
  if (magic.size() == 2 && static_cast<uint8_t>(magic.at(0)) == 0x1FU &&
      static_cast<uint8_t>(magic.at(1)) == 0x8BU) {
    return readAisData(file.readAll(), format, options, sink);
  }
  return readAis(file, format, options, sink, progress);
}

} // namespace ts
