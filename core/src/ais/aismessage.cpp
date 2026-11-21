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
#include "ts/ais/aismessage.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <utility>

#include <QList>

namespace ts
{

namespace
{

/** Bits of a payload by position (0 is the first). */
class AisBits
{
public:
  /** Reads @p bits, which must outlive it. */
  explicit AisBits(const std::vector<bool>& bits) : m_bits(&bits) {}

  /** Returns the number of bits. */
  int32_t size() const { return static_cast<int32_t>(m_bits->size()); }

  /** Returns @p count bits (at most 32) from @p first, unsigned. */
  uint32_t get(int32_t first, int32_t count) const
  {
    uint32_t value = 0;
    for (int32_t i = first; i < first + count; ++i) {
      value =
          (value << 1U) | (m_bits->at(static_cast<std::size_t>(i)) ? 1U : 0U);
    }
    return value;
  }

  /** Returns @p count bits from @p first, two's complement. */
  int32_t getSigned(int32_t first, int32_t count) const
  {
    const uint32_t value = get(first, count);
    const uint32_t sign = 1U << static_cast<uint32_t>(count - 1);
    return (value & sign) != 0
               ? static_cast<int32_t>(value) - static_cast<int32_t>(sign << 1U)
               : static_cast<int32_t>(value);
  }

