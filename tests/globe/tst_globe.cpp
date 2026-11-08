// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <optional>
#include <vector>

#include <QImage>
#include <QtTest>

#include "goldenimage.h"
#include "offscreengl.h"
#include "ts/camera/camera.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodetic.h"
#include "ts/globe/globerenderer.h"

namespace
{

constexpr int imageWidthPx = 320;
constexpr int imageHeightPx = 240;

/**
 * Golden images are compared only on Mesa llvmpipe, whose output is the
 * same everywhere for a given version; other drivers differ in rounding.
 * Channel tolerance and differing-pixel fraction allow for small changes
 * between llvmpipe versions (Linux packages, the Windows Mesa build).
 */
constexpr ts::test::GoldenTolerance goldenTolerance{12, 0.005};

ts::Camera makeCamera(const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(imageWidthPx, imageHeightPx);
  camera.setPose(pose);
  return camera;
}

/** Centroid of the pixels where red clearly exceeds blue (the markers). */
std::optional<QPointF> redCentroid(const QImage& image)
{
  double sum = 0.0;
  double sumX = 0.0;
  double sumY = 0.0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      const double weight = std::max(0, c.red() - c.blue());
      sum += weight;
      sumX += weight * (x + 0.5);
      sumY += weight * (y + 0.5);
    }
  }
  if (sum <= 0.0) {
    return std::nullopt;
  }
  return QPointF(sumX / sum, sumY / sum);
}

int to8Bit(float value)
{
  return qRound(value * 255.0F);
}

/** True if (x, y) shows the globe rather than the background colour. */
bool isGlobe(const QImage& image, int x, int y)
{
  const QColor c = image.pixelColor(x, y);
  const ts::GlobeStyle style;
  return std::abs(c.red() - to8Bit(style.backgroundRgb[0])) +
             std::abs(c.green() - to8Bit(style.backgroundRgb[1])) +
             std::abs(c.blue() - to8Bit(style.backgroundRgb[2])) >
         6;
}

} // namespace

/** GPU tests for ts/globe: golden images, precision near the surface. */
class TestGlobe : public QObject
{
  Q_OBJECT

private:
  /** Renders @p camera into an image (needs a valid m_gl). */
  QImage render(const ts::Camera& camera,
                const std::vector<ts::GlobeMarker>& markers = {})
  {
    m_gl->makeCurrent();
    m_renderer->setMarkers(markers);
    ts::PassDesc pass;
    pass.target = m_target.get();
    pass.viewportPx = QRect(0, 0, imageWidthPx, imageHeightPx);
    m_renderer->render(camera, pass, 1.0);
    return m_gl->device().readPixels(*m_target);
  }

  std::unique_ptr<ts::test::OffscreenGl> m_gl;
  std::unique_ptr<ts::GlobeRenderer> m_renderer;
  std::unique_ptr<ts::RenderTarget> m_target;
  bool m_softwareRenderer = false;

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void initTestCase()
  {
    m_gl = std::make_unique<ts::test::OffscreenGl>();
    if (!m_gl->isValid()) {
      return;
    }
    m_renderer = std::make_unique<ts::GlobeRenderer>(m_gl->device());
    QString error;
    QVERIFY2(m_renderer->initialise(&error), qPrintable(error));
    m_target = m_gl->device().createRenderTarget(imageWidthPx, imageHeightPx);
    QVERIFY(m_target);
    const ts::RenderDeviceInfo info = m_gl->device().info();
    m_softwareRenderer = info.renderer.contains(QLatin1String("llvmpipe"));
    qInfo("renderer: %s", qPrintable(info.renderer));
  }

  void cleanupTestCase()
  {
    if (m_gl && m_gl->isValid()) {
      m_gl->makeCurrent();
    }
    m_target.reset();
    m_renderer.reset();
    m_gl.reset();
  }

