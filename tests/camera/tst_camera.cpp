// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>

#include <QtTest>

#include "concurrency.h"
#include "ts/camera/camera.h"
#include "ts/camera/cameracontroller.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"
#include "ts/maths/vec4.h"

namespace
{

constexpr double widthPx = 800.0;
constexpr double heightPx = 600.0;

/** Frame rates the frame-rate-independence tests replay at. */
constexpr std::array<double, 4> frameRatesHz = {10.0, 20.0, 60.0, 144.0};

ts::Camera makeCamera(const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(widthPx, heightPx);
  camera.setPose(pose);
  return camera;
}

ts::CameraController makeController(const ts::CameraPose& pose)
{
  ts::CameraController controller;
  controller.setViewportSize(widthPx, heightPx);
  controller.setPose(pose);
  return controller;
}

/** Distance in metres between the targets of two poses. */
double targetDistanceM(const ts::CameraPose& a, const ts::CameraPose& b)
{
  return ts::geodesicInverse(a.targetLatDeg, a.targetLonDeg, b.targetLatDeg,
                             b.targetLonDeg)
      .distanceM;
}

/** Runs @p controller for @p durationS at @p frameRateHz frames per second. */
void runFor(ts::CameraController& controller, double durationS,
            double frameRateHz)
{
  const int frames = static_cast<int>(std::lround(durationS * frameRateHz));
  for (int i = 0; i < frames; ++i) {
    controller.advance(1.0 / frameRateHz);
  }
}

/** Point of the globe under (x, y), ECEF, or nothing. */
std::optional<ts::Vec3> hitAt(const ts::Camera& camera, double x, double y)
{
  return ts::intersectEllipsoidPoint(camera.rayThroughPixel(x, y));
}

/** Poses used by several tests: straight down, tilted, turned, polar. */
const std::array<ts::CameraPose, 4> samplePoses = {
    ts::CameraPose{10.0, 20.0, 2.0e6, 0.0, 0.0},
    ts::CameraPose{48.0, 10.0, 5.0e5, 30.0, 50.0},
    ts::CameraPose{-33.9, 151.2, 2.0e4, 250.0, 70.0},
    ts::CameraPose{80.0, -170.0, 3.0e6, 90.0, 20.0}};

} // namespace

/** Unit tests for ts/camera: the camera and its controller. */
class TestCamera : public QObject
{
  Q_OBJECT

private slots:
  // --- Camera ----------------------------------------------------------------

  void poseIsClamped()
  {
    ts::Camera camera;
    camera.setPose(ts::CameraPose{95.0, 190.0, 1.0e9, -30.0, 120.0});
    const ts::CameraPose& pose = camera.pose();
    QVERIFY(pose.targetLatDeg < 90.0 && pose.targetLatDeg > 89.99);
    QCOMPARE(pose.targetLonDeg, -170.0);
    QCOMPARE(pose.rangeM, ts::Camera::maximumRangeM);
    QCOMPARE(pose.headingDeg, 330.0);
    QCOMPARE(pose.tiltDeg, ts::Camera::maximumTiltDeg);
  }

  void minimumEyeHeight_data()
  {
    QTest::addColumn<double>("tiltDeg");
    QTest::newRow("down") << 0.0;
    QTest::newRow("tilted") << 60.0;
    QTest::newRow("grazing") << 85.0;
  }

  void minimumEyeHeight()
  {
    // A range of zero (or negative) is lengthened to keep the eye 1 m up.
    QFETCH(double, tiltDeg);
    ts::Camera camera;
    camera.setPose(ts::CameraPose{45.0, 7.0, 0.0, 10.0, tiltDeg});
    QVERIFY(std::abs(camera.eyeGeodetic().heightM -
                     ts::Camera::minimumEyeHeightM) < 1e-6);
    camera.setPose(ts::CameraPose{45.0, 7.0, -5.0, 10.0, tiltDeg});
    QVERIFY(camera.eyeGeodetic().heightM >=
            ts::Camera::minimumEyeHeightM - 1e-6);
  }

  void straightDownEyePosition()
  {
    const ts::Camera camera =
        makeCamera(ts::CameraPose{30.0, 60.0, 1.0e5, 0.0, 0.0});
    const ts::Geodetic eye = camera.eyeGeodetic();
    QVERIFY(std::abs(eye.latDeg - 30.0) < 1e-9);
    QVERIFY(std::abs(eye.lonDeg - 60.0) < 1e-9);
    QVERIFY(std::abs(eye.heightM - 1.0e5) < 1e-6);
  }