  /**
   * Returns @p characters 6-bit characters from @p first (fewer if the
   * payload ends), without the '@' padding and trailing spaces.
   */
  QString text(int32_t first, int32_t characters) const
  {
    QString result;
    for (int32_t i = 0; i < characters; ++i) {
      const int32_t at = first + 6 * i;
      if (at + 6 > size()) {
        break;
      }
      const uint32_t v = get(at, 6);
      const char c = static_cast<char>(v < 32 ? v + 64 : v);
      if (c == '@') {
        break;
      }
      result.append(QLatin1Char(c));
    }
    return result.trimmed();
  }

private:
  const std::vector<bool>* m_bits;
};

/** A position in 1/10000 minute (or 1/10 for type 27), if available. */
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void readPosition(AisBits bits, int32_t lonAt, int32_t lonBits, int32_t latAt,
                  int32_t latBits, double perDegree, AisMessage& message)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const double lon = bits.getSigned(lonAt, lonBits) / perDegree;
  const double lat = bits.getSigned(latAt, latBits) / perDegree;
  if (std::fabs(lon) <= 180.0 && std::fabs(lat) <= 90.0) {
    message.lonDeg = lon;
    message.latDeg = lat;
  }
}

/** Speed in tenths of a knot (1023: not available). */
std::optional<double> tenthsOfKnots(uint32_t value)
{
  return value == 1023
             ? std::nullopt
             : std::optional<double>(static_cast<double>(value) / 10.0);
}

/** Course in tenths of a degree (3600 and above: not available). */
std::optional<double> tenthsOfDegrees(uint32_t value)
{
  return value >= 3600
             ? std::nullopt
             : std::optional<double>(static_cast<double>(value) / 10.0);
}

/** Heading in degrees (511: not available). */
std::optional<int32_t> heading(uint32_t value)
{
  return value >= 360 ? std::nullopt
                      : std::optional<int32_t>(static_cast<int32_t>(value));
}

/** The UTC second (60 and above: not available, or other meanings). */
std::optional<int32_t> utcSecond(uint32_t value)
{
  return value >= 60 ? std::nullopt
                     : std::optional<int32_t>(static_cast<int32_t>(value));
}

/** Dimensions from 30 bits at @p at (9, 9, 6, 6), if any is set. */
std::optional<AisDimensions> dimensions(AisBits bits, int32_t at)
{
  AisDimensions d;
  d.toBow = static_cast<int32_t>(bits.get(at, 9));
  d.toStern = static_cast<int32_t>(bits.get(at + 9, 9));
  d.toPort = static_cast<int32_t>(bits.get(at + 18, 6));
  d.toStarboard = static_cast<int32_t>(bits.get(at + 24, 6));
  if (d.toBow + d.toStern + d.toPort + d.toStarboard == 0) {
    return std::nullopt;
  }
  return d;
}

std::optional<QString> textOrNone(const QString& text)
{
  return text.isEmpty() ? std::nullopt : std::optional<QString>(text);
}

std::optional<int32_t> shipType(uint32_t value)
{
  return value == 0 ? std::nullopt
                    : std::optional<int32_t>(static_cast<int32_t>(value));
}

bool decodeClassA(AisBits bits, AisMessage& m)
{
  if (bits.size() < 143) {
    return false;
  }
  m.status = static_cast<AisNavStatus>(bits.get(38, 4));
  const int32_t rot = bits.getSigned(42, 8);
  if (rot != -128) {
    // ROT_AIS = 4.733 √(degrees a minute), signed.
    const double root = rot / 4.733;
    m.turnDegMin = (rot < 0 ? -1.0 : 1.0) * root * root;
  }
  m.speedKn = tenthsOfKnots(bits.get(50, 10));
  m.accurate = bits.get(60, 1) != 0;
  readPosition(bits, 61, 28, 89, 27, 600000.0, m);
  m.courseDeg = tenthsOfDegrees(bits.get(116, 12));
  m.headingDeg = heading(bits.get(128, 9));
  m.second = utcSecond(bits.get(137, 6));
  return true;
}

bool decodeBaseStation(AisBits bits, AisMessage& m)
{
  if (bits.size() < 134) {
    return false;
  }
  m.utc = std::vector<int32_t>{static_cast<int32_t>(bits.get(38, 14)),
                               static_cast<int32_t>(bits.get(52, 4)),
                               static_cast<int32_t>(bits.get(56, 5)),
                               static_cast<int32_t>(bits.get(61, 5)),
                               static_cast<int32_t>(bits.get(66, 6)),
                               static_cast<int32_t>(bits.get(72, 6))};
  m.accurate = bits.get(78, 1) != 0;
  readPosition(bits, 79, 28, 107, 27, 600000.0, m);
  return true;
}

bool decodeStaticVoyage(AisBits bits, AisMessage& m)
{
  if (bits.size() < 422) {
    return false;
  }
  const uint32_t imo = bits.get(40, 30);
  if (imo != 0) {
    m.imo = imo;
  }
  m.callsign = textOrNone(bits.text(70, 7));
  m.name = textOrNone(bits.text(112, 20));
  m.shipType = shipType(bits.get(232, 8));
  m.dimensions = dimensions(bits, 240);
  AisEta eta;
  eta.month = static_cast<int32_t>(bits.get(274, 4));
  eta.day = static_cast<int32_t>(bits.get(278, 5));
  eta.hour = static_cast<int32_t>(bits.get(283, 5));
  eta.minute = static_cast<int32_t>(bits.get(288, 6));
  if (eta.month != 0 && eta.day != 0 && eta.hour < 24 && eta.minute < 60) {
    m.eta = eta;
  }
  const uint32_t draught = bits.get(294, 8);
  if (draught != 0) {
    m.draughtM = static_cast<double>(draught) / 10.0;
  }
  m.destination = textOrNone(bits.text(302, 20));
  return true;
}

bool decodeSarAircraft(AisBits bits, AisMessage& m)
{
  if (bits.size() < 134) {
    return false;
  }
  const uint32_t altitude = bits.get(38, 12);
  if (altitude != 4095) {
    m.altitudeM = static_cast<int32_t>(altitude);
  }
  const uint32_t speed = bits.get(50, 10);
  if (speed != 1023) {
    m.speedKn = static_cast<double>(speed);
  }
  m.accurate = bits.get(60, 1) != 0;
  readPosition(bits, 61, 28, 89, 27, 600000.0, m);
  m.courseDeg = tenthsOfDegrees(bits.get(116, 12));
  m.second = utcSecond(bits.get(128, 6));
  return true;
}

bool decodeClassB(AisBits bits, AisMessage& m)
{
  if (bits.size() < 139) {
    return false;
  }
  m.speedKn = tenthsOfKnots(bits.get(46, 10));
  m.accurate = bits.get(56, 1) != 0;
  readPosition(bits, 57, 28, 85, 27, 600000.0, m);
  m.courseDeg = tenthsOfDegrees(bits.get(112, 12));
  m.headingDeg = heading(bits.get(124, 9));
  m.second = utcSecond(bits.get(133, 6));
  if (m.type == 19) {
    if (bits.size() < 301) {
      return false;
    }
    m.name = textOrNone(bits.text(143, 20));
    m.shipType = shipType(bits.get(263, 8));
    m.dimensions = dimensions(bits, 271);
  }
  return true;
}

bool decodeAidToNavigation(AisBits bits, AisMessage& m)
{
  if (bits.size() < 259) {
    return false;
  }
  m.aidType = static_cast<int32_t>(bits.get(38, 5));
  QString name = bits.text(43, 20);
  // The name's extension, after the fixed fields, when the name is full.
  if (bits.size() > 272) {
    name += bits.text(272, (bits.size() - 272) / 6);
  }
  m.name = textOrNone(name);
  m.accurate = bits.get(163, 1) != 0;
  readPosition(bits, 164, 28, 192, 27, 600000.0, m);
  m.dimensions = dimensions(bits, 219);
  m.second = utcSecond(bits.get(253, 6));
  return true;
}

bool decodeStaticClassB(AisBits bits, AisMessage& m)
{
  if (bits.size() < 40) {
    return false;
  }
  const int32_t part = static_cast<int32_t>(bits.get(38, 2));
  m.part = part;
  if (part == 0) {
    if (bits.size() < 160) {
      return false;
    }
    m.name = textOrNone(bits.text(40, 20));
    return true;
  }
  if (part != 1 || bits.size() < 162) {
    return false;
  }
  m.shipType = shipType(bits.get(40, 8));
  m.callsign = textOrNone(bits.text(90, 7));
  // An auxiliary craft (MMSI 98xxxxxxx) gives its mother ship instead.
  if (m.mmsi / 10000000U != 98U) {
    m.dimensions = dimensions(bits, 132);
  }
  return true;
}

bool decodeLongRange(AisBits bits, AisMessage& m)
{
  if (bits.size() < 95) {
    return false;
  }
  m.accurate = bits.get(38, 1) != 0;
  m.status = static_cast<AisNavStatus>(bits.get(40, 4));
  readPosition(bits, 44, 18, 62, 17, 600.0, m);
  const uint32_t speed = bits.get(79, 6);
  if (speed != 63) {
    m.speedKn = static_cast<double>(speed);
  }
  const uint32_t course = bits.get(85, 9);
  if (course < 360) {
    m.courseDeg = static_cast<double>(course);
  }
  return true;
}

} // namespace

