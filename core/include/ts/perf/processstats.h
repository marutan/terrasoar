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
 * Memory and CPU use of the current process.
 */
#ifndef TS_PERF_PROCESSSTATS_H
#define TS_PERF_PROCESSSTATS_H

#include <cstdint>

namespace ts
{

/**
 * A snapshot of the current process's resource use.
 *
 * @reentrant
 */
struct ProcessStats {
  /** Resident memory now, in bytes (Linux VmRSS, Windows working set). */
  std::uint64_t residentBytes = 0;
  /** Peak resident memory so far, in bytes (VmHWM / peak working set). */
  std::uint64_t peakResidentBytes = 0;
  /** CPU time used so far in user and kernel mode, in seconds. */
  double cpuTimeS = 0.0;
  /** True if the values could be read on this platform. */
  bool valid = false;
};

/**
 * Reads the current process's memory and CPU use.
 *
 * @threadsafe
 */
ProcessStats currentProcessStats();

} // namespace ts

#endif // TS_PERF_PROCESSSTATS_H
