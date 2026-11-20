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
#include "ts/orbits/orbitalelements.h"

#include <cmath>
#include <cstdint>
#include <utility>

#include <QCoreApplication>
#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringView>
#include <QTime>
#include <QTimeZone>

namespace ts
{

namespace
{

constexpr double msPerDay = 86400000.0;

/** The Julian date of 0h UTC on 1 January of @p year. */
double januaryFirstJd(int32_t year)
{
  // Vallado's jday() for 1 January (valid 1900–2100).
  return 367.0 * year -
         std::floor(7.0 * (year + std::floor(10.0 / 12.0)) * 0.25) +
         std::floor(275.0 / 9.0) + 1.0 + 1721013.5;
}

/** Characters @p from to @p to (exclusive) of @p line. */
QStringView columns(const QString& line, int32_t from, int32_t to)
{
  return QStringView(line).mid(from, to - from);
}

std::optional<double> number(QStringView text)
{
  bool ok = false;
  const double value = text.trimmed().toDouble(&ok);
  return ok ? std::optional<double>(value) : std::nullopt;
}

/** An integer field; blank is 0. */
std::optional<int32_t> integer(QStringView text)
{
  const QStringView trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return 0;
  }
  bool ok = false;
  const int32_t value = trimmed.toInt(&ok);
  return ok ? std::optional<int32_t>(value) : std::nullopt;
}

/**
 * A number in the TLE's exponent form (" 12345-3" is 0.12345e-3): the sign,
 * five digits with the decimal point before them, and a signed exponent.
 */
// The number's three fields, in their order on the line.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<double> exponentNumber(QStringView sign, QStringView digits,
                                     QStringView exponent)
{
  QString mantissa = QStringLiteral("0.") + digits.toString();
  mantissa.replace(QLatin1Char(' '), QLatin1Char('0'));
  const std::optional<double> value = number(mantissa);
  const std::optional<int32_t> power = integer(exponent);
  if (!value || !power) {
    return std::nullopt;
  }
  // As Vallado's twoline2rv: the mantissa, then times a power of ten.
  const double magnitude = *value * std::pow(10.0, *power);
  return sign == QLatin1String("-") ? -magnitude : magnitude;
}

/** "98067A" as "1998-067A" (two-digit years before 57 are 20xx). */
QString cosparDesignator(QStringView text)
{
  QString trimmed = text.trimmed().toString();
  bool ok = false;
  const int32_t year = trimmed.left(2).toInt(&ok);
  if (!ok || trimmed.size() < 5) {
    return trimmed;
  }
  const int32_t fullYear = year < 57 ? 2000 + year : 1900 + year;
  return QString::number(fullYear) + QLatin1Char('-') + trimmed.mid(2);
}

/** The places in a line that must hold a given character. */
struct Fixed {
  int32_t index;
  char expected;
};

/** Checks the fixed columns of @p line; the first wrong one, or empty. */
QString checkColumns(const QString& line, int32_t number,
                     std::initializer_list<Fixed> fixed)
{
  for (const Fixed& f : fixed) {
    if (line.at(f.index) != QLatin1Char(f.expected)) {
      return QCoreApplication::translate("ts::OrbitalElements",
                                         "line %1, column %2: '%3' expected")
          .arg(number)
          .arg(f.index + 1)
          .arg(QLatin1Char(f.expected == ' ' ? '_' : f.expected));
    }
  }
  return QString();
}