uint8_t nmeaChecksum(const QByteArray& text)
{
  uint8_t sum = 0;
  for (const char c : text) {
    sum = static_cast<uint8_t>(sum ^ static_cast<uint8_t>(c));
  }
  return sum;
}

namespace
{

/** Checks "body*hh" and returns the body; nothing if it doesn't match. */
std::optional<QByteArray> checkedBody(const QByteArray& text)
{
  const int32_t star = static_cast<int32_t>(text.lastIndexOf('*'));
  if (star < 0 || star + 3 > text.size()) {
    return std::nullopt;
  }
  bool ok = false;
  const uint32_t given = text.mid(star + 1, 2).toUInt(&ok, 16);
  QByteArray body = text.left(star);
  if (!ok || given != nmeaChecksum(body)) {
    return std::nullopt;
  }
  return body;
}

/** Reads a tag block's fields into @p sentence. */
void readTagBlock(const QByteArray& block, AisSentence& sentence)
{
  for (const QByteArray& field : block.split(',')) {
    if (field.startsWith("c:")) {
      bool ok = false;
      double seconds = field.mid(2).toDouble(&ok);
      if (ok) {
        // Seconds, or milliseconds from AIS-catcher and others.
        if (seconds > 1.0e11) {
          seconds /= 1000.0;
        }
        sentence.tagTimeS = seconds;
      }
    } else if (field.startsWith("s:")) {
      sentence.tagSource = field.mid(2);
    }
  }
}

} // namespace

std::optional<AisSentence> parseAisSentence(const QByteArray& line,
                                            AisSentenceError* error)
{
  const std::function<std::optional<AisSentence>(AisSentenceError)> fail =
      [error](AisSentenceError why) {
        if (error != nullptr) {
          *error = why;
        }
        return std::optional<AisSentence>();
      };
  QByteArray text = line.trimmed();
  AisSentence sentence;
  // A tag block: \c:…,s:…*hh\ before the sentence.
  if (text.startsWith('\\')) {
    const int32_t end = static_cast<int32_t>(text.indexOf('\\', 1));
    if (end < 0) {
      return fail(AisSentenceError::Broken);
    }
    const std::optional<QByteArray> block = checkedBody(text.mid(1, end - 1));
    if (!block) {
      return fail(AisSentenceError::Checksum);
    }
    readTagBlock(*block, sentence);
    text = text.mid(end + 1);
  }
  const int32_t start = static_cast<int32_t>(text.indexOf('!'));
  if (start < 0 || text.size() < start + 6 ||
      (text.mid(start + 3, 3) != "VDM" && text.mid(start + 3, 3) != "VDO")) {
    return fail(AisSentenceError::NotAis);
  }
  const std::optional<QByteArray> body = checkedBody(text.mid(start + 1));
  if (!body) {
    return fail(AisSentenceError::Checksum);
  }
  const QList<QByteArray> fields = body->split(',');
  if (fields.size() < 7) {
    return fail(AisSentenceError::Broken);
  }
  bool countOk = false;
  bool numberOk = false;
  bool fillOk = false;
  sentence.count = fields.at(1).toInt(&countOk);
  sentence.number = fields.at(2).toInt(&numberOk);
  sentence.sequence = fields.at(3).isEmpty() ? -1 : fields.at(3).toInt();
  sentence.channel = fields.at(4);
  sentence.payload = fields.at(5);
  sentence.fillBits = fields.at(6).toInt(&fillOk);
  sentence.own = fields.at(0).endsWith("VDO");
  if (!countOk || !numberOk || !fillOk || sentence.count < 1 ||
      sentence.count > 9 || sentence.number < 1 ||
      sentence.number > sentence.count || sentence.fillBits < 0 ||
      sentence.fillBits > 5) {
    return fail(AisSentenceError::Broken);
  }
  if (error != nullptr) {
    *error = AisSentenceError::None;
  }
  return sentence;
}

