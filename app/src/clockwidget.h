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
 * The clock in the status bar: the program's time, play and pause,
 * speed, stepping and jumping to a date and time.
 */
#ifndef TS_APP_CLOCKWIDGET_H
#define TS_APP_CLOCKWIDGET_H

#include <vector>

#include <QDateTime>
#include <QDialog>
#include <QWidget>

class QAction;
class QDateTimeEdit;
class QEvent;
class QLabel;
class QMenu;
class QTimer;
class QToolButton;

namespace ts
{

class ProgramClock;

/**
 * Asks for a date and time (UTC) to move the clock to.
 *
 * @guithread
 */
class JumpToTimeDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(JumpToTimeDialog)

public:
  /** Creates the dialog showing @p time. */
  JumpToTimeDialog(const QDateTime& time, QWidget* parent);

  /** Destroys it. */
  ~JumpToTimeDialog() override;

  /** Returns the time chosen (UTC). */
  QDateTime time() const;

  /** Sets the time shown (for tests). */
  void setTime(const QDateTime& time);

private:
  QDateTimeEdit* m_edit = nullptr;
};

/**
 * The program's clock in the status bar: step back, play or pause, step
 * forward, the time (UTC; clicking it jumps to a date and time), the
 * speed (a menu of speeds, faster and slower) and Now (real time).
 *
 * Its actions are also in the Tools menu's Clock submenu. While the clock
 * runs the time shown is updated a few times a second; while it is paused
 * nothing runs.
 *
 * @guithread
 */
class ClockWidget : public QWidget
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ClockWidget)

public:
  /** Creates the widget for @p clock, which must outlive it. */
  ClockWidget(ProgramClock* clock, QWidget* parent);

  /** Destroys it. */
  ~ClockWidget() override;

  /** Returns the Play/Pause action. */
  QAction* playAction() const { return m_playAction; }
  /** Returns the Step Forward action. */
  QAction* stepForwardAction() const { return m_forwardAction; }
  /** Returns the Step Back action. */
  QAction* stepBackAction() const { return m_backAction; }
  /** Returns the Faster action. */
  QAction* fasterAction() const { return m_fasterAction; }
  /** Returns the Slower action. */
  QAction* slowerAction() const { return m_slowerAction; }
  /** Returns the Real Time (Now) action. */
  QAction* realTimeAction() const { return m_realTimeAction; }
  /** Returns the Jump to Date and Time action. */
  QAction* jumpAction() const { return m_jumpAction; }

  /** Returns the step of Step Forward and Step Back, seconds. */
  double stepSeconds() const { return m_stepSeconds; }

  /** Sets the step of Step Forward and Step Back, seconds. */
  void setStepSeconds(double seconds);

  /** Returns the time shown (for tests). */
  QString timeText() const;

  /** Returns the speed shown (for tests). */
  QString rateText() const;

  /** Returns @p rate as the speed is shown: "×60", "×−1", "×0.5". */
  static QString rateLabel(double rate);

  /**
   * Opens the Jump to Date and Time dialog and moves the clock to what is
   * chosen (running on if it ran).
   */
  void jump();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void updateState();
  void updateTime();
  void togglePlay();
  void buildMenus();
  void retranslateMenus();

  ProgramClock* m_clock;
  QToolButton* m_backButton = nullptr;
  QToolButton* m_playButton = nullptr;
  QToolButton* m_forwardButton = nullptr;
  QToolButton* m_timeButton = nullptr;
  QToolButton* m_rateButton = nullptr;
  QToolButton* m_nowButton = nullptr;
  QMenu* m_rateMenu = nullptr;
  QMenu* m_stepMenu = nullptr;
  /** The speeds in the menu. */
  std::vector<QAction*> m_rateActions;
  /** The steps in its Step submenu. */
  std::vector<QAction*> m_stepActions;
  QAction* m_playAction = nullptr;
  QAction* m_forwardAction = nullptr;
  QAction* m_backAction = nullptr;
  QAction* m_fasterAction = nullptr;
  QAction* m_slowerAction = nullptr;
  QAction* m_realTimeAction = nullptr;
  QAction* m_jumpAction = nullptr;
  QTimer* m_refresh = nullptr;
  double m_stepSeconds = 60.0;
};

} // namespace ts

#endif // TS_APP_CLOCKWIDGET_H
