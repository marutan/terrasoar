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

#ifndef TS_TILING_MOTIONHOLD_H
#define TS_TILING_MOTIONHOLD_H

#include <cstdint>
#include <optional>

#include "ts/maths/vec3.h"

namespace ts
{

class Camera;

/** Settings of MotionHold. */
struct MotionHoldSettings {
  /**
   * Speed above which the camera counts as moving fast, in screen heights
   * per second: how far the eye moves, relative to the height of the
   * ground the view shows at its target.
   */
  double fastScreensPerS = 1.5;
  /** How long the camera must be slower than that before the hold ends. */
  double settleS = 0.15;
};

/**
 * Tells whether the camera moves too fast for detailed tiles to be worth
 * fetching: those would be on screen for a moment only. The renderer then
 * fetches only coarse stand-ins, and the detail once the camera settles.
 *
 * The hold starts after two fast frames in a row, so a single jump (going
 * straight to a place, or a test setting the camera) doesn't wait. It
 * ends settleS after the last fast frame.
 *
 * @reentrant
 */
class MotionHold
{
public:
  /** Creates a tracker with @p settings. */
  explicit MotionHold(MotionHoldSettings settings = {});

  /** Notes @p camera as drawn at @p timeS (any steady clock, seconds). */
  void update(const Camera& camera, double timeS);

  /**
   * Returns true while detail should wait, as of the last update(): the
   * camera is moving fast, or did less than settleS before.
   */
  bool isHolding() const { return m_holding; }

  /** Returns the speed at the last update(), screen heights per second. */
  double screensPerS() const { return m_screensPerS; }

private:
  MotionHoldSettings m_settings;
  std::optional<Vec3> m_lastEye;
  double m_lastTimeS = 0.0;
  double m_lastFastS = 0.0;
  int32_t m_fastFrames = 0;
  double m_screensPerS = 0.0;
  bool m_holding = false;
};

} // namespace ts

#endif // TS_TILING_MOTIONHOLD_H
