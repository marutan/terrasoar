// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Limits redraws to the screen's refresh rate.
 */
#ifndef TS_RENDER_FRAMEPACER_H
#define TS_RENDER_FRAMEPACER_H

namespace ts
{

/**
 * Decides when the next frame may start so that frames never come faster
 * than the screen refreshes.
 *
 * With vsync the swap already waits for the screen; this covers the cases
 * without it (remote desktop, some software renderers) and keeps input
 * that arrives faster than the refresh rate (a 1000 Hz mouse) from
 * rendering frames nobody sees. Times are seconds on any monotonic clock.
 *
 * @reentrant
 */
class FramePacer
{
public:
  /** Rate used when the screen doesn't report one, frames per second. */
  static constexpr double defaultRefreshRateHz = 60.0;

  /**
   * Sets the screen's refresh rate. Values ≤ 0 or above 1000 (unknown or
   * bogus) select defaultRefreshRateHz.
   */
  void setRefreshRateHz(double rateHz);

  /** Returns the shortest time between frame starts, seconds. */
  double minimumIntervalS() const { return m_intervalS; }

  /** Records that a frame started at @p nowS. */
  void frameStarted(double nowS);

  /**
   * Returns how long to wait from @p nowS before the next frame may
   * start: 0 if a whole interval has passed since the last one.
   */
  double delayS(double nowS) const;

private:
  double m_intervalS = 1.0 / defaultRefreshRateHz;
  double m_lastFrameS = -1.0e30;
};

} // namespace ts

#endif // TS_RENDER_FRAMEPACER_H
