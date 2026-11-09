// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/vector/vectorstyle.h"

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
    return QColor(static_cast<int>((value >> 16U) & 0xffU),
                  static_cast<int>((value >> 8U) & 0xffU),
                  static_cast<int>(value & 0xffU));
  }
  return QColor(static_cast<int>((value >> 24U) & 0xffU),
                static_cast<int>((value >> 16U) & 0xffU),
                static_cast<int>((value >> 8U) & 0xffU),
                static_cast<int>(value & 0xffU));
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
  return style;
}

QJsonObject VectorStyle::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("fill"), colorToRgbaHex(fill));
  json.insert(QStringLiteral("outline"), colorToRgbaHex(outline));
  json.insert(QStringLiteral("outlineWidthPx"), outlineWidthPx);
  return json;
}

} // namespace ts
