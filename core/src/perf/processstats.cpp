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

#include "ts/perf/processstats.h"

#include <charconv>
#include <cstring>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace ts
{

namespace
{

#if !defined(_WIN32)
/** Parses "VmRSS:   1234 kB" style lines; returns bytes or 0. */
std::uint64_t parseKilobytes(const std::string& line, const char* key)
{
  const std::size_t keyLength = std::strlen(key);
  if (line.compare(0, keyLength, key) != 0) {
    return 0;
  }
  const char* first = line.c_str() + keyLength;
  const char* last = line.c_str() + line.size();
  while (first < last && (*first == ' ' || *first == '\t')) {
    ++first;
  }
  std::uint64_t kilobytes = 0;
  const std::from_chars_result parsed = std::from_chars(first, last, kilobytes);
  if (parsed.ec != std::errc()) {
    return 0;
  }
  return kilobytes * 1024U;
}
#endif

} // namespace

ProcessStats currentProcessStats()
{
  ProcessStats stats;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters;
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    stats.residentBytes = counters.WorkingSetSize;
    stats.peakResidentBytes = counters.PeakWorkingSetSize;
    stats.valid = true;
  }
  FILETIME creation;
  FILETIME exit;
  FILETIME kernel;
  FILETIME user;
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    const ULONGLONG k = (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) |
                        kernel.dwLowDateTime;
    const ULONGLONG u = (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) |
                        user.dwLowDateTime;
    // FILETIME counts 100 ns intervals.
    stats.cpuTimeS = static_cast<double>(k + u) * 1e-7;
  }
#else
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (const std::uint64_t rss = parseKilobytes(line, "VmRSS:")) {
      stats.residentBytes = rss;
      stats.valid = true;
    } else if (const std::uint64_t hwm = parseKilobytes(line, "VmHWM:")) {
      stats.peakResidentBytes = hwm;
    }
  }
  struct rusage usage {
  };
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    stats.cpuTimeS =
        static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
        static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) *
            1e-6;
    if (!stats.valid) {
      // No /proc (e.g. macOS): ru_maxrss is the peak, in bytes or kB.
#if defined(__APPLE__)
      stats.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
      // glibc declares the rusage fields inside anonymous unions.
      // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
      stats.peakResidentBytes =
          static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
      // NOLINTEND(cppcoreguidelines-pro-type-union-access)
#endif
      stats.residentBytes = stats.peakResidentBytes;
      stats.valid = stats.peakResidentBytes > 0;
    }
  }
#endif
  if (stats.peakResidentBytes < stats.residentBytes) {
    stats.peakResidentBytes = stats.residentBytes;
  }
  return stats;
}

} // namespace ts
