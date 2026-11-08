// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <memory>

#include <QColor>
#include <QImage>
#include <QtTest>

#include "concurrency.h"
#include "ts/providers/testproviders.h"

namespace
{

const std::atomic<bool> notCancelled{false};

/** Number of pixels of @p image that are exactly @p color. */
int countColor(const QImage& image, QRgb color)
{
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      count += image.pixel(x, y) == color ? 1 : 0;
    }
  }
  return count;
}

} // namespace

/** Unit tests for ts/providers: the generated test imagery. */
class TestProviders : public QObject
{
  Q_OBJECT

private slots:
  void solidColor()
  {
    ts::SolidColorImageryProvider provider(QColor(10, 20, 30, 128), 32, 9);
    QCOMPARE(provider.tileSizePx(), 32);
    QCOMPARE(provider.maximumLevel(), 9);
    const QImage image = provider.loadImage(ts::TileKey{3, 1, 2}, notCancelled);
    QCOMPARE(image.size(), QSize(32, 32));
    QCOMPARE(QColor::fromRgba(image.pixel(5, 7)), QColor(10, 20, 30, 128));
    // Beyond the deepest level, invalid keys and cancelled loads: nothing.
    QVERIFY(provider.loadImage(ts::TileKey{10, 0, 0}, notCancelled).isNull());
    QVERIFY(provider.loadImage(ts::TileKey{1, 9, 0}, notCancelled).isNull());
    const std::atomic<bool> cancelled{true};
    QVERIFY(provider.loadImage(ts::TileKey{1, 0, 0}, cancelled).isNull());
    QVERIFY(provider.id() !=
            ts::SolidColorImageryProvider(QColor(10, 20, 30)).id());
    QVERIFY(provider.name().contains(QStringLiteral("#800a141e")));
  }

  void checkerboard()
  {
    ts::CheckerboardImageryProvider provider(128);
    const ts::TileKey key{5, 37, 11};
    const QImage image = provider.loadImage(key, notCancelled);
    QCOMPARE(image.size(), QSize(128, 128));
    // Deterministic: the same key gives the same pixels.
    QCOMPARE(provider.loadImage(key, notCancelled), image);
    // Outlined, with a white-on-black label in the middle.
    QCOMPARE(image.pixel(0, 64), qRgba(40, 40, 40, 255));
    QCOMPARE(image.pixel(127, 5), qRgba(40, 40, 40, 255));
    QVERIFY(countColor(image, qRgba(255, 255, 255, 255)) > 20);
    QCOMPARE(image.pixel(64, 64 - 2 * 5 / 2 - 2), qRgba(0, 0, 0, 255));
    // Neighbouring squares differ; another level has other colours.
    QVERIFY(image.pixel(4, 4) != image.pixel(20, 4));
    const QImage deeper = provider.loadImage(key.child(0), notCancelled);
    QVERIFY(deeper.pixel(4, 4) != image.pixel(4, 4));
    QCOMPARE(provider.id(), ts::CheckerboardImageryProvider(128).id());
    QVERIFY(provider.id() != ts::CheckerboardImageryProvider(256).id());
  }

  void concurrentSharedProvider()
  {
    // Thread-safe: one provider loading on many threads gives the same
    // tiles as on one.
    ts::CheckerboardImageryProvider provider(64);
    const QImage expected =
        provider.loadImage(ts::TileKey{7, 100, 50}, notCancelled);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&provider, &expected](int thread) {
          for (int i = 0; i < 50; ++i) {
            const QImage other = provider.loadImage(
                ts::TileKey{7, 100 + thread, 50 + i}, notCancelled);
            if (other.isNull()) {
              return false;
            }
          }
          return provider.loadImage(ts::TileKey{7, 100, 50}, notCancelled) ==
                 expected;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkCheckerboard()
  {
    ts::CheckerboardImageryProvider provider;
    QBENCHMARK
    {
      provider.loadImage(ts::TileKey{12, 2100, 700}, notCancelled);
    }
  }
};

QTEST_GUILESS_MAIN(TestProviders)
#include "tst_providers.moc"