// The line's number (1 or 2), then its shortest length.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString checkLine(const QString& line, int32_t lineNumber, int32_t minimum,
                  TleOptions options)
{
  const int32_t length = options.checkChecksums ? 69 : minimum;
  if (line.size() < length) {
    return QCoreApplication::translate(
               "ts::OrbitalElements",
               "line %1 is too short (%2 characters, %3 expected)")
        .arg(lineNumber)
        .arg(line.size())
        .arg(length);
  }
  for (const QChar c : line) {
    if (c.unicode() > 0x7e || c.unicode() < 0x20) {
      return QCoreApplication::translate(
                 "ts::OrbitalElements",
                 "line %1 has characters a TLE can't have")
          .arg(lineNumber);
    }
  }
  if (options.checkChecksums) {
    const QChar written = line.at(68);
    if (!written.isDigit() || written.digitValue() != tleChecksum(line)) {
      return QCoreApplication::translate(
                 "ts::OrbitalElements",
                 "line %1: wrong checksum (%2, should be %3)")
          .arg(lineNumber)
          .arg(written)
          .arg(tleChecksum(line));
    }
  }
  return QString();
}

bool fail(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return false;
}

bool readLine1(const QString& line, OrbitalElements& out, QString* errorString)
{
  const std::optional<int32_t> catalogue =
      parseCatalogueNumber(columns(line, 2, 7).toString());
  const std::optional<int32_t> year = integer(columns(line, 18, 20));
  const std::optional<double> day = number(columns(line, 20, 32));
  const std::optional<double> ndot = number(columns(line, 33, 43));
  const std::optional<double> nddot = exponentNumber(
      columns(line, 44, 45), columns(line, 45, 50), columns(line, 50, 52));
  const std::optional<double> bstar = exponentNumber(
      columns(line, 53, 54), columns(line, 54, 59), columns(line, 59, 61));
  const std::optional<int32_t> type = integer(columns(line, 62, 63));
  const std::optional<int32_t> set = integer(columns(line, 64, 68));
  if (!catalogue || !year || !day || !ndot || !nddot || !bstar || !type ||
      !set) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::OrbitalElements",
                                 "line 1 has a field that isn't a number"));
  }
  out.catalogueNumber = *catalogue;
  out.classification =
      line.at(7) == QLatin1Char(' ') ? 'U' : line.at(7).toLatin1();
  out.internationalDesignator = cosparDesignator(columns(line, 9, 17));
  const int32_t fullYear = *year < 57 ? 2000 + *year : 1900 + *year;
  const double wholeDays = std::floor(*day);
  out.epoch.day = januaryFirstJd(fullYear) + wholeDays - 1.0;
  out.epoch.fraction = *day - wholeDays;
  out.meanMotionDot = *ndot;
  out.meanMotionDdot = *nddot;
  out.bstar = *bstar;
  out.ephemerisType = *type;
  out.elementSetNumber = *set;
  return true;
}

bool readLine2(const QString& line, OrbitalElements& out, QString* errorString)
{
  QString eccentricity = QStringLiteral("0.") + columns(line, 26, 33);
  eccentricity.replace(QLatin1Char(' '), QLatin1Char('0'));
  const std::optional<double> incl = number(columns(line, 8, 16));
  const std::optional<double> raan = number(columns(line, 17, 25));
  const std::optional<double> ecc = number(eccentricity);
  const std::optional<double> argp = number(columns(line, 34, 42));
  const std::optional<double> mo = number(columns(line, 43, 51));
  const std::optional<double> n = number(columns(line, 52, 63));
  const std::optional<int32_t> revs = integer(columns(line, 63, 68));
  if (!incl || !raan || !ecc || !argp || !mo || !n || !revs) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::OrbitalElements",
                                 "line 2 has a field that isn't a number"));
  }
  out.inclinationDeg = *incl;
  out.raanDeg = *raan;
  out.eccentricity = *ecc;
  out.argOfPerigeeDeg = *argp;
  out.meanAnomalyDeg = *mo;
  out.meanMotionRevPerDay = *n;
  out.revolutionNumber = *revs;
  return true;
}

/** A member that may be a number or a string holding one. */
std::optional<double> jsonNumber(const QJsonObject& object, const char* key)
{
  const QJsonValue value = object.value(QLatin1String(key));
  if (value.isDouble()) {
    return value.toDouble();
  }
  if (value.isString()) {
    return number(value.toString());
  }
  return std::nullopt;
}