  void frameIsOrthonormal()
  {
    for (const ts::CameraPose& pose : samplePoses) {
      const ts::Camera camera = makeCamera(pose);
      const ts::Vec3& f = camera.forwardEcef();
      const ts::Vec3& u = camera.upEcef();
      const ts::Vec3& r = camera.rightEcef();
      QVERIFY(std::abs(ts::length(f) - 1.0) < 1e-12);
      QVERIFY(std::abs(ts::length(u) - 1.0) < 1e-12);
      QVERIFY(std::abs(ts::length(r) - 1.0) < 1e-12);
      QVERIFY(std::abs(ts::dot(f, u)) < 1e-12);
      QVERIFY(std::abs(ts::dot(f, r)) < 1e-12);
      QVERIFY(ts::distance(ts::cross(r, u), -f) < 1e-12);
      // The target is straight ahead at the range.
      QVERIFY(ts::distance(camera.eyeEcef() + f * pose.rangeM,
                           camera.targetEcef()) < 1e-6);
    }
  }

  void northUpEastRight()
  {
    // Looking straight down with heading 0, north is up and east right;
    // with heading 90, east is up.
    const ts::Camera north =
        makeCamera(ts::CameraPose{10.0, 20.0, 1.0e6, 0.0, 0.0});
    const ts::LocalFrame frame = ts::localFrame(10.0, 20.0);
    QVERIFY(ts::distance(north.upEcef(), frame.northEcef) < 1e-12);
    QVERIFY(ts::distance(north.rightEcef(), frame.eastEcef) < 1e-12);
    const std::optional<ts::Geodetic> above =
        north.pickGeodetic(widthPx / 2.0, 10.0);
    QVERIFY(above && above->latDeg > 10.0 &&
            std::abs(above->lonDeg - 20.0) < 1e-6);

    const ts::Camera east =
        makeCamera(ts::CameraPose{10.0, 20.0, 1.0e6, 90.0, 0.0});
    QVERIFY(ts::distance(east.upEcef(), frame.eastEcef) < 1e-12);
    const std::optional<ts::Geodetic> ahead =
        east.pickGeodetic(widthPx / 2.0, 10.0);
    // About 400 km east; the great circle there has bent about 0.02°
    // towards the equator.
    QVERIFY(ahead && ahead->lonDeg > 23.0 &&
            std::abs(ahead->latDeg - 10.0) < 0.05);
  }

  void centrePixelHitsTarget()
  {
    for (const ts::CameraPose& pose : samplePoses) {
      const ts::Camera camera = makeCamera(pose);
      const std::optional<ts::Geodetic> hit =
          camera.pickGeodetic(widthPx / 2.0, heightPx / 2.0);
      if (!hit) {
        QFAIL("hit: no value");
      }
      QVERIFY(std::abs(hit->latDeg - camera.pose().targetLatDeg) < 1e-9);
      QVERIFY(std::abs(ts::normaliseLonDeg(hit->lonDeg -
                                           camera.pose().targetLonDeg)) < 1e-9);
    }
  }

  void edgeRaysHaveTheFieldOfView()
  {
    // The ray through the middle of the top edge is half the vertical
    // field of view from the view direction.
    const ts::Camera camera = makeCamera(samplePoses[1]);
    const ts::Ray top = camera.rayThroughPixel(widthPx / 2.0, 0.0);
    const double angleDeg = ts::radToDeg(std::acos(
        std::clamp(ts::dot(top.direction, camera.forwardEcef()), -1.0, 1.0)));
    QVERIFY(std::abs(angleDeg - camera.verticalFovDeg() / 2.0) < 1e-9);
    QVERIFY(ts::dot(top.direction, camera.upEcef()) > 0.0);
  }

