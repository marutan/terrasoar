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
 * The time slider: moves the program's clock through the times the
 * layers have, and plays them.
 */
#ifndef TS_APP_TIMEBAR_H
#define TS_APP_TIMEBAR_H

#include <cstdint>
#include <vector>

#include <QDateTime>
#include <QToolBar>

#include "ts/time/timeextent.h"

class QAction;
class QComboBox;
class QEvent;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;
class QWidget;

namespace ts
{

class ProgramClock;

/**
 * A tool bar under the view: a slider over the times of the layers that
 * follow the clock (setExtent()), the clock's time, buttons for the
 * previous and next time, and a play button that steps through them at
 * the chosen speed until the last. Moving the slider sets the clock to
 * the latest of the layers' times not after the slider's place, so the
 * view always shows a time some layer has. With no such layers the
 * slider and buttons are disabled.
 *
 * See the user guide's Time section (docs/user/layers.md).
 *
 * @guithread
 */
class TimeBar : public QToolBar
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(TimeBar)

public:
  /** Creates the bar for @p clock (which must outlive it). */
  TimeBar(ProgramClock* clock, QWidget* parent);

  /**
   * Sets the times to move through (the layers' times joined); an empty
   * extent disables the bar.
   */
  void setExtent(const TimeExtent& extent);

  /** Returns the times the bar moves through. */
  const TimeExtent& extent() const { return m_extent; }

  /** Returns true while playing. */
  bool isPlaying() const;

  /** Plays from the clock's time (from the first if at the last). */
  void play();

  /** Stops playing. */
  void pause();

  /** Moves the clock to the next time; false if there is none. */
  bool stepForward();

  /** Moves the clock to the previous time; false if there is none. */
  bool stepBack();

  /** Returns the time shown, without "UTC" (for tests). */
  QString timeText() const;

  /** Returns the slider (for tests). */
  QSlider* slider() const { return m_slider; }

  /** Sets how long each time is shown while playing, ms. */
  void setStepIntervalMs(int32_t intervalMs);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void clockMoved();
  void sliderMoved(int32_t position);
  void tick();
  void updateButtons();
  QDateTime timeAtPosition(int32_t position) const;
  int32_t positionOf(const QDateTime& time) const;

  ProgramClock* m_clock;
  QToolButton* m_backButton = nullptr;
  QToolButton* m_playButton = nullptr;
  QToolButton* m_forwardButton = nullptr;
  QSlider* m_slider = nullptr;
  QLabel* m_timeLabel = nullptr;
  QComboBox* m_speedBox = nullptr;
  QTimer* m_playTimer = nullptr;
  /** The tool bar's actions for its widgets (named for menus and tests). */
  std::vector<QAction*> m_widgetActions;
  /** The time shown, as the layers' extent writes it. */
  QString m_timeText;
  TimeExtent m_extent;
  /** Each time, when there are few enough for a slider step each. */
  std::vector<QDateTime> m_values;
};

} // namespace ts

#endif // TS_APP_TIMEBAR_H
