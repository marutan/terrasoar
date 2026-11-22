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

#include "ts/tiling/motionhold.h"

#include <algorithm>

#include "ts/camera/camera.h"

namespace ts
{

MotionHold::MotionHold(MotionHoldSettings settings) : m_settings(settings) {}

void MotionHold::update(const Camera& camera, double timeS)
{
  const Vec3& eye = camera.eyeEcef();
  // The height of the ground shown at the target, metres: what one
  // screen height of movement is.
  const double screenM =
      std::max(2.0 * camera.pose().rangeM * camera.tanHalfFovY(), 1.0);
  if (m_lastEye && timeS > m_lastTimeS) {
    m_screensPerS = length(eye - *m_lastEye) / screenM / (timeS - m_lastTimeS);
  } else if (!m_lastEye) {
    m_screensPerS = 0.0;
  } // else: two updates at one time; keep the last speed
  if (!m_lastEye || timeS > m_lastTimeS) {
    m_lastEye = eye;
    m_lastTimeS = timeS;
    if (m_screensPerS > m_settings.fastScreensPerS) {
      ++m_fastFrames;
      if (m_fastFrames >= 2) {
        m_lastFastS = timeS;
        m_holding = true;
      }
    } else {
      m_fastFrames = 0;
    }
  }
  if (m_holding && m_fastFrames < 2 &&
      timeS - m_lastFastS >= m_settings.settleS) {
    m_holding = false;
  }
}

} // namespace ts