/** OMM's EPOCH, "2024-05-01T12:00:00.123456" (UTC, perhaps with Z). */
std::optional<JulianDate> ommEpoch(const QString& text)
{
  QString value = text.trimmed();
  if (value.endsWith(QLatin1Char('Z'))) {
    value.chop(1);
  }
  const int32_t t = static_cast<int32_t>(value.indexOf(QLatin1Char('T')));
  const QDate date = QDate::fromString(t < 0 ? value : value.left(t),
                                       QStringLiteral("yyyy-MM-dd"));
  if (!date.isValid()) {
    return std::nullopt;
  }
  JulianDate epoch;
  epoch.day = static_cast<double>(date.toJulianDay()) - 0.5;
  if (t < 0) {
    return epoch;
  }
  const QStringList parts = value.mid(t + 1).split(QLatin1Char(':'));
  if (parts.size() != 3) {
    return std::nullopt;
  }
  const std::optional<int32_t> hours = integer(parts.at(0));
  const std::optional<int32_t> minutes = integer(parts.at(1));
  const std::optional<double> seconds = number(parts.at(2));
  if (!hours || !minutes || !seconds) {
    return std::nullopt;
  }
  epoch.fraction = (*hours * 3600.0 + *minutes * 60.0 + *seconds) / 86400.0;
  return epoch;
}

/** @p value in the TLE's exponent form, 8 characters: " 12345-3". */
QString exponentField(double value)
{
  if (value == 0.0) {
    return QStringLiteral(" 00000+0");
  }
  int32_t exponent =
      static_cast<int32_t>(std::floor(std::log10(std::fabs(value)))) + 1;
  int64_t digits =
      std::llround(std::fabs(value) * std::pow(10.0, 5 - exponent));
  if (digits >= 100000) {
    digits /= 10;
    ++exponent;
  }
  exponent = std::max(-9, std::min(9, exponent));
  return QLatin1Char(value < 0.0 ? '-' : ' ') +
         QString::number(digits).rightJustified(5, QLatin1Char('0')) +
         QLatin1Char(exponent < 0 ? '-' : '+') +
         QString::number(std::abs(exponent));
}

QString fixed(double value, int32_t width, int32_t decimals)
{
  return QString::number(value, 'f', decimals).rightJustified(width);
}

QString withChecksum(QString line)
{
  line = line.leftJustified(68, QLatin1Char(' '), true);
  return line + QString::number(tleChecksum(line));
}

QString catalogueField(int32_t number)
{
  if (number < 100000) {
    return QString::number(number).rightJustified(5, QLatin1Char('0'));
  }
  // Alpha-5: a letter for the hundred-thousands (no I or O).
  int32_t letter = number / 10000 - 10;
  letter += letter >= 'I' - 'A' ? 1 : 0;
  letter += letter >= 'O' - 'A' ? 1 : 0;
  return QLatin1Char(static_cast<char>('A' + letter)) +
         QString::number(number % 10000).rightJustified(4, QLatin1Char('0'));
}

} // namespace

JulianDate JulianDate::fromDateTime(const QDateTime& time)
{
  const int64_t ms = time.toMSecsSinceEpoch();
  const int64_t days = ms >= 0 ? ms / 86400000 : (ms - 86399999) / 86400000;
  JulianDate date;
  date.day = unixEpochJd + static_cast<double>(days);
  date.fraction = static_cast<double>(ms - days * 86400000) / msPerDay;
  return date;
}

QDateTime JulianDate::toDateTime() const
{
  const double ms =
      (day - unixEpochJd) * msPerDay + std::round(fraction * msPerDay);
  return QDateTime::fromMSecsSinceEpoch(std::llround(ms), QTimeZone::utc());
}

double OrbitalElements::ageDays(const QDateTime& time) const
{
  return JulianDate::fromDateTime(time).minutesSince(epoch) / 1440.0;
}

double OrbitalElements::periodMinutes() const
{
  return meanMotionRevPerDay > 0.0 ? 1440.0 / meanMotionRevPerDay : 0.0;
}

