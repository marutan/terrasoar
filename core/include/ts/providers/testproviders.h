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

/**
 * @file
 * Imagery providers that generate their tiles: for tests and development.
 */
#ifndef TS_PROVIDERS_TESTPROVIDERS_H
#define TS_PROVIDERS_TESTPROVIDERS_H

#include <cstdint>

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
  explicit SolidColorImageryProvider(
      const QColor& color, int32_t tileSizePx = 64,
      int32_t maximumLevel = TileKey::maximumLevel);

  QString id() const override;
  QString name() const override;
  int32_t maximumLevel() const override { return m_maximumLevel; }
  int32_t tileSizePx() const override { return m_tileSizePx; }
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  QColor m_color;
  int32_t m_tileSizePx;
  int32_t m_maximumLevel;
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
      int32_t tileSizePx = 256, int32_t maximumLevel = TileKey::maximumLevel);

  QString id() const override;
  QString name() const override;
  int32_t maximumLevel() const override { return m_maximumLevel; }
  int32_t tileSizePx() const override { return m_tileSizePx; }
  QImage loadImage(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  int32_t m_tileSizePx;
  int32_t m_maximumLevel;
};

} // namespace ts

#endif // TS_PROVIDERS_TESTPROVIDERS_H
