// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Rolling frame-time statistics.
 */
#ifndef TS_PERF_FRAMETIMESTATS_H
#define TS_PERF_FRAMETIMESTATS_H

#include <cstddef>
#include <vector>

namespace ts
{

/**
 * Keeps the last N frame durations and reports mean, percentiles and frame
 * rate over them. Used by the debug overlay and the performance harness.
 *
 * All durations are in seconds.
 *
 * @reentrant
 */
class FrameTimeStats
{
public:
  /**
   * Creates an empty window.
   *
   * @param capacity Number of most recent frames kept; at least 1.
   */
  explicit FrameTimeStats(std::size_t capacity = 240);

  /** Adds one frame duration in seconds. Negative values count as 0. */
  void addFrame(double durationS);

  /** Forgets every frame. */
  void clear();

  /** Returns the number of frames held (at most capacity()). */
  std::size_t count() const { return m_size; }

  /** Returns the window size given to the constructor. */
  std::size_t capacity() const { return m_samplesS.size(); }

  /** Returns the total number of frames added since the last clear(). */
  std::size_t totalFrames() const { return m_total; }

  /** Returns the mean frame duration in seconds, 0 if empty. */
  double meanS() const;

  /** Returns the longest frame duration in seconds, 0 if empty. */
  double maxS() const;

  /**
   * Returns the @p p-th percentile frame duration in seconds (nearest-rank),
   * 0 if empty.
   *
   * @param p Percentile in [0, 100]; values outside are clamped.
   */
  double percentileS(double p) const;

  /** Returns frames per second from the mean duration, 0 if empty. */
  double meanFps() const;

private:
  std::vector<double> m_samplesS;
  std::size_t m_next = 0;
  std::size_t m_size = 0;
  std::size_t m_total = 0;
};

} // namespace ts

#endif // TS_PERF_FRAMETIMESTATS_H