QString OrbitalElements::displayName() const
{
  return name.isEmpty() ? QString::number(catalogueNumber) : name;
}

bool OrbitalElements::operator==(const OrbitalElements& other) const
{
  return name == other.name && catalogueNumber == other.catalogueNumber &&
         classification == other.classification &&
         internationalDesignator == other.internationalDesignator &&
         epoch.day == other.epoch.day &&
         epoch.fraction == other.epoch.fraction &&
         meanMotionDot == other.meanMotionDot &&
         meanMotionDdot == other.meanMotionDdot && bstar == other.bstar &&
         inclinationDeg == other.inclinationDeg && raanDeg == other.raanDeg &&
         eccentricity == other.eccentricity &&
         argOfPerigeeDeg == other.argOfPerigeeDeg &&
         meanAnomalyDeg == other.meanAnomalyDeg &&
         meanMotionRevPerDay == other.meanMotionRevPerDay &&
         ephemerisType == other.ephemerisType &&
         elementSetNumber == other.elementSetNumber &&
         revolutionNumber == other.revolutionNumber;
}

int32_t tleChecksum(const QString& line)
{
  int32_t sum = 0;
  const int32_t n = std::min<int32_t>(68, static_cast<int32_t>(line.size()));
  for (int32_t i = 0; i < n; ++i) {
    const QChar c = line.at(i);
    if (c.isDigit()) {
      sum += c.digitValue();
    } else if (c == QLatin1Char('-')) {
      ++sum;
    }
  }
  return sum % 10;
}

std::optional<int32_t> parseCatalogueNumber(const QString& text)
{
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty() || trimmed.size() > 5) {
    return std::nullopt;
  }
  const QChar first = trimmed.at(0);
  int32_t high = 0;
  QString digits = trimmed;
  if (first.isLetter()) {
    const char c = first.toUpper().toLatin1();
    if (c < 'A' || c > 'Z' || c == 'I' || c == 'O' || trimmed.size() != 5) {
      return std::nullopt;
    }
    high = c - 'A' + 10 - (c > 'I' ? 1 : 0) - (c > 'O' ? 1 : 0);
    digits = trimmed.mid(1);
  }
  for (const QChar c : std::as_const(digits)) {
    if (!c.isDigit()) {
      return std::nullopt;
    }
  }
  return high * 10000 + digits.toInt();
}

std::optional<OrbitalElements>
parseTle(const QString& line1, const QString& line2, const QString& name,
         QString* errorString, TleOptions options)
{
  QString problem;
  const QString a = line1.trimmed();
  const QString b = line2.trimmed();
  problem = checkLine(a, 1, 64, options);
  if (problem.isEmpty()) {
    problem = checkColumns(a, 1,
                           {{0, '1'},
                            {1, ' '},
                            {8, ' '},
                            {23, '.'},
                            {32, ' '},
                            {34, '.'},
                            {43, ' '},
                            {52, ' '},
                            {61, ' '},
                            {63, ' '}});
  }
  if (problem.isEmpty()) {
    problem = checkLine(b, 2, 68, options);
  }
  if (problem.isEmpty()) {
    problem = checkColumns(b, 2,
                           {{0, '2'},
                            {1, ' '},
                            {7, ' '},
                            {11, '.'},
                            {16, ' '},
                            {20, '.'},
                            {25, ' '},
                            {33, ' '},
                            {37, '.'},
                            {42, ' '},
                            {46, '.'},
                            {51, ' '}});
  }
  if (problem.isEmpty() && columns(a, 2, 7) != columns(b, 2, 7)) {
    problem = QCoreApplication::translate(
        "ts::OrbitalElements", "the catalogue numbers of lines 1 and 2 differ");
  }
  if (!problem.isEmpty()) {
    fail(errorString, problem);
    return std::nullopt;
  }
  OrbitalElements elements;
  QString nameLine = name.trimmed();
  if (nameLine.startsWith(QLatin1String("0 "))) {
    nameLine = nameLine.mid(2).trimmed();
  }
  elements.name = nameLine;
  if (!readLine1(a, elements, errorString) ||
      !readLine2(b, elements, errorString)) {
    return std::nullopt;
  }
  return elements;
}

