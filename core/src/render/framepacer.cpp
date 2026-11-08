// SPDX-License-Identifier: GPL-2.0-or-later
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
