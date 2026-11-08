// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/providers/testproviders.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include <QCoreApplication>
#include <QRect>

namespace ts
{

namespace
{

/** Glyph rows of a 3 × 5 bitmap font, bit 2 the leftmost pixel. */
using Glyph = std::array<std::uint8_t, 5>;

constexpr std::array<Glyph, 11> glyphs = {{
    {7, 5, 5, 5, 7}, // 0
    {2, 6, 2, 2, 7}, // 1
    {7, 1, 7, 4, 7}, // 2
    {7, 1, 7, 1, 7}, // 3
    {5, 5, 7, 1, 1}, // 4
    {7, 4, 7, 1, 7}, // 5
    {7, 4, 7, 5, 7}, // 6
    {7, 1, 1, 1, 1}, // 7
    {7, 5, 7, 5, 7}, // 8
    {7, 5, 7, 1, 7}, // 9
    {1, 1, 2, 4, 4}, // /
}};

void fillRect(QImage& image, QRect rect, QRgb color)
{
  const QRect clipped = rect.intersected(image.rect());
  const int x0 = clipped.left();
  const int y0 = clipped.top();
  const int x1 = clipped.left() + clipped.width();
  const int y1 = clipped.top() + clipped.height();
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      image.setPixel(x, y, color);
    }
  }
}

/** Draws @p text (digits and '/') centred, each font pixel @p scale wide. */
void drawLabel(QImage& image, const QString& text, int scale)
{
  const int advance = 4 * scale;
  const int width = static_cast<int>(text.size()) * advance - scale;
  const int height = 5 * scale;
  const int left = (image.width() - width) / 2;
  const int top = (image.height() - height) / 2;
  fillRect(image,
           QRect(left - 2 * scale, top - 2 * scale, width + 4 * scale,
                 height + 4 * scale),
           qRgba(0, 0, 0, 255));
  int x = left;
  for (const QChar c : text) {
    const int index = c == QLatin1Char('/') ? 10 : c.digitValue();
    if (index >= 0 && index < static_cast<int>(glyphs.size())) {
      const Glyph& glyph = glyphs.at(static_cast<std::size_t>(index));
      for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 3; ++column) {
          if ((glyph.at(static_cast<std::size_t>(row)) >> (2 - column) & 1U) !=
              0U) {
            fillRect(image,
                     QRect(x + column * scale, top + row * scale, scale, scale),
                     qRgba(255, 255, 255, 255));
          }
        }
      }
    }
    x += advance;
  }
}

/** Light and dark squares; the hue changes with the level. */
std::array<QRgb, 2> checkerColors(int level)
{
  static constexpr std::array<std::array<QRgb, 2>, 4> palette = {{
      {qRgba(214, 226, 240, 255), qRgba(96, 128, 168, 255)},
      {qRgba(222, 236, 214, 255), qRgba(104, 150, 96, 255)},
      {qRgba(240, 226, 210, 255), qRgba(176, 128, 88, 255)},
      {qRgba(228, 216, 238, 255), qRgba(132, 106, 170, 255)},
  }};
  return palette.at(static_cast<std::size_t>(level % 4));
}

} // namespace

// --- SolidColorImageryProvider -----------------------------------------------

SolidColorImageryProvider::SolidColorImageryProvider(const QColor& color,
                                                     int tileSizePx,
                                                     int maximumLevel)
    : m_color(color), m_tileSizePx(std::clamp(tileSizePx, 1, 4096)),
      m_maximumLevel(std::clamp(maximumLevel, 0, TileKey::maximumLevel))
{
}

QString SolidColorImageryProvider::id() const
{
  return QStringLiteral("test:solid:") + m_color.name(QColor::HexArgb) +
         QStringLiteral(":%1:%2").arg(m_tileSizePx).arg(m_maximumLevel);
}

QString SolidColorImageryProvider::name() const
{
  return QCoreApplication::translate("ts::TestProviders", "Solid colour %1")
      .arg(m_color.name(QColor::HexArgb));
}

QImage SolidColorImageryProvider::loadImage(TileKey key,
                                            const std::atomic<bool>& cancelled)
{
  if (!key.isValid() || key.level > m_maximumLevel || cancelled.load()) {
    return QImage();
  }
  QImage image(m_tileSizePx, m_tileSizePx, QImage::Format_RGBA8888);
  image.fill(m_color);
  return image;
}

// --- CheckerboardImageryProvider ---------------------------------------------

CheckerboardImageryProvider::CheckerboardImageryProvider(int tileSizePx,
                                                         int maximumLevel)
    : m_tileSizePx(std::clamp(tileSizePx, 16, 4096)),
      m_maximumLevel(std::clamp(maximumLevel, 0, TileKey::maximumLevel))
{
}

QString CheckerboardImageryProvider::id() const
{
  return QStringLiteral("test:checkerboard:%1:%2")
      .arg(m_tileSizePx)
      .arg(m_maximumLevel);
}

QString CheckerboardImageryProvider::name() const
{
  return QCoreApplication::translate("ts::TestProviders", "Checkerboard");
}

QImage
CheckerboardImageryProvider::loadImage(TileKey key,
                                       const std::atomic<bool>& cancelled)
{
  if (!key.isValid() || key.level > m_maximumLevel || cancelled.load()) {
    return QImage();
  }
  const int size = m_tileSizePx;
  QImage image(size, size, QImage::Format_RGBA8888);
  const std::array<QRgb, 2> colors = checkerColors(key.level);
  const int squares = 8;
  for (int y = 0; y < size; ++y) {
    const int row = y * squares / size;
    for (int x = 0; x < size; ++x) {
      const int column = x * squares / size;
      image.setPixel(x, y,
                     colors.at(static_cast<std::size_t>((row + column) % 2)));
    }
  }
  const QRgb outline = qRgba(40, 40, 40, 255);
  fillRect(image, QRect(0, 0, size, 1), outline);
  fillRect(image, QRect(0, size - 1, size, 1), outline);
  fillRect(image, QRect(0, 0, 1, size), outline);
  fillRect(image, QRect(size - 1, 0, 1, size), outline);
  drawLabel(image, key.toString(), std::max(1, size / 64));
  return image;
}

} // namespace ts
