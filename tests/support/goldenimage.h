// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Comparing rendered images with stored golden images.
 */
#ifndef TS_TESTS_GOLDENIMAGE_H
#define TS_TESTS_GOLDENIMAGE_H

#include <algorithm>
#include <cstdlib>

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QtGlobal>

namespace ts::test
{

/** How two images differ. */
struct ImageDifference {
  /** False if the sizes differ (the counts are then meaningless). */
  bool sameSize = false;
  /** Pixels where some channel differs by more than the tolerance. */
  int differingPixels = 0;
  /** Largest difference of any channel, 0–255. */
  int maxChannelDifference = 0;
  /** differingPixels as a fraction of all pixels. */
  double differingFraction = 0.0;
  /** Per-pixel difference, scaled up so small differences show. */
  QImage diffImage;
};

/**
 * Compares @p actual with @p expected channel by channel; a pixel differs
 * if any channel is off by more than @p channelTolerance.
 */
inline ImageDifference compareImages(const QImage& actual,
                                     const QImage& expected,
                                     int channelTolerance)
{
  ImageDifference result;
  if (actual.size() != expected.size()) {
    return result;
  }
  result.sameSize = true;
  const QImage a = actual.convertToFormat(QImage::Format_RGBA8888);
  const QImage b = expected.convertToFormat(QImage::Format_RGBA8888);
  result.diffImage = QImage(a.size(), QImage::Format_RGB32);
  for (int y = 0; y < a.height(); ++y) {
    for (int x = 0; x < a.width(); ++x) {
      const QColor pa = a.pixelColor(x, y);
      const QColor pb = b.pixelColor(x, y);
      const int d = std::max(
          {std::abs(pa.red() - pb.red()), std::abs(pa.green() - pb.green()),
           std::abs(pa.blue() - pb.blue()), std::abs(pa.alpha() - pb.alpha())});
      result.maxChannelDifference = std::max(result.maxChannelDifference, d);
      if (d > channelTolerance) {
        ++result.differingPixels;
      }
      const int shown = std::min(255, d * 8);
      result.diffImage.setPixel(x, y, qRgb(shown, shown, shown));
    }
  }
  result.differingFraction =
      static_cast<double>(result.differingPixels) /
      static_cast<double>(std::max(1, a.width() * a.height()));
  return result;
}

/** How far an image may be from its golden image. */
struct GoldenTolerance {
  /** A pixel differs if any channel is off by more than this (0–255). */
  int channel = 0;
  /** At most this fraction of the pixels may differ. */
  double maxDifferingFraction = 0.0;
};

/** Returns tests/data/golden/@p name.png. */
inline QString goldenPath(const QString& name)
{
  return QDir(QStringLiteral(TS_TEST_DATA_DIR))
      .filePath(QStringLiteral("golden/") + name + QStringLiteral(".png"));
}

/**
 * Compares @p actual with the golden image @p name. Returns an empty
 * string if it matches within @p tolerance; otherwise returns what
 * differs.
 *
 * - TS_UPDATE_GOLDEN=1 writes @p actual as the new golden image instead.
 * - TS_GOLDEN_OUTPUT=<dir> saves <name>-actual.png and <name>-diff.png
 *   there for every comparison that fails (CI uploads that directory).
 */
inline QString checkGolden(const QImage& actual, const QString& name,
                           GoldenTolerance tolerance)
{
  const QString path = goldenPath(name);
  if (qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") != 0) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return actual.save(path) ? QString()
                             : QStringLiteral("could not write ") + path;
  }
  const QImage expected(path);
  QString failure;
  ImageDifference difference;
  if (expected.isNull()) {
    failure = QStringLiteral("no golden image %1 (run with TS_UPDATE_GOLDEN=1 "
                             "to create it)")
                  .arg(path);
  } else {
    difference = compareImages(actual, expected, tolerance.channel);
    if (!difference.sameSize) {
      failure = QStringLiteral("size %1x%2, golden %3x%4")
                    .arg(actual.width())
                    .arg(actual.height())
                    .arg(expected.width())
                    .arg(expected.height());
    } else if (difference.differingFraction > tolerance.maxDifferingFraction) {
      failure = QStringLiteral("%1 pixels (%2 %) differ by more than %3; "
                               "largest difference %4")
                    .arg(difference.differingPixels)
                    .arg(100.0 * difference.differingFraction, 0, 'f', 2)
                    .arg(tolerance.channel)
                    .arg(difference.maxChannelDifference);
    }
  }
  const QString outputDir = qEnvironmentVariable("TS_GOLDEN_OUTPUT");
  if (!failure.isEmpty() && !outputDir.isEmpty()) {
    QDir().mkpath(outputDir);
    const QDir dir(outputDir);
    actual.save(dir.filePath(name + QStringLiteral("-actual.png")));
    if (!difference.diffImage.isNull()) {
      difference.diffImage.save(
          dir.filePath(name + QStringLiteral("-diff.png")));
    }
  }
  return failure;
}

} // namespace ts::test

#endif // TS_TESTS_GOLDENIMAGE_H
