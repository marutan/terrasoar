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
#include "ts/adsb/modesmessage.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <functional>

namespace ts
{

namespace
{

/** The parity's generator, less its x²⁴ term. */
constexpr uint32_t generator = 0xFFF409U;

/** The 6-bit character set of callsigns ('#' for none). */
constexpr char callsignCharacters[] =
    "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

/** Bits of a message by position (1 is the first bit). */
class Bits
{
public:
  explicit Bits(const QByteArray& bytes) : m_bytes(bytes) {}

  /** Returns @p count bits (at most 32) from bit @p first on. */
  uint32_t get(int32_t first, int32_t count) const
  {
    uint32_t value = 0;
    for (int32_t i = 0; i < count; ++i) {
      const int32_t bit = first - 1 + i;
      const uint8_t byte = static_cast<uint8_t>(m_bytes.at(bit / 8));
      value =
          (value << 1U) | ((byte >> static_cast<uint32_t>(7 - bit % 8)) & 1U);
    }
    return value;
  }

private:
  const QByteArray& m_bytes;
};

int32_t messageBits(int32_t downlinkFormat)
{
  return downlinkFormat >= 16 ? 112 : 56;
}

/** The ADS-B 12-bit altitude with the M bit (0) put back. */
uint32_t withMBit(uint32_t code12)
{
  return ((code12 & 0xFC0U) << 1U) | (code12 & 0x3FU);
}

/** Returns a Gray-coded @p value as a binary number. */
uint32_t fromGray(uint32_t value)
{
  uint32_t result = value;
  for (uint32_t shift = value >> 1U; shift != 0; shift >>= 1U) {
    result ^= shift;
  }
  return result;
}

void decodeIdentification(const Bits& me, ModeSMessage& message)
{
  QString callsign;
  for (int32_t i = 0; i < 8; ++i) {
    const char c = callsignCharacters[me.get(9 + 6 * i, 6)];
    if (c != '#') {
      callsign.append(QLatin1Char(c));
    }
  }
  message.callsign = callsign.trimmed();
  const uint32_t category = me.get(6, 3);
  if (category != 0) {
    // Type codes 4, 3, 2, 1 are sets A, B, C, D.
    const char set = static_cast<char>('A' + (4 - message.typeCode));
    message.category = QString(QLatin1Char(set)) + QString::number(category);
  }
}

void decodePosition(const Bits& me, ModeSMessage& message)
{
  const int32_t tc = message.typeCode;
  CprFrame frame;
  frame.odd = me.get(22, 1) != 0;
  frame.lat = me.get(23, 17);
  frame.lon = me.get(40, 17);
  frame.surface = tc >= 5 && tc <= 8;
  if (frame.surface) {
    message.onGround = true;
    message.groundSpeedKt = decodeSurfaceMovementKt(me.get(6, 7));
    if (me.get(13, 1) != 0) {
      message.trackDeg = 360.0 * me.get(14, 7) / 128.0;
    }
  } else if (tc <= 18) {
    message.onGround = false;
    message.altitudeFt = decodeAltitudeCode(withMBit(me.get(9, 12)));
  } else {
    message.onGround = false;
    const uint32_t height = me.get(9, 12);
    if (height != 0) {
      message.gnssHeightM = static_cast<int32_t>(height);
    }
  }
  message.cpr = frame;
}

void decodeVelocity(const Bits& me, ModeSMessage& message)
{
  const uint32_t subtype = me.get(6, 3);
  if (subtype == 1 || subtype == 2) {
    const uint32_t vew = me.get(15, 10);
    const uint32_t vns = me.get(26, 10);
    if (vew != 0 && vns != 0) {
      const double factor = subtype == 2 ? 4.0 : 1.0;
      const double vx = (me.get(14, 1) != 0 ? -1.0 : 1.0) * factor * (vew - 1);
      const double vy = (me.get(25, 1) != 0 ? -1.0 : 1.0) * factor * (vns - 1);
      message.groundSpeedKt = std::hypot(vx, vy);
      double track = std::atan2(vx, vy) * 180.0 / 3.14159265358979323846;
      if (track < 0.0) {
        track += 360.0;
      }
      message.trackDeg = track;
    }
  } else if (subtype == 3 || subtype == 4) {
    if (me.get(14, 1) != 0) {
      message.headingDeg = 360.0 * me.get(15, 10) / 1024.0;
    }
    const uint32_t airspeed = me.get(26, 10);
    if (airspeed != 0) {
      message.airspeedKt = (subtype == 4 ? 4.0 : 1.0) * (airspeed - 1);
      message.trueAirspeed = me.get(25, 1) != 0;
    }
  } else {
    return;
  }
  const uint32_t rate = me.get(38, 9);
  if (rate != 0) {
    const int32_t sign = me.get(37, 1) != 0 ? -1 : 1;
    message.verticalRateFtMin = sign * 64 * static_cast<int32_t>(rate - 1);
    message.verticalRateBaro = me.get(36, 1) != 0;
  }
  const uint32_t difference = me.get(50, 7);
  if (difference != 0 && difference != 0x7FU) {
    const int32_t sign = me.get(49, 1) != 0 ? -1 : 1;
    message.gnssMinusBaroFt = sign * 25 * static_cast<int32_t>(difference - 1);
  }
}

void decodeStatus(const Bits& me, ModeSMessage& message)
{
  if (me.get(6, 3) != 1) {
    return;
  }
  message.emergency = static_cast<AdsbEmergency>(me.get(9, 3));
  const uint32_t identity = me.get(12, 13);
  if (identity != 0) {
    message.squawk = decodeIdentity(identity);
  }
}

/** Decodes an extended squitter's ME field (bits 33 to 88). */
bool decodeExtendedSquitter(const Bits& bits, const QByteArray& bytes,
                            ModeSMessage& message)
{
  const QByteArray meBytes = bytes.mid(4, 7);
  const Bits me(meBytes);
  message.typeCode = static_cast<int32_t>(me.get(1, 5));
  const int32_t tc = message.typeCode;
  if (message.downlinkFormat == 17) {
    const uint32_t capability = bits.get(6, 3);
    if (capability == 4) {
      message.onGround = true;
    } else if (capability == 5) {
      message.onGround = false;
    }
  }
  if (tc >= 1 && tc <= 4) {
    decodeIdentification(me, message);
  } else if ((tc >= 5 && tc <= 18) || (tc >= 20 && tc <= 22)) {
    decodePosition(me, message);
  } else if (tc == 19) {
    decodeVelocity(me, message);
  } else if (tc == 28) {
    decodeStatus(me, message);
  }
  return true;
}

} // namespace

uint32_t modeSParity(const QByteArray& message)
{
  const int32_t n = static_cast<int32_t>(message.size());
  if (n < 4) {
    return 0xFFFFFFU;
  }
  uint32_t crc = 0;
  for (int32_t i = 0; i < n - 3; ++i) {
    crc ^= static_cast<uint32_t>(static_cast<uint8_t>(message.at(i))) << 16U;
    for (int32_t bit = 0; bit < 8; ++bit) {
      crc <<= 1U;
      if ((crc & 0x1000000U) != 0) {
        crc ^= generator;
      }
    }
    crc &= 0xFFFFFFU;
  }
  const uint32_t parity =
      (static_cast<uint32_t>(static_cast<uint8_t>(message.at(n - 3))) << 16U) |
      (static_cast<uint32_t>(static_cast<uint8_t>(message.at(n - 2))) << 8U) |
      static_cast<uint32_t>(static_cast<uint8_t>(message.at(n - 1)));
  return (crc ^ parity) & 0xFFFFFFU;
}

QByteArray modeSFromHex(const QByteArray& hex)
{
  if (hex.size() != 14 && hex.size() != 28) {
    return QByteArray();
  }
  for (const char c : hex) {
    const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
    if (!digit) {
      return QByteArray();
    }
  }
  return QByteArray::fromHex(hex);
}

std::optional<int32_t> decodeAltitudeCode(uint32_t code13)
{
  if (code13 == 0) {
    return std::nullopt;
  }
  const bool m = (code13 & 0x40U) != 0;
  const bool q = (code13 & 0x10U) != 0;
  if (m) {
    // Metres: the other 12 bits (left to later; rare).
    const uint32_t metres = ((code13 & 0x1F80U) >> 1U) | (code13 & 0x3FU);
    return static_cast<int32_t>(std::lround(metres / 0.3048));
  }
  if (q) {
    const uint32_t n = ((code13 & 0x1F80U) >> 2U) | ((code13 & 0x20U) >> 1U) |
                       (code13 & 0x0FU);
    return 25 * static_cast<int32_t>(n) - 1000;
  }
  // Gillham code: C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4 (bit 12 down to 0).
  std::array<uint32_t, 13> bit{};
  for (uint32_t i = 0; i < 13; ++i) {
    bit[i] = (code13 >> (12U - i)) & 1U;
  }
  const uint32_t c1 = bit[0];
  const uint32_t a1 = bit[1];
  const uint32_t c2 = bit[2];
  const uint32_t a2 = bit[3];
  const uint32_t c4 = bit[4];
  const uint32_t a4 = bit[5];
  const uint32_t b1 = bit[7];
  const uint32_t b2 = bit[9];
  const uint32_t d2 = bit[10];
  const uint32_t b4 = bit[11];
  const uint32_t d4 = bit[12];
  const uint32_t gray500 = (d2 << 7U) | (d4 << 6U) | (a1 << 5U) | (a2 << 4U) |
                           (a4 << 3U) | (b1 << 2U) | (b2 << 1U) | b4;
  const uint32_t gray100 = (c1 << 2U) | (c2 << 1U) | c4;
  const uint32_t n500 = fromGray(gray500);
  int32_t n100 = static_cast<int32_t>(fromGray(gray100));
  if (n100 == 0 || n100 == 5 || n100 == 6) {
    return std::nullopt;
  }
  if (n100 == 7) {
    n100 = 5;
  }
  if (n500 % 2 != 0) {
    n100 = 6 - n100;
  }
  return 500 * static_cast<int32_t>(n500) + 100 * n100 - 1300;
}

QString decodeIdentity(uint32_t code13)
{
  // C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4 (bit 12 down to 0).
  const std::function<uint32_t(uint32_t)> bit = [code13](uint32_t i) {
    return (code13 >> (12U - i)) & 1U;
  };
  const uint32_t a = (bit(5) << 2U) | (bit(3) << 1U) | bit(1);
  const uint32_t b = (bit(11) << 2U) | (bit(9) << 1U) | bit(7);
  const uint32_t c = (bit(4) << 2U) | (bit(2) << 1U) | bit(0);
  const uint32_t d = (bit(12) << 2U) | (bit(10) << 1U) | bit(8);
  return QString::number(a * 1000 + b * 100 + c * 10 + d)
      .rightJustified(4, QLatin1Char('0'));
}

std::optional<double> decodeSurfaceMovementKt(uint32_t movement)
{
  if (movement == 0 || movement > 124) {
    return std::nullopt;
  }
  const double n = movement;
  if (movement == 1) {
    return 0.0;
  }
  if (movement <= 8) {
    return 0.125 + (n - 2.0) * 0.125;
  }
  if (movement <= 12) {
    return 1.0 + (n - 9.0) * 0.25;
  }
  if (movement <= 38) {
    return 2.0 + (n - 13.0) * 0.5;
  }
  if (movement <= 93) {
    return 15.0 + (n - 39.0);
  }
  if (movement <= 108) {
    return 70.0 + (n - 94.0) * 2.0;
  }
  if (movement <= 123) {
    return 100.0 + (n - 109.0) * 5.0;
  }
  return 175.0;
}

std::optional<ModeSMessage> decodeModeS(const QByteArray& bytes)
{
  if (bytes.size() != 7 && bytes.size() != 14) {
    return std::nullopt;
  }
  const Bits bits(bytes);
  ModeSMessage message;
  message.downlinkFormat = static_cast<int32_t>(bits.get(1, 5));
  const int32_t df = message.downlinkFormat;
  if (messageBits(df) != bytes.size() * 8) {
    return std::nullopt;
  }
  const uint32_t parity = modeSParity(bytes);
  switch (df) {
  case 11:
    // The remainder is the interrogator's code (0 to 79), or 0.
    if (parity > 0x7FU) {
      return std::nullopt;
    }
    message.address = bits.get(9, 24);
    {
      const uint32_t capability = bits.get(6, 3);
      if (capability == 4) {
        message.onGround = true;
      } else if (capability == 5) {
        message.onGround = false;
      }
    }
    return message;
  case 17:
  case 18:
    if (parity != 0) {
      return std::nullopt;
    }
    // DF 18 with control field 0 has an ICAO address; the others are
    // anonymous, TIS-B or ADS-R, left out.
    if (df == 18 && bits.get(6, 3) != 0) {
      return std::nullopt;
    }
    message.address = bits.get(9, 24);
    decodeExtendedSquitter(bits, bytes, message);
    return message;
  case 0:
  case 16:
    message.address = parity;
    message.addressFromParity = true;
    message.onGround = bits.get(6, 1) != 0;
    message.altitudeFt = decodeAltitudeCode(bits.get(20, 13));
    return message;
  case 4:
  case 20:
  case 5:
  case 21: {
    message.address = parity;
    message.addressFromParity = true;
    const uint32_t status = bits.get(6, 3);
    if (status == 1 || status == 3) {
      message.onGround = true;
    } else if (status == 0 || status == 2) {
      message.onGround = false;
    }
    if (df == 4 || df == 20) {
      message.altitudeFt = decodeAltitudeCode(bits.get(20, 13));
    } else {
      const uint32_t identity = bits.get(20, 13);
      message.squawk = decodeIdentity(identity);
    }
    return message;
  }
  default:
    return std::nullopt;
  }
}

} // namespace ts
