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
 * Computing where many satellites are, and their tracks, off the GUI
 * thread.
 */
#ifndef TS_ORBITS_ORBITCOMPUTER_H
#define TS_ORBITS_ORBITCOMPUTER_H

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <QObject>

#include "ts/orbits/orbitalelements.h"
#include "ts/orbits/satellitetrack.h"
#include "ts/orbits/sgp4.h"

namespace ts
{

/** One satellite to compute. */
struct OrbitTask {
  /** The satellite; never null. */
  std::shared_ptr<const Sgp4> sgp4;
  /** True to compute its track too. */
  bool track = false;
  /** The past span of its track, minutes. */
  double pastMinutes = 0.0;
  /** The future span of its track, minutes. */
  double futureMinutes = 0.0;
};

/** What to compute: where the satellites are at one time. */
struct OrbitRequest {
  /** A number the caller chooses, handed back with the result. */
  uint64_t generation = 0;
  /** The time (UTC). */
  JulianDate time;
  /** The satellites. */
  std::vector<OrbitTask> tasks;
};

/** What was computed for a request. */
struct OrbitResult {
  /** The request's generation. */
  uint64_t generation = 0;
  /** The time. */
  JulianDate time;
  /** Where each satellite is, in the request's order. */
  std::vector<SatelliteState> states;
  /** Each satellite's track (empty where none was asked for). */
  std::vector<SatelliteTrack> tracks;
  /** How long it took, milliseconds. */
  double computeMs = 0.0;
};

/**
 * Computes OrbitRequests on a thread of its own. Only the latest request
 * matters: one made while another waits replaces it. Each satellite's
 * track keeps its points between requests (TrackSampler), so a moving
 * clock computes only what comes into its spans.
 *
 * @guithread (request() and result(); the computing is on its thread)
 */
class OrbitComputer : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(OrbitComputer)

public:
  /** Starts the thread. */
  explicit OrbitComputer(QObject* parent = nullptr);

  /** Stops the thread (after the request being computed). */
  ~OrbitComputer() override;

  /** Asks for @p request to be computed, replacing one still waiting. */
  void request(OrbitRequest request);

  /** Returns the latest result, or null before the first. */
  std::shared_ptr<const OrbitResult> result() const;

  /** Returns true while a request waits or is being computed. */
  bool isBusy() const;

  /** Waits until idle, at most @p timeoutMs; returns !isBusy(). */
  bool waitForIdle(int32_t timeoutMs) const;

  /**
   * Computes @p request here and now, with @p samplers keeping tracks'
   * points (by satellite; those not asked for are dropped).
   */
  static OrbitResult
  compute(const OrbitRequest& request,
          std::map<const Sgp4*, std::unique_ptr<TrackSampler>>& samplers);

signals:
  /** A new result() is there (emitted on the GUI thread). */
  void resultReady();

private:
  void run();

  mutable std::mutex m_mutex;
  mutable std::condition_variable m_condition;
  std::optional<OrbitRequest> m_pending;
  bool m_computing = false;
  bool m_stop = false;
  std::shared_ptr<const OrbitResult> m_result;
  std::thread m_thread;
};

} // namespace ts

#endif // TS_ORBITS_ORBITCOMPUTER_H
