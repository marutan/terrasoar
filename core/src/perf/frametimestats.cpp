// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/perf/frametimestats.h"

#include <algorithm>
#include <cmath>

namespace ts
{

FrameTimeStats::FrameTimeStats(std::size_t capacity)
    : m_samplesS(std::max<std::size_t>(capacity, 1), 0.0)
{
}

void FrameTimeStats::addFrame(double durationS)
{
  m_samplesS[m_next] = durationS > 0.0 ? durationS : 0.0;
  m_next = (m_next + 1) % m_samplesS.size();
  m_size = std::min(m_size + 1, m_samplesS.size());
  ++m_total;
}

void FrameTimeStats::clear()
{
  m_next = 0;
  m_size = 0;
  m_total = 0;
}

double FrameTimeStats::meanS() const
{
  if (m_size == 0) {
    return 0.0;
  }
  double sum = 0.0;
  for (std::size_t i = 0; i < m_size; ++i) {
    sum += m_samplesS[i];
  }
  return sum / static_cast<double>(m_size);
}

double FrameTimeStats::maxS() const
{
  if (m_size == 0) {
    return 0.0;
  }
  return *std::max_element(m_samplesS.begin(), m_samplesS.begin() + m_size);
}

double FrameTimeStats::percentileS(double p) const
{
  if (m_size == 0) {
    return 0.0;
  }
  const double clamped = std::clamp(p, 0.0, 100.0);
  std::vector<double> sorted(m_samplesS.begin(), m_samplesS.begin() + m_size);
  // Nearest-rank: the smallest value with at least p% of samples <= it.
  const double rank = std::ceil(clamped / 100.0 * static_cast<double>(m_size));
  const std::size_t index = rank < 1.0 ? 0 : static_cast<std::size_t>(rank) - 1;
  std::nth_element(sorted.begin(), sorted.begin() + index, sorted.end());
  return sorted[index];
}

double FrameTimeStats::meanFps() const
{
  const double mean = meanS();
  return mean > 0.0 ? 1.0 / mean : 0.0;
}

} // namespace ts
