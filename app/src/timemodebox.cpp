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
#include "timemodebox.h"

#include <cstdint>

#include <QEvent>

namespace ts
{

TimeModeBox::TimeModeBox(QWidget* parent) : QComboBox(parent)
{
  setExtent(TimeExtent());
}

void TimeModeBox::setExtent(const TimeExtent& extent)
{
  const QSignalBlocker blocker(this);
  clear();
  addItem(QString(), static_cast<int32_t>(LayerTime::Mode::Clock));
  if (LayerTime::isMonthly(extent)) {
    addItem(QString(), static_cast<int32_t>(LayerTime::Mode::MonthOfYear));
  }
  addItem(QString(), static_cast<int32_t>(LayerTime::Mode::Fixed));
  retranslateUi();
  setCurrentIndex(0);
}

LayerTime::Mode TimeModeBox::mode() const
{
  return static_cast<LayerTime::Mode>(currentData().toInt());
}

bool TimeModeBox::setMode(LayerTime::Mode mode)
{
  const int32_t index = findData(static_cast<int32_t>(mode));
  if (index < 0) {
    return false;
  }
  setCurrentIndex(index);
  return true;
}

void TimeModeBox::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QComboBox::changeEvent(event);
}

void TimeModeBox::retranslateUi()
{
  for (int32_t i = 0; i < count(); ++i) {
    switch (static_cast<LayerTime::Mode>(itemData(i).toInt())) {
    case LayerTime::Mode::Clock:
      setItemText(i, tr("Follow the program's clock"));
      break;
    case LayerTime::Mode::MonthOfYear:
      setItemText(i, tr("The clock's month, any year"));
      break;
    case LayerTime::Mode::Fixed:
      setItemText(i, tr("Always this time:"));
      break;
    }
  }
}

} // namespace ts