  void pixelRayProjectRoundTrip()
  {
    // Ray through a pixel → point on the globe → projected back to the
    // same pixel.
    for (const ts::CameraPose& pose : samplePoses) {
      const ts::Camera camera = makeCamera(pose);
      for (int row = 0; row < 17; ++row) {
        const double y = 0.5 + 37.0 * row;
        for (int column = 0; column < 20; ++column) {
          const double x = 0.5 + 41.0 * column;
          const std::optional<ts::Vec3> hit = hitAt(camera, x, y);
          if (!hit) {
            continue;
          }
          const std::optional<ts::ScreenPoint> back = camera.project(*hit);
          if (!back) {
            QFAIL("back: no value");
          }
          QVERIFY(std::abs(back->xPx - x) < 1e-6);
          QVERIFY(std::abs(back->yPx - y) < 1e-6);
          QVERIFY(back->depthM > camera.nearM());
          QVERIFY(back->depthM < camera.farM());
        }
      }
    }
  }

  void matricesMatchProject()
  {
    // projection × viewRotation of the eye-relative offset gives the same
    // pixel as project().
    const ts::Camera camera = makeCamera(samplePoses[2]);
    const ts::Vec3 point =
        ts::geodeticToEcef(ts::Geodetic{-33.85, 151.25, 120.0});
    const std::optional<ts::ScreenPoint> expected = camera.project(point);
    if (!expected) {
      QFAIL("expected: no value");
    }
    const ts::Vec4 clip =
        camera.viewProjection() * ts::Vec4::point(point - camera.eyeEcef());
    const double x = (clip.x / clip.w + 1.0) * 0.5 * widthPx;
    const double y = (1.0 - clip.y / clip.w) * 0.5 * heightPx;
    QVERIFY(std::abs(x - expected->xPx) < 1e-6);
    QVERIFY(std::abs(y - expected->yPx) < 1e-6);
    QVERIFY(std::abs(clip.w - expected->depthM) < 1e-6);
    QVERIFY(clip.z / clip.w > -1.0 && clip.z / clip.w < 1.0);
  }

  void pointsBehindAreNotProjected()
  {
    const ts::Camera camera = makeCamera(samplePoses[0]);
    QVERIFY(!camera.project(camera.eyeEcef() - camera.forwardEcef() * 10.0));
  }