ElementSetList parseTleText(const QString& text, TleOptions options)
{
  ElementSetList result;
  const QStringList lines = text.split(QLatin1Char('\n'));
  QString name;
  for (int32_t i = 0; i < static_cast<int32_t>(lines.size()); ++i) {
    const QString line = lines.at(i).trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
      name.clear();
      continue;
    }
    if (!line.startsWith(QLatin1String("1 "))) {
      if (line.startsWith(QLatin1String("2 "))) {
        result.errors.append(
            QCoreApplication::translate("ts::OrbitalElements",
                                        "Line %1: line 2 without line 1")
                .arg(i + 1));
        name.clear();
      } else {
        name = line;
      }
      continue;
    }
    if (i + 1 >= static_cast<int32_t>(lines.size())) {
      result.errors.append(
          QCoreApplication::translate("ts::OrbitalElements",
                                      "Line %1: line 1 without line 2")
              .arg(i + 1));
      break;
    }
    QString problem;
    const std::optional<OrbitalElements> elements =
        parseTle(line, lines.at(i + 1), name, &problem, options);
    if (elements) {
      result.elements.push_back(*elements);
    } else {
      result.errors.append(
          QCoreApplication::translate("ts::OrbitalElements", "Line %1: %2")
              .arg(i + 1)
              .arg(problem));
    }
    name.clear();
    ++i;
  }
  return result;
}

std::optional<OrbitalElements> parseOmm(const QJsonObject& object,
                                        QString* errorString)
{
  const std::optional<JulianDate> epoch =
      ommEpoch(object.value(QLatin1String("EPOCH")).toString());
  const std::optional<double> n = jsonNumber(object, "MEAN_MOTION");
  const std::optional<double> ecc = jsonNumber(object, "ECCENTRICITY");
  const std::optional<double> incl = jsonNumber(object, "INCLINATION");
  const std::optional<double> raan = jsonNumber(object, "RA_OF_ASC_NODE");
  const std::optional<double> argp = jsonNumber(object, "ARG_OF_PERICENTER");
  const std::optional<double> mo = jsonNumber(object, "MEAN_ANOMALY");
  const std::optional<double> catalogue = jsonNumber(object, "NORAD_CAT_ID");
  if (!epoch || !n || !ecc || !incl || !raan || !argp || !mo || !catalogue) {
    fail(errorString, QCoreApplication::translate(
                          "ts::OrbitalElements",
                          "an OMM without its epoch, catalogue number or "
                          "mean elements"));
    return std::nullopt;
  }
  OrbitalElements elements;
  elements.name =
      object.value(QLatin1String("OBJECT_NAME")).toString().trimmed();
  elements.catalogueNumber = static_cast<int32_t>(std::lround(*catalogue));
  const QString classification =
      object.value(QLatin1String("CLASSIFICATION_TYPE")).toString();
  elements.classification =
      classification.isEmpty() ? 'U' : classification.at(0).toLatin1();
  elements.internationalDesignator =
      object.value(QLatin1String("OBJECT_ID")).toString().trimmed();
  elements.epoch = *epoch;
  elements.meanMotionDot = jsonNumber(object, "MEAN_MOTION_DOT").value_or(0.0);
  elements.meanMotionDdot =
      jsonNumber(object, "MEAN_MOTION_DDOT").value_or(0.0);
  elements.bstar = jsonNumber(object, "BSTAR").value_or(0.0);
  elements.inclinationDeg = *incl;
  elements.raanDeg = *raan;
  elements.eccentricity = *ecc;
  elements.argOfPerigeeDeg = *argp;
  elements.meanAnomalyDeg = *mo;
  elements.meanMotionRevPerDay = *n;
  elements.ephemerisType = static_cast<int32_t>(
      std::lround(jsonNumber(object, "EPHEMERIS_TYPE").value_or(0.0)));
  elements.elementSetNumber = static_cast<int32_t>(
      std::lround(jsonNumber(object, "ELEMENT_SET_NO").value_or(0.0)));
  elements.revolutionNumber = static_cast<int32_t>(
      std::lround(jsonNumber(object, "REV_AT_EPOCH").value_or(0.0)));
  return elements;
}

