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

#include "ts/vector/vectorstyle.h"

#include <cstdint>

#include <QJsonValue>
#include <QString>

namespace ts
{

QColor colorFromRgbaHex(const QString& text)
{
  if (!text.startsWith(QLatin1Char('#')) ||
      (text.size() != 7 && text.size() != 9)) {
    return QColor();
  }
  bool ok = true;
  const uint value = text.mid(1).toUInt(&ok, 16);
  if (!ok) {
    return QColor();
  }
  if (text.size() == 7) {
    return QColor(static_cast<int32_t>((value >> 16U) & 0xffU),
                  static_cast<int32_t>((value >> 8U) & 0xffU),
                  static_cast<int32_t>(value & 0xffU));
  }
  return QColor(static_cast<int32_t>((value >> 24U) & 0xffU),
                static_cast<int32_t>((value >> 16U) & 0xffU),
                static_cast<int32_t>((value >> 8U) & 0xffU),
                static_cast<int32_t>(value & 0xffU));
}

QString colorToRgbaHex(const QColor& color)
{
  const QColor rgb = color.toRgb();
  const uint value = (static_cast<uint>(rgb.red()) << 24U) |
                     (static_cast<uint>(rgb.green()) << 16U) |
                     (static_cast<uint>(rgb.blue()) << 8U) |
                     static_cast<uint>(rgb.alpha());
  return QStringLiteral("#%1").arg(value, 8, 16, QLatin1Char('0'));
}

std::array<float, 4> premultipliedRgba(const QColor& color, double opacity)
{
  const QColor rgb = color.toRgb();
  // alphaF() and friends are double in Qt 5 and float in Qt 6.
  const double a = static_cast<double>(rgb.alphaF()) * opacity;
  return {static_cast<float>(static_cast<double>(rgb.redF()) * a),
          static_cast<float>(static_cast<double>(rgb.greenF()) * a),
          static_cast<float>(static_cast<double>(rgb.blueF()) * a),
          static_cast<float>(a)};
}

VectorStyle VectorStyle::fromJson(const QJsonObject& json)
{
  VectorStyle style;
  const QColor fill =
      colorFromRgbaHex(json.value(QStringLiteral("fill")).toString());
  if (fill.isValid()) {
    style.fill = fill;
  }
  const QColor outline =
      colorFromRgbaHex(json.value(QStringLiteral("outline")).toString());
  if (outline.isValid()) {
    style.outline = outline;
  }
  const double width =
      json.value(QStringLiteral("outlineWidthPx")).toDouble(-1.0);
  if (width >= 0.0) {
    style.outlineWidthPx = width;
  }
  const double point = json.value(QStringLiteral("pointSizePx")).toDouble(-1.0);
  if (point >= 0.0) {
    style.pointSizePx = point;
  }
  return style;
}

QJsonObject VectorStyle::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("fill"), colorToRgbaHex(fill));
  json.insert(QStringLiteral("outline"), colorToRgbaHex(outline));
  json.insert(QStringLiteral("outlineWidthPx"), outlineWidthPx);
  // Only when not the default, so styles without points stay as they were.
  if (pointSizePx != VectorStyle().pointSizePx) {
    json.insert(QStringLiteral("pointSizePx"), pointSizePx);
  }
  return json;
}

} // namespace ts