  void farPlaneBeyondHorizon()
  {
    // From high up, the horizon point at the top of the view is inside the
    // far plane.
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0});
    double farthest = 0.0;
    for (int row = 0; row < static_cast<int>(heightPx); ++row) {
      const std::optional<ts::Vec3> hit =
          hitAt(camera, widthPx / 2.0, row + 0.5);
      const std::optional<ts::ScreenPoint> point =
          hit ? camera.project(*hit) : std::nullopt;
      if (point) {
        farthest = std::max(farthest, point->depthM);
      }
    }
    QVERIFY(farthest > 0.0);
    QVERIFY(farthest < camera.farM());
  }

  // --- Controller: mouse -----------------------------------------------------

  void panKeepsPointUnderCursor()
  {
    for (const ts::CameraPose& pose : samplePoses) {
      ts::CameraController controller = makeController(pose);
      const QPointF from(widthPx * 0.4, heightPx * 0.55);
      const std::optional<ts::Vec3> grabbed =
          hitAt(controller.camera(), from.x(), from.y());
      if (!grabbed) {
        QFAIL("grabbed: no value");
      }
      QVERIFY(controller.beginPan(from, 0.0));
      double worstM = 0.0;
      for (int i = 1; i <= 10; ++i) {
        const QPointF to = from + QPointF(13.0 * i, -7.0 * i);
        controller.dragTo(to, 0.01 * i);
        const std::optional<ts::Vec3> under =
            hitAt(controller.camera(), to.x(), to.y());
        if (!under) {
          QFAIL("under: no value");
        }
        worstM = std::max(worstM, ts::distance(*under, *grabbed));
      }
      controller.endDrag(0.5);
      QVERIFY2(worstM < 1e-3, qPrintable(QString::number(worstM)));
    }
  }

  void panOffTheGlobeDoesNothing()
  {
    ts::CameraController controller =
        makeController(ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0});
    QVERIFY(!controller.beginPan(QPointF(2.0, 2.0), 0.0));
    QVERIFY(!controller.isDragging());
  }

  void zoomKeepsPointUnderCursor()
  {
    for (const ts::CameraPose& pose : samplePoses) {
      ts::CameraController controller = makeController(pose);
      const QPointF at(widthPx * 0.3, heightPx * 0.6);
      const std::optional<ts::Vec3> before =
          hitAt(controller.camera(), at.x(), at.y());
      if (!before) {
        QFAIL("before: no value");
      }
      const double rangeBefore = controller.camera().pose().rangeM;
      controller.zoomAt(at, 2.0);
      QVERIFY(controller.camera().pose().rangeM < rangeBefore);
      const std::optional<ts::Vec3> after =
          hitAt(controller.camera(), at.x(), at.y());
      if (!after) {
        QFAIL("after: no value");
      }
      QVERIFY(ts::distance(*after, *before) < 1e-3);
    }
  }

  void zoomStopsOneMetreUp()
  {
    ts::CameraController controller = makeController(samplePoses[1]);
    for (int i = 0; i < 200; ++i) {
      controller.zoomAt(QPointF(widthPx / 2.0, heightPx / 2.0), 1.0);
    }
    const double heightM = controller.camera().eyeGeodetic().heightM;
    QVERIFY(heightM >= ts::Camera::minimumEyeHeightM - 1e-6);
    QVERIFY(heightM < ts::Camera::minimumEyeHeightM + 1e-3);
    for (int i = 0; i < 200; ++i) {
      controller.zoomAt(QPointF(widthPx / 2.0, heightPx / 2.0), -1.0);
    }
    QCOMPARE(controller.camera().pose().rangeM, ts::Camera::maximumRangeM);
  }

  void rotateDragTurnsAndTilts()
  {
    ts::CameraController controller = makeController(samplePoses[0]);
    const ts::CameraPose before = controller.camera().pose();
    controller.beginRotate(QPointF(400.0, 300.0));
    controller.dragTo(QPointF(600.0, 200.0), 0.1);
    controller.endDrag(0.2);
    const ts::CameraPose& after = controller.camera().pose();
    // 200 px of 800 is 45 degrees; 100 px up of 600 is 15 degrees of tilt.
    QVERIFY(std::abs(after.headingDeg - before.headingDeg - 45.0) < 1e-9);
    QVERIFY(std::abs(after.tiltDeg - before.tiltDeg - 15.0) < 1e-9);
    QCOMPARE(after.targetLatDeg, before.targetLatDeg);
    QCOMPARE(after.targetLonDeg, before.targetLonDeg);
    QVERIFY(!controller.isAnimating());
  }

  // --- Controller: time-based motion ----------------------------------------

  void keysAreFrameRateIndependent_data()
  {
    QTest::addColumn<unsigned int>("motion");
    QTest::addColumn<double>("toleranceM");
    QTest::newRow("forward")
        << static_cast<unsigned int>(ts::MoveForward) << 1e-6;
    QTest::newRow("diagonal")
        << static_cast<unsigned int>(ts::MoveForward | ts::MoveRight) << 1e-6;
    QTest::newRow("diagonal while zooming in")
        << static_cast<unsigned int>(ts::MoveBackward | ts::MoveLeft |
                                     ts::ZoomIn)
        << 1e-6;
    QTest::newRow("zoom out, turn and tilt")
        << static_cast<unsigned int>(ts::ZoomOut | ts::TurnLeft | ts::TiltUp)
        << 1e-6;
  }

  void keysAreFrameRateIndependent()
  {
    // The same 1.5 s of held keys at 10, 20, 60 and 144 fps ends at the
    // same pose.
    QFETCH(unsigned int, motion);
    QFETCH(double, toleranceM);
    const ts::CameraPose start{35.0, 139.0, 4.0e5, 40.0, 20.0};
    std::optional<ts::CameraPose> reference;
    for (const double rate : frameRatesHz) {
      ts::CameraController controller = makeController(start);
      controller.setMotion(motion);
      QVERIFY(controller.isAnimating());
      runFor(controller, 1.5, rate);
      const ts::CameraPose& end = controller.camera().pose();
      if (!reference) {
        reference = end;
        QVERIFY(targetDistanceM(start, end) > 0.0 ||
                end.rangeM != start.rangeM);
        continue;
      }
      QVERIFY2(targetDistanceM(*reference, end) < toleranceM,
               qPrintable(QStringLiteral("%1 fps: target %2 m off")
                              .arg(rate)
                              .arg(targetDistanceM(*reference, end))));
      QVERIFY(std::abs(end.rangeM - reference->rangeM) <
              1e-9 * reference->rangeM);
      QVERIFY(std::abs(end.headingDeg - reference->headingDeg) < 1e-9);
      QVERIFY(std::abs(end.tiltDeg - reference->tiltDeg) < 1e-9);
    }
  }

  void keyRatesPerSecond()
  {
    // One second of zoom-in halves the range; one second of turning turns
    // 60 degrees; forward moves half a range along the heading.
    const ts::CameraPose start{0.0, 0.0, 1.0e5, 0.0, 0.0};
    ts::CameraController zoom = makeController(start);
    zoom.setMotion(ts::ZoomIn);
    runFor(zoom, 1.0, 60.0);
    QVERIFY(std::abs(zoom.camera().pose().rangeM - 5.0e4) < 1e-6);

    ts::CameraController turn = makeController(start);
    turn.setMotion(ts::TurnRight);
    runFor(turn, 1.0, 60.0);
    QVERIFY(std::abs(turn.camera().pose().headingDeg - 60.0) < 1e-9);

    ts::CameraController move = makeController(start);
    move.setMotion(ts::MoveForward);
    runFor(move, 1.0, 60.0);
    QVERIFY(std::abs(targetDistanceM(start, move.camera().pose()) - 5.0e4) <
            1e-6);
    QVERIFY(move.camera().pose().targetLatDeg > 0.0);
    move.setMotion(ts::NoMotion);
    QVERIFY(!move.isAnimating());
  }

  void inertiaIsFrameRateIndependent()
  {
    // A pan released while moving glides to a stop; the glide covers the
    // same ground at every frame rate, and the drag itself was sampled at
    // that frame rate too.
    const ts::CameraPose start{20.0, 30.0, 2.0e6, 0.0, 30.0};
    std::optional<ts::CameraPose> reference;
    for (const double rate : frameRatesHz) {
      ts::CameraController controller = makeController(start);
      const QPointF from(300.0, 300.0);
      QVERIFY(controller.beginPan(from, 0.0));
      const int frames = static_cast<int>(std::lround(0.3 * rate));
      for (int i = 1; i <= frames; ++i) {
        const double t = i / rate;
        controller.dragTo(from + QPointF(400.0 * t, 150.0 * t), t);
      }
      const ts::CameraPose released = controller.camera().pose();
      controller.endDrag(frames / rate);
      QVERIFY(controller.isAnimating());
      runFor(controller, 4.0, rate);
      QVERIFY(!controller.isAnimating());
      const ts::CameraPose& end = controller.camera().pose();
      const double glideM = targetDistanceM(released, end);
      QVERIFY(glideM > 1.0e4);
      if (!reference) {
        reference = end;
        continue;
      }
      // The drag ends at the same cursor position; its velocity estimate
      // comes from frames at different times, so allow 1 % of the glide.
      const double offM = targetDistanceM(*reference, end);
      qInfo("%3.0f fps: glide %.0f m, %.1f m from the 10 fps result", rate,
            glideM, offM);
      QVERIFY(offM < 0.01 * glideM);
    }
  }

  void stillReleaseHasNoInertia()
  {
    ts::CameraController controller = makeController(samplePoses[0]);
    QVERIFY(controller.beginPan(QPointF(300.0, 300.0), 0.0));
    controller.dragTo(QPointF(350.0, 300.0), 0.05);
    // Held still for half a second before letting go.
    controller.endDrag(0.55);
    QVERIFY(!controller.isAnimating());
  }

  void stallsAreClamped()
  {
    // A 5 s frame moves no further than maximumStepS of motion.
    const ts::CameraPose start{0.0, 0.0, 1.0e5, 0.0, 0.0};
    ts::CameraController stalled = makeController(start);
    stalled.setMotion(ts::MoveForward);
    stalled.advance(5.0);
    ts::CameraController normal = makeController(start);
    normal.setMotion(ts::MoveForward);
    normal.advance(ts::CameraController::maximumStepS);
    QVERIFY(targetDistanceM(stalled.camera().pose(), normal.camera().pose()) <
            1e-6);
  }

  void flyToArrivesExactly()
  {
    ts::CameraController controller = makeController(samplePoses[0]);
    const ts::CameraPose destination{-33.9, 151.2, 2.0e4, 120.0, 45.0};
    controller.flyTo(destination, 2.0);
    QVERIFY(controller.isAnimating());
    runFor(controller, 1.0, 60.0);
    QVERIFY(controller.isAnimating());
    runFor(controller, 1.1, 60.0);
    QVERIFY(!controller.isAnimating());
    const ts::CameraPose& end = controller.camera().pose();
    QCOMPARE(end.targetLatDeg, destination.targetLatDeg);
    QCOMPARE(end.targetLonDeg, destination.targetLonDeg);
    QCOMPARE(end.rangeM, destination.rangeM);
    QCOMPARE(end.headingDeg, destination.headingDeg);
    QCOMPARE(end.tiltDeg, destination.tiltDeg);
  }

  void flyToIsFrameRateIndependent()
  {
    // Halfway through, the pose depends only on elapsed time.
    const ts::CameraPose destination{51.5, -0.1, 1.0e4, 0.0, 30.0};
    std::optional<ts::CameraPose> reference;
    for (const double rate : frameRatesHz) {
      ts::CameraController controller = makeController(samplePoses[0]);
      controller.flyTo(destination, 3.0);
      runFor(controller, 1.5, rate);
      const ts::CameraPose& mid = controller.camera().pose();
      if (!reference) {
        reference = mid;
        continue;
      }
      QVERIFY(targetDistanceM(*reference, mid) < 1e-6);
      QVERIFY(std::abs(mid.rangeM - reference->rangeM) <
              1e-9 * reference->rangeM);
    }
  }

  void longFlightsRise()
  {
    // Halfway along a 10 000 km flight the camera is higher than at
    // either end, so both ends stay in view.
    ts::CameraController controller =
        makeController(ts::CameraPose{40.7, -74.0, 5.0e4, 0.0, 0.0});
    const ts::CameraPose destination{35.7, 139.7, 5.0e4, 0.0, 0.0};
    controller.flyTo(destination, 2.0);
    runFor(controller, 1.0, 60.0);
    QVERIFY(controller.camera().pose().rangeM > 1.0e6);
    const double durationS =
        makeController(samplePoses[0]).flightDurationS(destination);
    QVERIFY(durationS >= 1.0 && durationS <= 4.0);
  }

  void inputStopsFlight()
  {
    ts::CameraController controller = makeController(samplePoses[0]);
    controller.flyTo(samplePoses[1], 2.0);
    controller.zoomAt(QPointF(400.0, 300.0), 1.0);
    QVERIFY(!controller.isAnimating());
    controller.flyTo(samplePoses[1], 2.0);
    controller.setMotion(ts::TurnLeft);
    controller.advance(0.1);
    controller.setMotion(ts::NoMotion);
    QVERIFY(!controller.isAnimating());
  }

  // --- Thread safety and performance ----------------------------------------

  void concurrentOwnInstances()
  {
    // Reentrant: each thread drives its own controller through the same
    // input and must end at the single-threaded pose.
    const std::function<ts::CameraPose()> drive = []() {
      ts::CameraController controller = makeController(samplePoses[1]);
      controller.beginPan(QPointF(300.0, 300.0), 0.0);
      for (int i = 1; i <= 20; ++i) {
        controller.dragTo(QPointF(300.0 + 5.0 * i, 300.0 + 2.0 * i), 0.01 * i);
      }
      controller.endDrag(0.2);
      runFor(controller, 2.0, 60.0);
      controller.flyTo(samplePoses[2], 1.0);
      runFor(controller, 0.5, 60.0);
      return controller.camera().pose();
    };
    const ts::CameraPose expected = drive();
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&drive, &expected](int) {
          const ts::CameraPose pose = drive();
          return pose.targetLatDeg == expected.targetLatDeg &&
                 pose.targetLonDeg == expected.targetLonDeg &&
                 pose.rangeM == expected.rangeM;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkPick()
  {
    const ts::Camera camera = makeCamera(samplePoses[1]);
    QBENCHMARK
    {
      camera.pickGeodetic(321.0, 245.0);
    }
  }

  void benchmarkPanStep()
  {
    ts::CameraController controller = makeController(samplePoses[1]);
    controller.beginPan(QPointF(300.0, 300.0), 0.0);
    double x = 300.0;
    QBENCHMARK
    {
      x = x > 500.0 ? 300.0 : x + 1.0;
      controller.dragTo(QPointF(x, 300.0), 0.0);
    }
  }
};

QTEST_GUILESS_MAIN(TestCamera)
#include "tst_camera.moc"