ElementSetList parseOmmJson(const QByteArray& json)
{
  ElementSetList result;
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (document.isNull()) {
    result.errors.append(
        QCoreApplication::translate("ts::OrbitalElements", "Not JSON: %1")
            .arg(error.errorString()));
    return result;
  }
  QJsonArray array = document.array();
  if (document.isObject()) {
    array.append(document.object());
  }
  for (int32_t i = 0; i < static_cast<int32_t>(array.size()); ++i) {
    QString problem;
    const std::optional<OrbitalElements> elements =
        parseOmm(array.at(i).toObject(), &problem);
    if (elements) {
      result.elements.push_back(*elements);
    } else {
      result.errors.append(
          QCoreApplication::translate("ts::OrbitalElements", "Entry %1: %2")
              .arg(i + 1)
              .arg(problem));
    }
  }
  return result;
}

ElementSetList parseElementSets(const QByteArray& data, TleOptions options)
{
  const QByteArray trimmed = data.trimmed();
  if (trimmed.startsWith('[') || trimmed.startsWith('{')) {
    return parseOmmJson(trimmed);
  }
  return parseTleText(QString::fromUtf8(data), options);
}

QStringList formatTle(const OrbitalElements& elements)
{
  const QDate date =
      QDate::fromJulianDay(std::llround(elements.epoch.day + 0.5));
  const double dayOfYear = date.dayOfYear() + elements.epoch.fraction;
  QString designator = elements.internationalDesignator;
  if (designator.size() > 5 && designator.at(4) == QLatin1Char('-')) {
    designator = designator.mid(2, 2) + designator.mid(5);
  }
  const QString catalogue = catalogueField(elements.catalogueNumber);
  QString ndot = fixed(elements.meanMotionDot, 10, 8);
  ndot.replace(QLatin1String("0."), QLatin1String("."));
  ndot = ndot.rightJustified(10);
  const QString line1 =
      QStringLiteral("1 ") + catalogue + QLatin1Char(elements.classification) +
      QLatin1Char(' ') + designator.leftJustified(8, QLatin1Char(' '), true) +
      QLatin1Char(' ') +
      QString::number(date.year() % 100).rightJustified(2, QLatin1Char('0')) +
      QString::number(dayOfYear, 'f', 8).rightJustified(12, QLatin1Char('0')) +
      QLatin1Char(' ') + ndot + QLatin1Char(' ') +
      exponentField(elements.meanMotionDdot) + QLatin1Char(' ') +
      exponentField(elements.bstar) + QLatin1Char(' ') +
      QString::number(elements.ephemerisType % 10) + QLatin1Char(' ') +
      QString::number(elements.elementSetNumber % 10000).rightJustified(4);
  QString eccentricity = QString::number(elements.eccentricity, 'f', 7);
  eccentricity = eccentricity.mid(eccentricity.indexOf(QLatin1Char('.')) + 1);
  const QString line2 =
      QStringLiteral("2 ") + catalogue + QLatin1Char(' ') +
      fixed(elements.inclinationDeg, 8, 4) + QLatin1Char(' ') +
      fixed(elements.raanDeg, 8, 4) + QLatin1Char(' ') + eccentricity +
      QLatin1Char(' ') + fixed(elements.argOfPerigeeDeg, 8, 4) +
      QLatin1Char(' ') + fixed(elements.meanAnomalyDeg, 8, 4) +
      QLatin1Char(' ') + fixed(elements.meanMotionRevPerDay, 11, 8) +
      QString::number(elements.revolutionNumber % 100000).rightJustified(5);
  return {withChecksum(line1), withChecksum(line2)};
}

} // namespace ts