std::optional<AisAssembler::Payload>
AisAssembler::add(const AisSentence& sentence)
{
  ++m_count;
  // Drop what waited too long.
  for (std::map<QByteArray, Partial>::iterator i = m_partial.begin();
       i != m_partial.end();) {
    if (m_count - i->second.started > maximumPending) {
      ++m_dropped;
      i = m_partial.erase(i);
    } else {
      ++i;
    }
  }
  if (sentence.count == 1) {
    return Payload{sentence.payload, sentence.fillBits, sentence.tagTimeS,
                   sentence.own};
  }
  const QByteArray key = QByteArray::number(sentence.sequence) + '/' +
                         sentence.channel + '/' +
                         QByteArray::number(sentence.count);
  Partial& partial = m_partial[key];
  if (partial.fragments.empty()) {
    partial.fragments.resize(static_cast<std::size_t>(sentence.count));
    partial.started = m_count;
  }
  QByteArray& slot =
      partial.fragments.at(static_cast<std::size_t>(sentence.number - 1));
  if (!slot.isEmpty()) {
    // A fragment again: a new message with the same id began.
    partial = Partial();
    partial.fragments.resize(static_cast<std::size_t>(sentence.count));
    partial.started = m_count;
    ++m_dropped;
  }
  partial.fragments.at(static_cast<std::size_t>(sentence.number - 1)) =
      sentence.payload.isEmpty() ? QByteArray(" ") : sentence.payload;
  if (sentence.number == sentence.count) {
    partial.fillBits = sentence.fillBits;
  }
  // The first fragment's time, or any while it hasn't come.
  if (sentence.number == 1 || !partial.tagTimeS) {
    partial.tagTimeS = sentence.tagTimeS;
  }
  partial.own = sentence.own;
  for (const QByteArray& fragment : partial.fragments) {
    if (fragment.isEmpty()) {
      return std::nullopt;
    }
  }
  Payload whole;
  for (const QByteArray& fragment : partial.fragments) {
    whole.payload += fragment.trimmed();
  }
  whole.fillBits = partial.fillBits;
  whole.tagTimeS = partial.tagTimeS;
  whole.own = partial.own;
  m_partial.erase(key);
  return whole;
}

std::vector<bool> aisPayloadBits(const QByteArray& payload, int32_t fillBits)
{
  std::vector<bool> bits;
  bits.reserve(static_cast<std::size_t>(payload.size()) * 6);
  for (const char c : payload) {
    int32_t v = static_cast<uint8_t>(c) - 48;
    if (v > 40) {
      v -= 8;
    }
    if (v < 0 || v > 63) {
      return std::vector<bool>();
    }
    for (int32_t bit = 5; bit >= 0; --bit) {
      bits.push_back(((v >> bit) & 1) != 0);
    }
  }
  const std::size_t fill = static_cast<std::size_t>(std::max(fillBits, 0));
  bits.resize(bits.size() > fill ? bits.size() - fill : 0);
  return bits;
}

std::optional<AisMessage> decodeAis(const QByteArray& payload, int32_t fillBits,
                                    int32_t* type)
{
  const std::vector<bool> raw = aisPayloadBits(payload, fillBits);
  const AisBits bits(raw);
  if (type != nullptr) {
    *type = bits.size() >= 6 ? static_cast<int32_t>(bits.get(0, 6)) : 0;
  }
  if (bits.size() < 38) {
    return std::nullopt;
  }
  AisMessage m;
  m.type = static_cast<int32_t>(bits.get(0, 6));
  m.mmsi = bits.get(8, 30);
  bool read = false;
  switch (m.type) {
  case 1:
  case 2:
  case 3:
    read = decodeClassA(bits, m);
    break;
  case 4:
    read = decodeBaseStation(bits, m);
    break;
  case 5:
    read = decodeStaticVoyage(bits, m);
    break;
  case 9:
    read = decodeSarAircraft(bits, m);
    break;
  case 18:
  case 19:
    read = decodeClassB(bits, m);
    break;
  case 21:
    read = decodeAidToNavigation(bits, m);
    break;
  case 24:
    read = decodeStaticClassB(bits, m);
    break;
  case 27:
    read = decodeLongRange(bits, m);
    break;
  default:
    break;
  }
  if (!read) {
    return std::nullopt;
  }
  return m;
}

} // namespace ts
