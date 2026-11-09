// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * How a vector layer is drawn: fill and outline colours, outline width.
 */
#ifndef TS_VECTOR_VECTORSTYLE_H
#define TS_VECTOR_VECTORSTYLE_H

#include <array>

#include <QColor>
#include <QJsonObject>

namespace ts
{

/**
 * The drawing style of a vector layer, the "style" object of its layer
 * definition:
 *
 * ```json
 * {"fill": "#c8b98fff", "outline": "#f2e6bfff", "outlineWidthPx": 1.2}
 * ```
 *
 * Colours are `#rrggbb` or `#rrggbbaa` (straight alpha); a missing colour
 * draws nothing. The graticule layer uses the outline colour and width for
 * its lines.
 *
 * @reentrant
 */
struct VectorStyle {
  /** Polygon fill; fully transparent for none. */
  QColor fill = QColor(0, 0, 0, 0);
  /** Polygon rings and line strings; fully transparent for none. */
  QColor outline = QColor(0, 0, 0, 0);
  /** Outline width in logical pixels. */
  double outlineWidthPx = 1.0;

  /** Returns true if the fill shows (alpha above zero). */
  bool hasFill() const { return fill.isValid() && fill.alpha() > 0; }

  /** Returns true if the outline shows (alpha and width above zero). */
  bool hasOutline() const
  {
    return outline.isValid() && outline.alpha() > 0 && outlineWidthPx > 0.0;
  }

  /** Reads a style object; missing members keep their defaults. */
  static VectorStyle fromJson(const QJsonObject& json);

  /** Returns the style object. */
  QJsonObject toJson() const;
};

/**
 * Parses `#rrggbb` or `#rrggbbaa` (alpha last, unlike QColor's `#aarrggbb`).
 * Returns an invalid colour for anything else.
 */
QColor colorFromRgbaHex(const QString& text);

/** Returns @p color as `#rrggbbaa`. */
QString colorToRgbaHex(const QColor& color);

/**
 * Returns @p color as premultiplied RGBA floats in [0, 1], its alpha
 * multiplied by @p opacity first.
 */
std::array<float, 4> premultipliedRgba(const QColor& color, double opacity);

} // namespace ts

#endif // TS_VECTOR_VECTORSTYLE_H
