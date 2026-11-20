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

#include <QDateTime>
#include <QObject>

namespace ts
{

/**
 * The time the program shows, in UTC: which picture a layer with a time
 * dimension draws (LayerTime), and later the sun's position (Phase 14)
 * and where satellites are (Phase 15). One per main window. It doesn't
 * run by itself; the time slider moves it, and its play button steps it.
 * Saved in projects (view.time).
 *
 * @guithread
 */
class ProgramClock : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ProgramClock)

public:
  /** Starts at the current time. */
  explicit ProgramClock(QObject* parent = nullptr);

  /** Destroys it. */
  ~ProgramClock() override;

  /** Returns the time (UTC). */
  QDateTime time() const { return m_time; }

  /** Sets the time (converted to UTC); timeChanged() if it moved. */
  void setTime(const QDateTime& time);

  /** Sets the current time. */
  void setNow();

signals:
  /** The time moved to @p time (UTC). */
  void timeChanged(const QDateTime& time);

private:
  QDateTime m_time;
};

} // namespace ts

#endif // TS_TIME_PROGRAMCLOCK_H
