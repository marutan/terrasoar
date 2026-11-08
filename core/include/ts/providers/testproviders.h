// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Imagery providers that generate their tiles: for tests and development.
 */
#ifndef TS_PROVIDERS_TESTPROVIDERS_H
#define TS_PROVIDERS_TESTPROVIDERS_H

#include <QColor>

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * Every tile is one colour (alpha included, for blending tests).
 *
 * @threadsafe
 */
class SolidColorImageryProvider : public ImageryProvider
{
public:
  /** Creates a provider of @p color tiles of @p tileSizePx pixels. */
  explicit SolidColorImageryProvider(const QColor& color, int tileSizePx = 64,
                                     int maximumLevel = TileKey::maximumLevel);

  QString id() const override;
  QString name() const override;
  int maximumLevel() const override { return m_maximumLevel; }
  int tileSizePx() const override { return m_tileSizePx; }
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  QColor m_color;
  int m_tileSizePx;
  int m_maximumLevel;
};

/**
 * A checkerboard of 8 × 8 squares per tile, with a one-pixel outline and
 * the tile key ("level/x/y") written in the middle, so tile boundaries,
 * levels and orientation are visible on the globe.
 *
 * The text uses a built-in bitmap font, not the system's fonts, so tiles
 * are the same on every platform (golden images) and generating them on
 * worker threads needs no font engine.
 *
 * @threadsafe
 */
class CheckerboardImageryProvider : public ImageryProvider
{
public:
  /** Creates a provider of @p tileSizePx-pixel tiles. */
  explicit CheckerboardImageryProvider(
      int tileSizePx = 256, int maximumLevel = TileKey::maximumLevel);

  QString id() const override;
  QString name() const override;
  int maximumLevel() const override { return m_maximumLevel; }
  int tileSizePx() const override { return m_tileSizePx; }
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  int m_tileSizePx;
  int m_maximumLevel;
};

} // namespace ts

#endif // TS_PROVIDERS_TESTPROVIDERS_H
