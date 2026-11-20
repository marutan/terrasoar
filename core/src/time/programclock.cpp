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
#include "ts/time/programclock.h"

namespace ts
{

ProgramClock::ProgramClock(QObject* parent)
    : QObject(parent), m_time(QDateTime::currentDateTimeUtc())
{
}

void ProgramClock::setTime(const QDateTime& time)
{
  if (!time.isValid()) {
    return;
  }
  const QDateTime utc = time.toUTC();
  if (utc == m_time) {
    return;
  }
  m_time = utc;
  emit timeChanged(m_time);
}

void ProgramClock::setNow()
{
  setTime(QDateTime::currentDateTimeUtc());
}

ProgramClock::~ProgramClock() = default;

} // namespace ts
