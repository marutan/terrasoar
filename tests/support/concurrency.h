// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Helpers for the concurrent (thread-safety) tests.
 */
#ifndef TS_TESTS_CONCURRENCY_H
#define TS_TESTS_CONCURRENCY_H

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace ts::test
{

/** Number of threads the concurrent tests start (more than CPU cores). */
inline constexpr int concurrentThreadCount = 8;

/**
 * Runs @p work on @p threadCount threads at once and waits for all of them.
 * All threads are released together so their work overlaps as much as
 * possible, which gives ThreadSanitizer the best chance to see races.
 *
 * @param threadCount Number of threads.
 * @param work Called with the thread index; returns false on failure.
 * @return The number of threads whose work returned false.
 */
inline int runConcurrently(int threadCount,
                           const std::function<bool(int)>& work)
{
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(threadCount));
  for (int i = 0; i < threadCount; ++i) {
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
