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
 * Start-up time, memory and idle-CPU measurement for the performance
 * harness.
 */
#ifndef TS_APP_STARTUPMEASUREMENT_H
#define TS_APP_STARTUPMEASUREMENT_H

#include <functional>

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QEvent;
class QWidget;

namespace ts
{

/**
 * Measures how long the program takes from main() to the first paint of the
 * main window, the memory in use at that point, and optionally the CPU used
 * while the window then sits idle.
 *
 * Used by `terrasoar --measure`; see docs/testing.md for the JSON format.
 *
 * @guithread
 */
class StartupMeasurement : public QObject
{
  Q_OBJECT

public:
  /**
   * Prepares a measurement.
   *
   * @param sinceMain Timer started at the top of main().
   * @param window The main window; its first paint ends the start-up phase.
   * @param idleS Seconds to wait idle after start-up before finishing; 0 to
   *        finish straight after the first paint.
   * @param parent Usual QObject parent.
   */
  StartupMeasurement(QElapsedTimer sinceMain, QWidget* window, int idleS,
                     QObject* parent = nullptr);

  /** Starts watching the window. Call before the window is shown. */
  void start();

  /**
   * Sets a check for one-off start-up work running in the background
   * (the base pack's checksums): the idle period starts once it returns
   * false, so it measures the program at rest.
   */
  void setBackgroundBusy(std::function<bool()> busy);

  /**
   * Sets a function whose result is added to the JSON as "state" when the
   * measurement finishes (what the window shows; used by the offline smoke
   * test).
   */
  void setStateReport(std::function<QJsonObject()> report);

  /** Returns the results; complete once finished() has been emitted. */
  QJsonObject result() const { return m_result; }

  /** Returns true once the measurement is complete. */
  bool isFinished() const { return m_finished; }

signals:
  /** Emitted once, when every value has been measured. */
  void finished();

protected:
  /** Watches for the window's first paint. */
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void onStartupComplete();
  void startIdle();
  void onIdleComplete();
  void finish();

  QElapsedTimer m_sinceMain;
  QPointer<QWidget> m_window;
  int m_idleS = 0;
  std::function<bool()> m_backgroundBusy;
  std::function<QJsonObject()> m_stateReport;
  bool m_painted = false;
  bool m_finished = false;
  double m_idleStartCpuS = 0.0;
  QElapsedTimer m_idleTimer;
  QJsonObject m_result;
};

} // namespace ts

#endif // TS_APP_STARTUPMEASUREMENT_H
