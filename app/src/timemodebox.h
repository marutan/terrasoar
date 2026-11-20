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
 * The box choosing how a layer's time follows the program's clock, shown
 * in the Add WMS and Add WMTS dialogs for layers with a time dimension.
 */
#ifndef TS_APP_TIMEMODEBOX_H
#define TS_APP_TIMEMODEBOX_H

#include <QComboBox>

#include "ts/time/layertime.h"
#include "ts/time/timeextent.h"

class QEvent;
class QWidget;

namespace ts
{

/**
 * Offers the modes of LayerTime for a layer's times: follow the program's
 * clock (chosen at first); the clock's month in any year, for monthly
 * times such as Blue Marble's (LayerTime::isMonthly()); or one value,
 * chosen in the dialog's value box.
 *
 * @guithread
 */
class TimeModeBox : public QComboBox
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(TimeModeBox)

public:
  /** Creates the box with the modes for no times (clock and fixed). */
  explicit TimeModeBox(QWidget* parent);

  /** Destroys it. */
  ~TimeModeBox() override;

  /** Offers the modes that suit @p extent, and chooses the clock. */
  void setExtent(const TimeExtent& extent);

  /** Returns the chosen mode. */
  LayerTime::Mode mode() const;

  /** Chooses @p mode; false if it isn't offered. */
  bool setMode(LayerTime::Mode mode);

protected:
  /** Re-applies the texts on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
};

} // namespace ts

#endif // TS_APP_TIMEMODEBOX_H
