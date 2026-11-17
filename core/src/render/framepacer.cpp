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

#include "ts/render/framepacer.h"

#include <algorithm>

namespace ts
{

void FramePacer::setRefreshRateHz(double rateHz)
{
  const double rate =
      rateHz > 0.0 && rateHz <= 1000.0 ? rateHz : defaultRefreshRateHz;
  m_intervalS = 1.0 / rate;
}

void FramePacer::frameStarted(double nowS)
{
  m_lastFrameS = nowS;
}

double FramePacer::delayS(double nowS) const
{
  return std::max(0.0, m_lastFrameS + m_intervalS - nowS);
}

} // namespace ts
