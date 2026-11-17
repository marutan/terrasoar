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
 * Helpers for the concurrent (thread-safety) tests.
 */
#ifndef TS_TESTS_CONCURRENCY_H
#define TS_TESTS_CONCURRENCY_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace ts::test
{

/** Number of threads the concurrent tests start (more than CPU cores). */
inline constexpr int32_t concurrentThreadCount = 8;

/**
 * Runs @p work on @p threadCount threads at once and waits for all of them.
 * All threads are released together so their work overlaps as much as
 * possible, which gives ThreadSanitizer the best chance to see races.
 *
 * @param threadCount Number of threads.
 * @param work Called with the thread index; returns false on failure.
 * @return The number of threads whose work returned false.
 */
inline int32_t runConcurrently(int32_t threadCount,
                               const std::function<bool(int32_t)>& work)
{
  std::atomic<int32_t> ready{0};
  std::atomic<bool> go{false};
  std::atomic<int32_t> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(threadCount));
  for (int32_t i = 0; i < threadCount; ++i) {
    threads.emplace_back([&, i]() {
      ready.fetch_add(1);
      while (!go.load()) {
        std::this_thread::yield();
      }
      if (!work(i)) {
        failures.fetch_add(1);
      }
    });
  }
  while (ready.load() < threadCount) {
    std::this_thread::yield();
  }
  go.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }
  return failures.load();
}

} // namespace ts::test

#endif // TS_TESTS_CONCURRENCY_H