  void goldenImages_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("headingDeg");
    QTest::addColumn<double>("tiltDeg");
    QTest::addColumn<bool>("withMarkers");
    QTest::newRow("whole-globe") << 20.0 << 0.0 << 1.6e7 << 0.0 << 0.0 << false;
    QTest::newRow("europe-tilted")
        << 48.0 << 10.0 << 3.0e6 << 30.0 << 50.0 << false;
    QTest::newRow("north-pole") << 89.9 << 0.0 << 1.5e7 << 0.0 << 0.0 << false;
    QTest::newRow("antimeridian-markers")
        << -15.0 << 180.0 << 8.0e6 << 0.0 << 20.0 << true;
    QTest::newRow("low-horizon")
        << 45.0 << 7.0 << 3000.0 << 0.0 << 80.0 << false;
  }

  void goldenImages()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, headingDeg);
    QFETCH(double, tiltDeg);
    QFETCH(bool, withMarkers);
    std::vector<ts::GlobeMarker> markers;
    if (withMarkers) {
      for (const double lon : {170.0, 180.0, -170.0}) {
        ts::GlobeMarker marker;
        marker.positionEcef = ts::geodeticToEcef(ts::Geodetic{-15.0, lon, 0.0});
        marker.rgba = {230, 40, 40, 255};
        marker.radiusPx = 5.0;
        markers.push_back(marker);
      }
    }
    const QImage image = render(
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, headingDeg, tiltDeg}),
        markers);
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void silhouetteMatchesPicking()
  {
    // Every pixel the CPU picking hits shows the globe and every pixel it
    // misses shows the background, except along the antialiased edge.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{10.0, 20.0, 1.6e7, 0.0, 0.0});
    const QImage image = render(camera);
    int mismatches = 0;
    for (int y = 0; y < imageHeightPx; ++y) {
      for (int x = 0; x < imageWidthPx; ++x) {
        // A pixel is decided if its centre and all four neighbours'
        // centres agree.
        const bool hit = camera.pickGeodetic(x + 0.5, y + 0.5).has_value();
        bool decided = true;
        for (const QPointF& offset :
             {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) {
          decided = decided && camera.pickGeodetic(x + 0.5 + offset.x(),
                                                   y + 0.5 + offset.y())
                                       .has_value() == hit;
        }
        if (decided && isGlobe(image, x, y) != hit) {
          ++mismatches;
        }
      }
    }
    QCOMPARE(mismatches, 0);
  }

  void horizonOneMetreAboveSurface()
  {
    // Looking towards the horizon from 1 m up. The horizon row from the
    // GPU must match CPU picking (double precision) to within a pixel; the
    // ray-cast's constant term is 3e-7 here and would vanish in float.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setPose(ts::CameraPose{51.4769, -0.0005, 1.0, 0.0, 85.0});
    QVERIFY(std::abs(camera.eyeGeodetic().heightM - 1.0) < 0.01);
    const QImage image = render(camera);
    const int x = imageWidthPx / 2;
    int cpuRow = -1;
    int gpuRow = -1;
    for (int y = 0; y < imageHeightPx; ++y) {
      if (cpuRow < 0 && camera.pickGeodetic(x + 0.5, y + 0.5)) {
        cpuRow = y;
      }
      if (gpuRow < 0 && isGlobe(image, x, y)) {
        gpuRow = y;
      }
    }
    qInfo("horizon row: CPU %d, GPU %d", cpuRow, gpuRow);
    QVERIFY(cpuRow > 0 && cpuRow < imageHeightPx - 1);
    QVERIFY(std::abs(cpuRow - gpuRow) <= 1);
  }

  void noJitterOneMetreAboveSurface()
  {
    // The camera is 1 m above the ground and moves east in 1 mm steps; a
    // marker 2 m away must land where the double-precision projection puts
    // it, to well under a pixel, in every frame. In plain float, ECEF
    // positions round to 0.5 m and the marker would jump by tens of pixels.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::CameraPose start{51.4769, -0.0005, 2.0, 0.0, 60.0};
    ts::Camera camera = makeCamera(start);
    QVERIFY(std::abs(camera.eyeGeodetic().heightM - 1.0) < 0.01);
    ts::GlobeMarker marker;
    marker.positionEcef = camera.targetEcef();
    marker.rgba = {255, 0, 0, 255};
    marker.radiusPx = 6.0;
    const std::vector<ts::GlobeMarker> markers = {marker};

    const ts::LocalFrame frame =
        ts::localFrame(start.targetLatDeg, start.targetLonDeg);
    double worstPx = 0.0;
    for (int step = 0; step <= 40; ++step) {
      // Move the camera, not the marker: shift the target east.
      const ts::Geodetic moved = ts::ecefToGeodetic(
          marker.positionEcef + frame.eastEcef * (0.001 * step));
      ts::CameraPose pose = start;
      pose.targetLatDeg = moved.latDeg;
      pose.targetLonDeg = moved.lonDeg;
      camera = makeCamera(pose);
      const std::optional<ts::ScreenPoint> expected =
          camera.project(marker.positionEcef);
      if (!expected) {
        QFAIL("expected: no value");
      }
      const std::optional<QPointF> drawn = redCentroid(render(camera, markers));
      if (!drawn) {
        QFAIL("drawn: no value");
      }
      const double errorPx =
          std::hypot(drawn->x() - expected->xPx, drawn->y() - expected->yPx);
      worstPx = std::max(worstPx, errorPx);
    }
    qInfo("worst marker position error: %.3f px", worstPx);
    QVERIFY(worstPx < 0.25);
  }

  void highDpiScalesMarkers()
  {
    // The same camera at device pixel ratio 2 covers twice the pixels in
    // each direction; markers keep their logical size.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 2.0e7, 0.0, 0.0});
    ts::GlobeMarker marker;
    marker.positionEcef = camera.targetEcef();
    marker.rgba = {255, 0, 0, 255};
    marker.radiusPx = 5.0;
    m_gl->makeCurrent();
    std::unique_ptr<ts::RenderTarget> large =
        m_gl->device().createRenderTarget(2 * imageWidthPx, 2 * imageHeightPx);
    QVERIFY(large);
    m_renderer->setMarkers({marker});
    ts::PassDesc pass;
    pass.target = large.get();
    pass.viewportPx = QRect(0, 0, 2 * imageWidthPx, 2 * imageHeightPx);
    m_renderer->render(camera, pass, 2.0);
    const QImage image = m_gl->device().readPixels(*large);
    int redPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        const QColor c = image.pixelColor(x, y);
        redPixels += c.red() > 200 && c.blue() < 100 ? 1 : 0;
      }
    }
    // A disc of radius 2 × 5 − 1 (inside the rim) device pixels.
    const double expected = 3.14159265358979 * 9.0 * 9.0;
    qInfo("red pixels at 2x: %d (disc %.0f)", redPixels, expected);
    QVERIFY(std::abs(redPixels - expected) < 0.15 * expected);
    const std::optional<QPointF> centre = redCentroid(image);
    if (!centre) {
      QFAIL("centre: no value");
    }
    QVERIFY(std::abs(centre->x() - imageWidthPx) < 0.5);
    QVERIFY(std::abs(centre->y() - imageHeightPx) < 0.5);
  }

  void benchmarkFrame()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{20.0, 0.0, 2.2e7, 0.0, 0.0});
    QBENCHMARK
    {
      render(camera);
    }
  }
};

QTEST_MAIN(TestGlobe)
#include "tst_globe.moc"
