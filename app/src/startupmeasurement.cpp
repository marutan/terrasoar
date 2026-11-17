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

#include "startupmeasurement.h"

#include <utility>

#include <QEvent>
#include <QGuiApplication>
#include <QTimer>
#include <QWidget>

#include "ts/base/buildinfo.h"
#include "ts/perf/processstats.h"

namespace ts
{

StartupMeasurement::StartupMeasurement(QElapsedTimer sinceMain, QWidget* window,
                                       int idleS, QObject* parent)
    : QObject(parent), m_sinceMain(sinceMain), m_window(window),
      m_idleS(idleS > 0 ? idleS : 0)
{
}

void StartupMeasurement::setBackgroundBusy(std::function<bool()> busy)
{
  m_backgroundBusy = std::move(busy);
}

void StartupMeasurement::start()
{
  if (m_window) {
    m_window->installEventFilter(this);
  }
}

bool StartupMeasurement::eventFilter(QObject* watched, QEvent* event)
{
  if (!m_painted && watched == m_window && event->type() == QEvent::Paint) {
    m_painted = true;
    // Let the paint finish and the frame be flushed before measuring.
    QTimer::singleShot(0, this, &StartupMeasurement::onStartupComplete);
  }
  return QObject::eventFilter(watched, event);
}

void StartupMeasurement::onStartupComplete()
{
  if (m_window) {
    m_window->removeEventFilter(this);
  }
  const ProcessStats stats = currentProcessStats();
  const BuildInfo info = buildInfo();
  m_result.insert(QStringLiteral("startup_ms"),
                  static_cast<double>(m_sinceMain.nsecsElapsed()) / 1e6);
  m_result.insert(QStringLiteral("resident_bytes"),
                  static_cast<double>(stats.residentBytes));
  m_result.insert(QStringLiteral("peak_resident_bytes"),
                  static_cast<double>(stats.peakResidentBytes));
  m_result.insert(QStringLiteral("startup_cpu_s"), stats.cpuTimeS);
  m_result.insert(QStringLiteral("platform"), QGuiApplication::platformName());
  m_result.insert(QStringLiteral("version"), info.version);
  m_result.insert(QStringLiteral("qt_runtime_version"), info.qtRuntimeVersion);
  m_result.insert(QStringLiteral("compiler"), info.compiler);

  if (m_idleS == 0) {
    finish();
    return;
  }
  startIdle();
}

void StartupMeasurement::startIdle()
{
  if (m_backgroundBusy && m_backgroundBusy()) {
    QTimer::singleShot(20, this, &StartupMeasurement::startIdle);
    return;
  }
  m_idleStartCpuS = currentProcessStats().cpuTimeS;
  m_idleTimer.start();
  QTimer::singleShot(m_idleS * 1000, this, &StartupMeasurement::onIdleComplete);
}

void StartupMeasurement::onIdleComplete()
{
  const ProcessStats stats = currentProcessStats();
  const double wallS = static_cast<double>(m_idleTimer.nsecsElapsed()) / 1e9;
  const double cpuS = stats.cpuTimeS - m_idleStartCpuS;
  m_result.insert(QStringLiteral("idle_s"), wallS);
  m_result.insert(QStringLiteral("idle_cpu_s"), cpuS);
  m_result.insert(QStringLiteral("idle_cpu_percent"),
                  wallS > 0.0 ? 100.0 * cpuS / wallS : 0.0);
  m_result.insert(QStringLiteral("idle_resident_bytes"),
                  static_cast<double>(stats.residentBytes));
  m_result.insert(QStringLiteral("peak_resident_bytes"),
                  static_cast<double>(stats.peakResidentBytes));
  finish();
}

void StartupMeasurement::setStateReport(std::function<QJsonObject()> report)
{
  m_stateReport = std::move(report);
}

void StartupMeasurement::finish()
{
  if (m_stateReport) {
    m_result.insert(QStringLiteral("state"), m_stateReport());
  }
  m_finished = true;
  emit finished();
}

} // namespace ts
