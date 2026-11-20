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
 * The program's clock: the time the layers show.
 */
#ifndef TS_TIME_PROGRAMCLOCK_H
#define TS_TIME_PROGRAMCLOCK_H

#include <cstdint>
#include <vector>

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>

class QTimer;

namespace ts
{

/**
 * The time the program shows, in UTC: which picture a layer with a time
 * dimension draws (LayerTime), where satellites are (Phase 15) and later
 * the sun's position (Phase 14). One per main window. Saved in projects
 * (view.time and view.clock).
 *
 * It runs at a rate (1 is real time, 60 a minute a second, negative
 * backwards) or is paused. It starts in real time: running at rate 1
 * from now. Setting a time pauses it there (a time chosen on the time
 * slider stays); play() runs it on from there.
 *
 * While running, time() moves on by itself, but timeChanged() comes only
 * every tick interval and only while ticking is on (setTicking()): what
 * moves with the clock (satellites) turns it on, so a window showing
 * nothing that moves stays idle.
 *
 * @guithread
 */
class ProgramClock : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ProgramClock)

public:
  /** Starts in real time. */
  explicit ProgramClock(QObject* parent = nullptr);

  /** Destroys it. */
  ~ProgramClock() override;

  /** Returns the time (UTC), moved on by the rate while running. */
  QDateTime time() const;

  /**
   * Sets the time (converted to UTC) and pauses there; timeChanged() if it
   * moved. Invalid times are ignored.
   */
  void setTime(const QDateTime& time);

  /** Sets the current time (pausing, as setTime()). */
  void setNow();

  /** Runs in real time: now, rate 1, running. */
  void setRealTime();

  /** Returns true while it follows real time (setRealTime(), unchanged). */
  bool isRealTime() const { return m_realTime; }

  /** Returns true while running. */
  bool isRunning() const { return m_running; }

  /** Runs on from the time at the rate. */
  void play();

  /** Stops at the time. */
  void pause();

  /** Returns the rate: seconds of the clock per second. */
  double rate() const { return m_rate; }

  /**
   * Sets the rate (not 0; clamped to ±1 000 000), keeping the time and
   * whether it runs. Anything but 1 leaves real time.
   */
  void setRate(double rate);

  /** Returns the next faster rate of rates() (the fastest stays). */
  double fasterRate() const;

  /** Returns the next slower rate of rates() (the slowest stays). */
  double slowerRate() const;

  /** Returns the rates offered, slowest (most backwards) first. */
  static const std::vector<double>& rates();

  /** Moves the time by @p seconds and pauses (as setTime()). */
  void step(double seconds);

  /** Turns ticking on or off: timeChanged() while running (see class). */
  void setTicking(bool ticking);

  /** Returns true if ticking is on. */
  bool isTicking() const { return m_ticking; }

  /** Sets the tick interval, milliseconds (default 100). */
  void setTickIntervalMs(int32_t intervalMs);

  /** Returns the tick interval, milliseconds. */
  int32_t tickIntervalMs() const;

signals:
  /** The time moved to @p time (UTC). */
  void timeChanged(const QDateTime& time);

  /** It started or stopped running, or its rate changed. */
  void runningChanged();

private:
  void anchor(const QDateTime& time);
  void updateTimer();
  void tick();

  /** The time when last anchored (setTime(), play(), setRate()). */
  QDateTime m_anchorTime;
  /** Measures the time since then. */
  QElapsedTimer m_sinceAnchor;
  /** The time last announced in timeChanged(). */
  QDateTime m_announced;
  double m_rate = 1.0;
  bool m_running = true;
  bool m_realTime = true;
  bool m_ticking = false;
  QTimer* m_timer = nullptr;
};

} // namespace ts

#endif // TS_TIME_PROGRAMCLOCK_H
