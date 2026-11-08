// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <QtTest>

#include "concurrency.h"
#include "ts/perf/frametimestats.h"
#include "ts/perf/processstats.h"

/** Unit tests for ts/perf: frame-time statistics and process stats. */
class TestPerf : public QObject
{
  Q_OBJECT

private slots:
  void emptyStats()
  {
    const ts::FrameTimeStats stats(10);
    QCOMPARE(stats.count(), std::size_t(0));
    QCOMPARE(stats.meanS(), 0.0);
    QCOMPARE(stats.maxS(), 0.0);
    QCOMPARE(stats.percentileS(50), 0.0);
    QCOMPARE(stats.meanFps(), 0.0);
  }

  void zeroCapacityBecomesOne()
  {
    ts::FrameTimeStats stats(0);
    QCOMPARE(stats.capacity(), std::size_t(1));
    stats.addFrame(0.5);
    stats.addFrame(0.25);
    QCOMPARE(stats.count(), std::size_t(1));
    QCOMPARE(stats.meanS(), 0.25);
  }

  void meanAndFps()
  {
    ts::FrameTimeStats stats(4);
    stats.addFrame(0.01);
    stats.addFrame(0.03);
    QCOMPARE(stats.count(), std::size_t(2));
    QVERIFY(qFuzzyCompare(stats.meanS(), 0.02));
    QVERIFY(qFuzzyCompare(stats.meanFps(), 50.0));
    QCOMPARE(stats.maxS(), 0.03);
  }

  void rollingWindow()
  {
    ts::FrameTimeStats stats(3);
    for (int i = 1; i <= 5; ++i) {
      stats.addFrame(i * 0.1);
    }
    // Keeps 0.3, 0.4, 0.5.
    QCOMPARE(stats.count(), std::size_t(3));
    QCOMPARE(stats.totalFrames(), std::size_t(5));
    QVERIFY(qFuzzyCompare(stats.meanS(), 0.4));
    QCOMPARE(stats.maxS(), 0.5);
    stats.clear();
    QCOMPARE(stats.count(), std::size_t(0));
    QCOMPARE(stats.totalFrames(), std::size_t(0));
  }

  void negativeCountsAsZero()
  {
    ts::FrameTimeStats stats(2);
    stats.addFrame(-1.0);
    QCOMPARE(stats.maxS(), 0.0);
  }

  void percentiles_data()
  {
    QTest::addColumn<double>("p");
    QTest::addColumn<double>("expected");
    // Samples 1..100 ms; nearest rank.
    QTest::newRow("p0") << 0.0 << 0.001;
    QTest::newRow("p1") << 1.0 << 0.001;
    QTest::newRow("p50") << 50.0 << 0.050;
    QTest::newRow("p95") << 95.0 << 0.095;
    QTest::newRow("p99") << 99.0 << 0.099;
    QTest::newRow("p100") << 100.0 << 0.100;
    QTest::newRow("clamped-high") << 150.0 << 0.100;
    QTest::newRow("clamped-low") << -5.0 << 0.001;
  }

  void percentiles()
  {
    QFETCH(double, p);
    QFETCH(double, expected);
    ts::FrameTimeStats stats(100);
    // Added in reverse to check that order does not matter.
    for (int ms = 100; ms >= 1; --ms) {
      stats.addFrame(ms / 1000.0);
    }
    QVERIFY(qFuzzyCompare(stats.percentileS(p), expected));
  }

  void processStatsValid()
  {
    const ts::ProcessStats stats = ts::currentProcessStats();
    QVERIFY(stats.valid);
    QVERIFY(stats.residentBytes > std::uint64_t{1024} * 1024U);
    QVERIFY(stats.peakResidentBytes >= stats.residentBytes);
    QVERIFY(stats.cpuTimeS >= 0.0);
  }

  void processStatsSeesAllocation()
  {
    const ts::ProcessStats before = ts::currentProcessStats();
    const std::size_t size = std::size_t{64} * 1024U * 1024U;
    std::vector<char> block(size);
    // Touch every page so it becomes resident.
    std::memset(block.data(), 1, block.size());
    const ts::ProcessStats after = ts::currentProcessStats();
    QVERIFY(after.peakResidentBytes >= before.residentBytes + size / 2);
    QCOMPARE(block[size - 1], char(1));
  }

  void processStatsCpuIncreases()
  {
    const double before = ts::currentProcessStats().cpuTimeS;
    QElapsedTimer timer;
    timer.start();
    volatile double sink = 0.0;
    while (timer.elapsed() < 200) {
      for (int i = 0; i < 10000; ++i) {
        sink = sink + i * 0.5;
      }
    }
    QVERIFY(ts::currentProcessStats().cpuTimeS > before);
  }

  void concurrentOwnInstances()
  {
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [](int thread) {
          ts::FrameTimeStats stats(64);
          for (int i = 0; i < 10000; ++i) {
            stats.addFrame((thread + 1) * 0.001);
          }
          const ts::ProcessStats process = ts::currentProcessStats();
          return process.valid &&
                 qFuzzyCompare(stats.percentileS(50), (thread + 1) * 0.001);
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkAddFrame()
  {
    ts::FrameTimeStats stats(240);
    QBENCHMARK
    {
      stats.addFrame(0.016);
    }
  }

  void benchmarkPercentile()
  {
    ts::FrameTimeStats stats(240);
    for (int i = 0; i < 240; ++i) {
      stats.addFrame(i * 0.0001);
    }
    double p = 0.0;
    QBENCHMARK
    {
      p = stats.percentileS(99);
    }
    QVERIFY(p > 0.0);
  }

  void benchmarkProcessStats()
  {
    ts::ProcessStats stats;
    QBENCHMARK
    {
      stats = ts::currentProcessStats();
    }
    QVERIFY(stats.valid);
  }
};

QTEST_GUILESS_MAIN(TestPerf)
#include "tst_perf.moc"
