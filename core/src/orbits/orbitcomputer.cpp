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
#include "ts/orbits/orbitcomputer.h"

#include <chrono>
#include <utility>

#include <QMetaObject>

namespace ts
{

OrbitComputer::OrbitComputer(QObject* parent)
    : QObject(parent), m_thread([this]() { run(); })
{
}

OrbitComputer::~OrbitComputer()
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = true;
    m_pending.reset();
  }
  m_condition.notify_all();
  m_thread.join();
}

void OrbitComputer::request(OrbitRequest request)
{
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = std::move(request);
  }
  m_condition.notify_all();
}

std::shared_ptr<const OrbitResult> OrbitComputer::result() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_result;
}

bool OrbitComputer::isBusy() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_pending.has_value() || m_computing;
}

bool OrbitComputer::waitForIdle(int32_t timeoutMs) const
{
  std::unique_lock<std::mutex> lock(m_mutex);
  return m_condition.wait_for(
      lock, std::chrono::milliseconds(timeoutMs),
      [this]() { return !m_pending.has_value() && !m_computing; });
}

OrbitResult OrbitComputer::compute(
    const OrbitRequest& request,
    std::map<const Sgp4*, std::unique_ptr<TrackSampler>>& samplers)
{
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  OrbitResult result;
  result.generation = request.generation;
  result.time = request.time;
  result.states.reserve(request.tasks.size());
  result.tracks.resize(request.tasks.size());
  std::map<const Sgp4*, std::unique_ptr<TrackSampler>> kept;
  for (std::size_t i = 0; i < request.tasks.size(); ++i) {
    const OrbitTask& task = request.tasks[i];
    result.states.push_back(satelliteState(*task.sgp4, request.time));
    if (!task.track) {
      continue;
    }
    std::map<const Sgp4*, std::unique_ptr<TrackSampler>>::iterator it =
        samplers.find(task.sgp4.get());
    std::unique_ptr<TrackSampler> sampler =
        it != samplers.end() && it->second
            ? std::move(it->second)
            : std::make_unique<TrackSampler>(task.sgp4);
    result.tracks[i] =
        sampler->track(request.time, task.pastMinutes, task.futureMinutes);
    kept[task.sgp4.get()] = std::move(sampler);
  }
  samplers = std::move(kept);
  result.computeMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  return result;
}

void OrbitComputer::announceResult()
{
  emit resultReady();
}

void OrbitComputer::run()
{
  std::map<const Sgp4*, std::unique_ptr<TrackSampler>> samplers;
  for (;;) {
    OrbitRequest request;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_condition.wait(lock,
                       [this]() { return m_stop || m_pending.has_value(); });
      if (m_stop) {
        return;
      }
      if (!m_pending) {
        continue;
      }
      request = std::move(*m_pending);
      m_pending.reset();
      m_computing = true;
    }
    std::shared_ptr<const OrbitResult> result =
        std::make_shared<const OrbitResult>(compute(request, samplers));
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      m_result = std::move(result);
      m_computing = false;
    }
    m_condition.notify_all();
    // By name, not with a lambda: the queued call is then made and freed
    // inside Qt, whose event queue's locks ThreadSanitizer can't see, so
    // nothing of ours crosses the threads unseen (a lambda's QCallableObject
    // is our code, written here and read on the GUI thread).
    QMetaObject::invokeMethod(this, "announceResult", Qt::QueuedConnection);
  }
}

} // namespace ts
