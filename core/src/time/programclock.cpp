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

#include <algorithm>
#include <cmath>

#include <QTimer>

namespace ts
{

ProgramClock::ProgramClock(QObject* parent)
    : QObject(parent), m_anchorTime(QDateTime::currentDateTimeUtc()),
      m_announced(m_anchorTime), m_timer(new QTimer(this))
{
  m_sinceAnchor.start();
  m_timer->setInterval(100);
  connect(m_timer, &QTimer::timeout, this, &ProgramClock::tick);
}

ProgramClock::~ProgramClock() = default;

QDateTime ProgramClock::time() const
{
  if (!m_running) {
    return m_anchorTime;
  }
  if (m_realTime) {
    return QDateTime::currentDateTimeUtc();
  }
  const double elapsedMs =
      static_cast<double>(m_sinceAnchor.elapsed()) * m_rate;
  return m_anchorTime.addMSecs(static_cast<qint64>(std::llround(elapsedMs)));
}

void ProgramClock::anchor(const QDateTime& time)
{
  m_anchorTime = time;
  m_sinceAnchor.start();
}

void ProgramClock::setTime(const QDateTime& time)
{
  if (!time.isValid()) {
    return;
  }
  const QDateTime utc = time.toUTC();
  const bool wasRunning = m_running;
  m_running = false;
  m_realTime = false;
  anchor(utc);
  updateTimer();
  if (wasRunning) {
    emit runningChanged();
  }
  if (utc != m_announced) {
    m_announced = utc;
    emit timeChanged(utc);
  }
}

void ProgramClock::setNow()
{
  setTime(QDateTime::currentDateTimeUtc());
}

void ProgramClock::setRealTime()
{
  const bool changed = !m_running || m_rate != 1.0 || !m_realTime;
  m_rate = 1.0;
  m_running = true;
  m_realTime = true;
  anchor(QDateTime::currentDateTimeUtc());
  updateTimer();
  if (changed) {
    emit runningChanged();
  }
  tick();
}

void ProgramClock::play()
{
  if (m_running) {
    return;
  }
  anchor(m_anchorTime);
  m_running = true;
  updateTimer();
  emit runningChanged();
}

void ProgramClock::pause()
{
  if (!m_running) {
    return;
  }
  anchor(time());
  m_running = false;
  m_realTime = false;
  updateTimer();
  emit runningChanged();
  tick();
}

void ProgramClock::setRate(double rate)
{
  if (rate == 0.0 || !std::isfinite(rate)) {
    return;
  }
  rate = std::clamp(rate, -1e6, 1e6);
  if (rate == m_rate) {
    return;
  }
  anchor(time());
  m_rate = rate;
  m_realTime = false;
  emit runningChanged();
}

const std::vector<double>& ProgramClock::rates()
{
  static const std::vector<double> list{
      -86400.0, -3600.0, -600.0, -60.0, -10.0, -1.0,  0.1,   0.5,    1.0,
      2.0,      5.0,     10.0,   30.0,  60.0,  300.0, 600.0, 3600.0, 86400.0};
  return list;
}

double ProgramClock::fasterRate() const
{
  for (const double r : rates()) {
    if (r > m_rate) {
      return r;
    }
  }
  return rates().back();
}

double ProgramClock::slowerRate() const
{
  const std::vector<double>& list = rates();
  for (std::vector<double>::const_reverse_iterator it = list.rbegin();
       it != list.rend(); ++it) {
    if (*it < m_rate) {
      return *it;
    }
  }
  return list.front();
}

void ProgramClock::step(double seconds)
{
  setTime(time().addMSecs(static_cast<qint64>(std::llround(seconds * 1000.0))));
}

void ProgramClock::setTicking(bool ticking)
{
  if (ticking == m_ticking) {
    return;
  }
  m_ticking = ticking;
  updateTimer();
  if (ticking) {
    tick();
  }
}

void ProgramClock::setTickIntervalMs(int32_t intervalMs)
{
  m_timer->setInterval(std::max(intervalMs, 1));
}

int32_t ProgramClock::tickIntervalMs() const
{
  return m_timer->interval();
}

void ProgramClock::updateTimer()
{
  if (m_running && m_ticking) {
    m_timer->start();
  } else {
    m_timer->stop();
  }
}

void ProgramClock::tick()
{
  const QDateTime now = time();
  if (now != m_announced) {
    m_announced = now;
    emit timeChanged(now);
  }
}

} // namespace ts
