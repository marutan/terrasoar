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
#include "ts/camera/scalebar.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/maths/angles.h"
#include "ts/maths/vec4.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/projection/projection.h"

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

/** A camera on the flat map of @p projection (by identifier). */
ts::Camera makeMapCamera(const char* projection, const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(widthPx, heightPx);
  camera.setMapProjection(ts::projectionById(QLatin1String(projection)));
  camera.setMorph(1.0);
  camera.setPose(pose);
  return camera;
}

/** A controller on the flat map of @p projection (by identifier). */
ts::CameraController makeMapController(const char* projection,
                                       const ts::CameraPose& pose)
{
  ts::CameraController controller;
  controller.setViewportSize(widthPx, heightPx);
  controller.setMapProjection(ts::projectionById(QLatin1String(projection)));
  controller.setViewMode(ts::ViewMode::Map, 0.0);
  controller.setPose(pose);
  return controller;
}

/** The map's own zoom: the range a pan must leave unchanged. */
double mapRangeM(const ts::Camera& camera)
{
  return camera.pose().rangeM / camera.mapFrame().planeScale;
}

/** Ground distance between two points, metres. */
double groundDistanceM(const ts::Geodetic& a, const ts::Geodetic& b)
{
  return ts::geodesicInverse(a.latDeg, a.lonDeg, b.latDeg, b.lonDeg).distanceM;
}

/** Where a place is on screen; far off screen if it can't be shown. */
ts::ScreenPoint onScreen(const ts::Camera& camera, double latDeg, double lonDeg)
{
  return camera.projectGeodetic(latDeg, lonDeg)
      .value_or(ts::ScreenPoint{-1e9, -1e9, -1.0});
}

/** The place under a pixel; nowhere on Earth if there is none. */
ts::Geodetic placeAt(const ts::Camera& camera, double xPx, double yPx)
{
  return camera.pickGeodetic(xPx, yPx).value_or(ts::Geodetic{1e9, 1e9, 0.0});
}

/** The scale bar; of no length if there is none. */
ts::ScaleBar barFor(const ts::Camera& camera, double maximumWidthPx)
{
  return ts::scaleBarFor(camera, maximumWidthPx).value_or(ts::ScaleBar{});
}

const std::array<const char*, 3> projectionIds = {"mercator", "web-mercator",
                                                  "miller"};

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

  // --- The flat map: camera --------------------------------------------------

  void mapPoseIsLimited()
  {
    // On the map the view looks straight down, north up, stays inside the
    // projection's latitudes and is never wider than the world.
    ts::Camera camera = makeMapCamera(
        "mercator", ts::CameraPose{89.5, 200.0, 4.9e7, 123.0, 60.0});
    const ts::Projection& mercator = *camera.mapProjection();
    QCOMPARE(camera.morph(), 1.0);
    QVERIFY(camera.isMap());
    QCOMPARE(camera.pose().headingDeg, 0.0);
    QCOMPARE(camera.pose().tiltDeg, 0.0);
    QCOMPARE(camera.pose().targetLatDeg, mercator.maximumLatDeg());
    QCOMPARE(camera.pose().targetLonDeg, -160.0);
    QCOMPARE(camera.pose().rangeM, camera.maximumMapRangeM());
    const ts::MapFrame frame = camera.mapFrame();
    QVERIFY(std::abs(2.0 * frame.halfWidthM - frame.worldWidthM) <
            1e-6 * frame.worldWidthM);
    // A narrower window shows the world from farther away.
    const double wideRangeM = camera.pose().rangeM;
    camera.setViewportSize(widthPx, 2.0 * heightPx);
    camera.setPose(ts::CameraPose{0.0, 0.0, 5.0e7, 0.0, 0.0});
    const double tallLimitM = camera.maximumMapRangeM();
    camera.setViewportSize(2.0 * widthPx, heightPx);
    QVERIFY(camera.maximumMapRangeM() < 0.5 * tallLimitM);
    QCOMPARE(camera.pose().rangeM, camera.maximumMapRangeM());
    QVERIFY(wideRangeM > 0.0);

    // Miller shows the poles; the centre goes up to 89°.
    const ts::Camera miller =
        makeMapCamera("miller", ts::CameraPose{-89.9, 0.0, 1.0e6, 10.0, 10.0});
    QCOMPARE(miller.pose().targetLatDeg, -ts::Camera::maximumMapCentreLatDeg);

    // Part of the way to the map nothing is limited yet.
    ts::Camera between =
        makeCamera(ts::CameraPose{88.0, 0.0, 4.0e7, 30.0, 40.0});
    between.setMorph(0.5);
    QCOMPARE(between.pose().targetLatDeg, 88.0);
    QCOMPARE(between.pose().headingDeg, 30.0);
    QCOMPARE(between.pose().tiltDeg, 40.0);
    QVERIFY(between.isMap());
    between.setMorph(0.49);
    QVERIFY(!between.isMap());
    between.setMorph(7.0);
    QCOMPARE(between.morph(), 1.0);
    between.setMorph(-1.0);
    QCOMPARE(between.morph(), 0.0);
    // A null projection is ignored.
    between.setMapProjection(nullptr);
    QVERIFY(between.mapProjection() == ts::projections().front());
  }

  void mapFrameAndCopies()
  {
    const ts::Camera camera =
        makeMapCamera("mercator", ts::CameraPose{60.0, 170.0, 3.0e6, 0.0, 0.0});
    const ts::MapFrame frame = camera.mapFrame();
    QCOMPARE(frame.centreLatDeg, 60.0);
    QCOMPARE(frame.centreLonDeg, 170.0);
    // True scale along the parallel at the centre: about cos 60°.
    QVERIFY(std::abs(frame.planeScale - 0.5) < 0.002);
    QVERIFY(std::abs(frame.halfWidthM * frame.planeScale -
                     3.0e6 * camera.tanHalfFovX()) < 1e-6);
    QVERIFY(std::abs(frame.worldWidthM -
                     ts::CylindricalProjection::worldWidthM()) < 1e-6);
    // Offsets take the nearest copy of the world: −175° is 15° east.
    const ts::MapPoint east = frame.offsetOf(60.0, -175.0);
    const ts::MapPoint west = frame.offsetOf(60.0, 155.0);
    QVERIFY(std::abs(east.xM + west.xM) < 1e-6);
    QVERIFY(east.xM > 0.0);
    QVERIFY(std::abs(east.yM) < 1e-6);
    QVERIFY(frame.offsetOf(70.0, 170.0).yM > 0.0);

    // The view is 3 000 km × 0.55 × 2 / 0.5 wide on the map: about ±30°.
    using Shifts = std::vector<double>;
    const double world = frame.worldWidthM;
    // In view, one copy.
    QCOMPARE(frame.copyShiftsM(157.5, 180.0, false), Shifts{0.0});
    QCOMPARE(frame.copyShiftsM(-180.0, -157.5, false), Shifts{0.0});
    // Out of view sideways: none.
    QVERIFY(frame.copyShiftsM(0.0, 22.5, false).empty());
    // While the globe unrolls the whole world is filled, and what
    // straddles the edge (here at 170° − 180° = −10°) appears on both
    // sides: its nearest copy is to the east, the other a world west.
    QCOMPARE(frame.copyShiftsM(0.0, 22.5, true), Shifts{0.0});
    QVERIFY(frame.offsetOf(60.0, -11.25).xM > 0.0);
    QCOMPARE(frame.copyShiftsM(-22.5, 0.0, true), (Shifts{0.0, -world}));
    // Zoomed right out the view spans the world: the same there.
    const ts::Camera wide =
        makeMapCamera("mercator", ts::CameraPose{0.0, 170.0, 5.0e7, 0.0, 0.0});
    QCOMPARE(wide.mapFrame().copyShiftsM(-22.5, 0.0, false),
             (Shifts{0.0, -world}));
    QCOMPARE(wide.mapFrame().copyShiftsM(45.0, 67.5, false), Shifts{0.0});
  }

  void mapPickProjectRoundTrip()
  {
    // Pixel → point → pixel on every map, across the antimeridian too.
    for (const char* id : projectionIds) {
      for (const ts::CameraPose& pose :
           {ts::CameraPose{10.0, 20.0, 2.0e6, 0.0, 0.0},
            ts::CameraPose{-60.0, 179.5, 5.0e5, 0.0, 0.0},
            ts::CameraPose{70.0, -120.0, 2.0e4, 0.0, 0.0},
            ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0}}) {
        const ts::Camera camera = makeMapCamera(id, pose);
        // The centre of the view is the target.
        const std::optional<ts::Geodetic> centre =
            camera.pickGeodetic(widthPx / 2.0, heightPx / 2.0);
        if (!centre) {
          QFAIL("centre: no value");
        }
        QVERIFY(std::abs(centre->latDeg - pose.targetLatDeg) < 1e-9);
        QVERIFY(std::abs(ts::normaliseLonDeg(centre->lonDeg -
                                             pose.targetLonDeg)) < 1e-9);
        for (const QPointF pixel :
             {QPointF(0.5, 0.5), QPointF(123.25, 456.75), QPointF(799.5, 300.0),
              QPointF(400.0, 599.5)}) {
          const std::optional<ts::Geodetic> point =
              camera.pickGeodetic(pixel.x(), pixel.y());
          if (!point) {
            continue; // off the top or bottom of the map
          }
          const std::optional<ts::ScreenPoint> back =
              camera.projectGeodetic(point->latDeg, point->lonDeg);
          if (!back) {
            QFAIL("back: no value");
          }
          QVERIFY2(std::abs(back->xPx - pixel.x()) < 1e-6 &&
                       std::abs(back->yPx - pixel.y()) < 1e-6,
                   id);
          QVERIFY(std::abs(back->depthM - camera.pose().rangeM) <
                  1e-9 * camera.pose().rangeM);
          // project() of the ECEF point is the same place.
          const std::optional<ts::ScreenPoint> viaEcef =
              camera.project(ts::geodeticToEcef(*point));
          QVERIFY(viaEcef && std::abs(viaEcef->xPx - pixel.x()) < 1e-6);
        }
      }
    }
  }

  void mapNorthUpAndTrueScaleAtCentre()
  {
    // North is up, east is right, and a short distance at the centre has
    // the size on screen it has on the globe from the same range.
    for (const char* id : projectionIds) {
      const ts::CameraPose pose{52.0, 13.0, 1.0e5, 0.0, 0.0};
      const ts::Camera map = makeMapCamera(id, pose);
      const ts::Camera globe = makeCamera(pose);
      const ts::GeodesicDirect east = ts::geodesicDirect(
          pose.targetLatDeg, pose.targetLonDeg, 90.0, 5000.0);
      const ts::GeodesicDirect north =
          ts::geodesicDirect(pose.targetLatDeg, pose.targetLonDeg, 0.0, 5000.0);
      const ts::ScreenPoint mapEast = onScreen(map, east.latDeg, east.lonDeg);
      const ts::ScreenPoint globeEast =
          onScreen(globe, east.latDeg, east.lonDeg);
      const ts::ScreenPoint mapNorth =
          onScreen(map, north.latDeg, north.lonDeg);
      const ts::ScreenPoint globeNorth =
          onScreen(globe, north.latDeg, north.lonDeg);
      QVERIFY(globeEast.depthM > 0.0 && globeNorth.depthM > 0.0);
      QVERIFY(mapEast.xPx > widthPx / 2.0 + 10.0);
      QVERIFY(std::abs(mapEast.yPx - heightPx / 2.0) < 0.1);
      QVERIFY(mapNorth.yPx < heightPx / 2.0 - 10.0);
      QVERIFY(std::abs(mapNorth.xPx - widthPx / 2.0) < 1e-6);
      // East-west the map is true to scale at the centre (to the globe's
      // curvature over 5 km seen from 100 km: a few thousandths of a pixel).
      QVERIFY2(std::abs(mapEast.xPx - globeEast.xPx) < 0.02, id);
      // North-south too where the projection is conformal; Miller is 6 %
      // short at this latitude, Web Mercator a fraction of a percent off.
      const double northOffPx = std::abs(mapNorth.yPx - globeNorth.yPx);
      const double northPx = heightPx / 2.0 - globeNorth.yPx;
      if (QLatin1String(id) == QLatin1String("mercator")) {
        QVERIFY(northOffPx < 0.02);
      } else if (QLatin1String(id) == QLatin1String("web-mercator")) {
        QVERIFY(northOffPx < 0.01 * northPx);
      } else {
        QVERIFY(northOffPx > 0.04 * northPx && northOffPx < 0.3 * northPx);
      }
    }
  }

  void mapPicksOffTheMapAndAroundTheWorld()
  {
    // Above the top edge of the Mercator map there is nothing to pick.
    const ts::Camera top =
        makeMapCamera("mercator", ts::CameraPose{84.0, 0.0, 2.0e6, 0.0, 0.0});
    QVERIFY(!top.pickGeodetic(widthPx / 2.0, 1.0));
    QVERIFY(top.pickGeodetic(widthPx / 2.0, heightPx - 1.0));
    // Miller's top edge is the pole.
    const ts::Camera pole =
        makeMapCamera("miller", ts::CameraPose{89.0, 0.0, 2.0e6, 0.0, 0.0});
    QVERIFY(!pole.pickGeodetic(widthPx / 2.0, 1.0));
    const std::optional<ts::Geodetic> below =
        pole.pickGeodetic(widthPx / 2.0, heightPx / 2.0 + 20.0);
    QVERIFY(below && below->latDeg < 89.0);
    // East of the antimeridian the longitudes carry on from −180°.
    const ts::Camera pacific =
        makeMapCamera("mercator", ts::CameraPose{0.0, 179.0, 2.0e6, 0.0, 0.0});
    const ts::Geodetic east = placeAt(pacific, widthPx - 1.0, heightPx / 2.0);
    const ts::Geodetic west = placeAt(pacific, 1.0, heightPx / 2.0);
    QVERIFY(east.lonDeg < -170.0 && east.lonDeg > -180.0);
    QVERIFY(west.lonDeg > 165.0 && west.lonDeg < 179.0);
    QVERIFY(std::abs(east.latDeg) < 1e-9);
  }

  void distancesAreTheSameInEveryView()
  {
    // Measuring is geodesic whatever is on screen: two places found on
    // screen and picked back are the same distance apart on the globe, on
    // Mercator and on Miller, to a millimetre.
    const ts::Geodetic a{48.8566, 2.3522, 0.0};
    const ts::Geodetic b{52.52, 13.405, 0.0};
    const double expectedM = groundDistanceM(a, b);
    QVERIFY(std::abs(expectedM - 879.7e3) < 1.0e3); // Paris to Berlin
    const ts::CameraPose pose{50.5, 8.0, 1.5e6, 0.0, 0.0};
    std::vector<ts::Camera> cameras = {makeCamera(pose)};
    for (const char* id : projectionIds) {
      cameras.push_back(makeMapCamera(id, pose));
    }
    for (const ts::Camera& camera : cameras) {
      const ts::ScreenPoint screenA = onScreen(camera, a.latDeg, a.lonDeg);
      const ts::ScreenPoint screenB = onScreen(camera, b.latDeg, b.lonDeg);
      // Both are in the view.
      QVERIFY(screenA.xPx > 0.0 && screenA.xPx < widthPx);
      QVERIFY(screenB.xPx > 0.0 && screenB.xPx < widthPx);
      const ts::Geodetic pickedA = placeAt(camera, screenA.xPx, screenA.yPx);
      const ts::Geodetic pickedB = placeAt(camera, screenB.xPx, screenB.yPx);
      QVERIFY(std::abs(pickedA.latDeg) <= 90.0 &&
              std::abs(pickedB.latDeg) <= 90.0);
      const double measuredM = groundDistanceM(pickedA, pickedB);
      QVERIFY2(
          std::abs(measuredM - expectedM) < 1e-3,
          qPrintable(QStringLiteral("%1 m off on %2")
                         .arg(measuredM - expectedM)
                         .arg(camera.isMap() ? camera.mapProjection()->id()
                                             : QStringLiteral("the globe"))));
    }
    // On screen the two are *not* the same distance apart in every view:
    // only the measurement is.
    const std::function<double(const ts::Camera&)> apartPx =
        [&](const ts::Camera& camera) {
          const ts::ScreenPoint screenA = onScreen(camera, a.latDeg, a.lonDeg);
          const ts::ScreenPoint screenB = onScreen(camera, b.latDeg, b.lonDeg);
          return std::hypot(screenA.xPx - screenB.xPx,
                            screenA.yPx - screenB.yPx);
        };
    const double globePx = apartPx(cameras[0]);
    const double millerPx = apartPx(cameras[3]);
    QVERIFY(std::abs(globePx - millerPx) > 1.0);
  }

  // --- The flat map: changing view -------------------------------------------

  void morphKeepsTheCentre()
  {
    // Globe → map over one second: the morph factor eases from 0 to 1,
    // heading and tilt go to zero, and the centre of the view is the same
    // place in every frame.
    const ts::CameraPose start{48.0, 11.0, 3.0e6, 40.0, 35.0};
    ts::CameraController controller = makeController(start);
    QCOMPARE(controller.viewMode(), ts::ViewMode::Globe);
    QVERIFY(!controller.isMorphing());
    controller.setViewMode(ts::ViewMode::Map, 1.0);
    QCOMPARE(controller.viewMode(), ts::ViewMode::Map);
    QVERIFY(controller.isMorphing() && controller.isAnimating());
    QCOMPARE(controller.camera().morph(), 0.0);
    double previous = 0.0;
    for (int frame = 1; frame <= 59; ++frame) {
      QVERIFY(controller.advance(1.0 / 60.0));
      const ts::Camera& camera = controller.camera();
      QVERIFY(camera.morph() > previous && camera.morph() < 1.0);
      previous = camera.morph();
      QCOMPARE(camera.pose().targetLatDeg, 48.0);
      QCOMPARE(camera.pose().targetLonDeg, 11.0);
      QVERIFY(camera.pose().headingDeg < 40.0 && camera.pose().tiltDeg < 35.0);
      QVERIFY(std::abs(camera.pose().rangeM - 3.0e6) < 1e-6);
      const std::optional<ts::Geodetic> centre =
          camera.pickGeodetic(widthPx / 2.0, heightPx / 2.0);
      if (!centre) {
        QFAIL("centre: no value");
      }
      QVERIFY(std::abs(centre->latDeg - 48.0) < 1e-9);
      QVERIFY(std::abs(centre->lonDeg - 11.0) < 1e-9);
      if (frame == 30) {
        // Ease-in-out: half-way in time is half-way there.
        QVERIFY(std::abs(camera.morph() - 0.5) < 1e-9);
        QVERIFY(std::abs(camera.pose().headingDeg - 20.0) < 1e-9);
      }
    }
    controller.advance(2.0 / 60.0);
    QVERIFY(!controller.isMorphing() && !controller.isAnimating());
    QCOMPARE(controller.camera().morph(), 1.0);
    QCOMPARE(controller.camera().pose().headingDeg, 0.0);
    QCOMPARE(controller.camera().pose().tiltDeg, 0.0);
    QCOMPARE(controller.camera().pose().targetLatDeg, 48.0);
    QCOMPARE(controller.camera().pose().targetLonDeg, 11.0);

    // And back: the same centre, still north up, straight down.
    controller.setViewMode(ts::ViewMode::Globe, 1.0);
    QCOMPARE(controller.viewMode(), ts::ViewMode::Globe);
    runFor(controller, 0.5, 60.0);
    QVERIFY(std::abs(controller.camera().morph() - 0.5) < 1e-9);
    runFor(controller, 0.6, 60.0);
    QVERIFY(!controller.isAnimating());
    QCOMPARE(controller.camera().morph(), 0.0);
    QCOMPARE(controller.camera().pose().targetLatDeg, 48.0);
    QCOMPARE(controller.camera().pose().targetLonDeg, 11.0);
    QCOMPARE(controller.camera().pose().headingDeg, 0.0);
    QVERIFY(std::abs(controller.camera().pose().rangeM - 3.0e6) < 1e-6);
    // Asking for the mode it has does nothing.
    controller.setViewMode(ts::ViewMode::Globe, 1.0);
    QVERIFY(!controller.isAnimating());
  }

  void morphIsFrameRateIndependent()
  {
    // The change takes its duration in seconds at any frame rate: slow
    // frames skip ahead rather than slowing it down.
    for (const double rate : frameRatesHz) {
      ts::CameraController controller =
          makeController(ts::CameraPose{-20.0, 130.0, 8.0e6, 300.0, 50.0});
      controller.setViewMode(ts::ViewMode::Map, 1.0);
      runFor(controller, 0.5, rate);
      QVERIFY2(std::abs(controller.camera().morph() - 0.5) < 1e-9,
               qPrintable(QString::number(rate)));
      // Heading 300° goes the short way round, to 360°.
      QVERIFY(std::abs(controller.camera().pose().headingDeg - 330.0) < 1e-9);
      QVERIFY(std::abs(controller.camera().pose().tiltDeg - 25.0) < 1e-9);
      runFor(controller, 0.5, rate);
      controller.advance(1e-6); // the frame's own rounding
      QVERIFY(!controller.isAnimating());
      QCOMPARE(controller.camera().morph(), 1.0);
    }
    // A stall of several seconds is one clamped step, not a jump to the
    // end.
    ts::CameraController stalled = makeController(samplePoses[1]);
    stalled.setViewMode(ts::ViewMode::Map, 1.0);
    stalled.advance(5.0);
    QVERIFY(stalled.isMorphing());
    QVERIFY(stalled.camera().morph() < 0.2);
  }

  void morphTurnsRoundAndIgnoresInput()
  {
    ts::CameraController controller =
        makeController(ts::CameraPose{30.0, 0.0, 5.0e6, 0.0, 20.0});
    controller.setViewMode(ts::ViewMode::Map, 1.0);
    runFor(controller, 0.25, 100.0);
    const double reached = controller.camera().morph();
    QVERIFY(reached > 0.1 && reached < 0.2);
    // Input waits for the change to finish.
    const ts::CameraPose during = controller.camera().pose();
    QVERIFY(!controller.beginPan(QPointF(400.0, 300.0), 0.0));
    controller.beginRotate(QPointF(400.0, 300.0));
    QVERIFY(!controller.isDragging());
    controller.zoomAt(QPointF(400.0, 300.0), 3.0);
    QCOMPARE(controller.camera().pose().rangeM, during.rangeM);
    controller.setMotion(ts::MoveForward | ts::ZoomIn);
    controller.advance(0.01);
    QCOMPARE(controller.camera().pose().targetLatDeg, 30.0);
    controller.setMotion(ts::NoMotion);
    // Turned round part of the way, it goes back from where it is, taking
    // proportionally less time.
    const double before = controller.camera().morph();
    controller.setViewMode(ts::ViewMode::Globe, 1.0);
    QVERIFY(controller.isMorphing());
    controller.advance(0.001);
    QVERIFY(controller.camera().morph() < before);
    runFor(controller, before * 1.0 + 0.02, 100.0);
    QVERIFY(!controller.isAnimating());
    QCOMPARE(controller.camera().morph(), 0.0);

    // setPose() and flyTo() finish a change at once.
    controller.setViewMode(ts::ViewMode::Map, 1.0);
    controller.advance(0.1);
    controller.setPose(ts::CameraPose{10.0, 10.0, 1.0e6, 45.0, 45.0});
    QVERIFY(!controller.isMorphing());
    QCOMPARE(controller.camera().morph(), 1.0);
    QCOMPARE(controller.camera().pose().headingDeg, 0.0);
    controller.setViewMode(ts::ViewMode::Globe, 1.0);
    controller.advance(0.1);
    controller.flyTo(ts::CameraPose{20.0, 20.0, 1.0e6, 0.0, 0.0}, 0.5);
    QVERIFY(!controller.isMorphing());
    QCOMPARE(controller.camera().morph(), 0.0);
    runFor(controller, 0.6, 100.0);
    QCOMPARE(controller.camera().pose().targetLatDeg, 20.0);
    // A duration of zero changes at once.
    controller.setViewMode(ts::ViewMode::Map, 0.0);
    QVERIFY(!controller.isAnimating());
    QCOMPARE(controller.camera().morph(), 1.0);
  }

  void morphToTheMapMovesAnUnreachableCentre()
  {
    // The Mercator map can't be centred at 88° N: the view slides to the
    // map's edge during the change. Miller can.
    ts::CameraController controller =
        makeController(ts::CameraPose{88.0, 25.0, 2.0e6, 0.0, 0.0});
    controller.setViewMode(ts::ViewMode::Map, 1.0);
    runFor(controller, 1.1, 60.0);
    const double limit =
        ts::projectionById(QStringLiteral("mercator"))->maximumLatDeg();
    QCOMPARE(controller.camera().pose().targetLatDeg, limit);
    QCOMPARE(controller.camera().pose().targetLonDeg, 25.0);
    // Changing the projection applies the new one's limits.
    controller.setMapProjection(ts::projectionById(QStringLiteral("miller")));
    controller.setPose(ts::CameraPose{88.0, 25.0, 2.0e6, 0.0, 0.0});
    QCOMPARE(controller.camera().pose().targetLatDeg, 88.0);
    controller.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
    QCOMPARE(controller.camera().pose().targetLatDeg, limit);
    controller.setMapProjection(nullptr);
    QCOMPARE(controller.camera().mapProjection()->id(),
             QStringLiteral("mercator"));
    // Zoomed out beyond what the map allows, the range comes in too.
    ts::CameraController far =
        makeController(ts::CameraPose{60.0, 0.0, 5.0e7, 0.0, 0.0});
    far.setViewMode(ts::ViewMode::Map, 1.0);
    runFor(far, 0.5, 60.0);
    QVERIFY(far.camera().pose().rangeM < 5.0e7);
    runFor(far, 0.6, 60.0);
    QCOMPARE(far.camera().pose().rangeM, far.camera().maximumMapRangeM());
  }

  // --- The flat map: navigation ----------------------------------------------

  void mapPanKeepsPointUnderCursor()
  {
    // Dragging slides the map: the grabbed place stays under the cursor
    // and the map keeps its size, also across the antimeridian.
    for (const char* id : projectionIds) {
      for (const ts::CameraPose& pose :
           {ts::CameraPose{10.0, 20.0, 2.0e6, 0.0, 0.0},
            ts::CameraPose{60.0, 178.0, 3.0e6, 0.0, 0.0},
            ts::CameraPose{-45.0, -179.0, 8.0e5, 0.0, 0.0}}) {
        ts::CameraController controller = makeMapController(id, pose);
        const double mapRange = mapRangeM(controller.camera());
        const QPointF from(widthPx * 0.4, heightPx * 0.55);
        const std::optional<ts::Geodetic> grabbed =
            controller.camera().pickGeodetic(from.x(), from.y());
        if (!grabbed) {
          QFAIL("grabbed: no value");
        }
        QVERIFY(controller.beginPan(from, 0.0));
        for (int i = 1; i <= 10; ++i) {
          const QPointF to = from + QPointF(23.0 * i, -11.0 * i);
          controller.dragTo(to, 0.01 * i);
          const std::optional<ts::Geodetic> under =
              controller.camera().pickGeodetic(to.x(), to.y());
          if (!under) {
            QFAIL("under: no value");
          }
          QVERIFY2(groundDistanceM(*under, *grabbed) < 1e-3, id);
          QVERIFY(std::abs(mapRangeM(controller.camera()) / mapRange - 1.0) <
                  1e-12);
          QCOMPARE(controller.camera().pose().headingDeg, 0.0);
        }
        controller.endDrag(0.5);
        QVERIFY(!controller.isAnimating());
      }
    }
    // Dragged against the edge of the map, the view stops there.
    ts::CameraController edge = makeMapController(
        "mercator", ts::CameraPose{80.0, 0.0, 1.0e6, 0.0, 0.0});
    QVERIFY(edge.beginPan(QPointF(400.0, 300.0), 0.0));
    edge.dragTo(QPointF(400.0, 5000.0), 0.1);
    edge.endDrag(1.0);
    QCOMPARE(edge.camera().pose().targetLatDeg,
             edge.camera().mapProjection()->maximumLatDeg());
    // Off the map there is nothing to grab.
    QVERIFY(!edge.beginPan(QPointF(400.0, 10.0), 2.0));
  }

  void mapZoomKeepsPointUnderCursor()
  {
    for (const char* id : projectionIds) {
      ts::CameraController controller =
          makeMapController(id, ts::CameraPose{35.0, 139.0, 1.0e6, 0.0, 0.0});
      const QPointF cursor(620.0, 140.0);
      const std::optional<ts::Geodetic> before =
          controller.camera().pickGeodetic(cursor.x(), cursor.y());
      if (!before) {
        QFAIL("before: no value");
      }
      controller.zoomAt(cursor, 3.0);
      QVERIFY(controller.camera().pose().rangeM < 0.6e6);
      const std::optional<ts::Geodetic> after =
          controller.camera().pickGeodetic(cursor.x(), cursor.y());
      if (!after) {
        QFAIL("after: no value");
      }
      QVERIFY2(groundDistanceM(*before, *after) < 1e-3, id);
      // Zooming out stops when the view is as wide as the world.
      controller.zoomAt(QPointF(400.0, 300.0), -60.0);
      const ts::MapFrame frame = controller.camera().mapFrame();
      QVERIFY(std::abs(2.0 * frame.halfWidthM / frame.worldWidthM - 1.0) <
              1e-9);
    }
  }

  void mapKeysAreFrameRateIndependent_data()
  {
    keysAreFrameRateIndependent_data();
  }

  void mapKeysAreFrameRateIndependent()
  {
    // The same 1.5 s of held keys at 10, 20, 60 and 144 fps ends at the
    // same place on the map.
    QFETCH(unsigned int, motion);
    QFETCH(double, toleranceM);
    for (const char* id : projectionIds) {
      const ts::CameraPose start{35.0, 139.0, 4.0e5, 0.0, 0.0};
      std::optional<ts::CameraPose> reference;
      for (const double rate : frameRatesHz) {
        ts::CameraController controller = makeMapController(id, start);
        controller.setMotion(motion);
        QVERIFY(controller.isAnimating());
        runFor(controller, 1.5, rate);
        const ts::CameraPose& end = controller.camera().pose();
        // Turning and tilting do nothing on a map.
        QCOMPARE(end.headingDeg, 0.0);
        QCOMPARE(end.tiltDeg, 0.0);
        if (!reference) {
          reference = end;
          QVERIFY(targetDistanceM(start, end) > 0.0 ||
                  end.rangeM != start.rangeM);
          continue;
        }
        QVERIFY2(targetDistanceM(*reference, end) < toleranceM,
                 qPrintable(QStringLiteral("%1, %2 fps: target %3 m off")
                                .arg(QLatin1String(id))
                                .arg(rate)
                                .arg(targetDistanceM(*reference, end))));
        QVERIFY(std::abs(end.rangeM - reference->rangeM) <
                1e-9 * reference->rangeM);
      }
    }
  }

  void mapKeyRatesAndWrap()
  {
    // One second of Forward moves half the view's range up the map, in a
    // straight line; Right for long enough goes all the way round.
    ts::CameraController controller = makeMapController(
        "mercator", ts::CameraPose{0.0, 0.0, 1.0e5, 0.0, 0.0});
    controller.setMotion(ts::MoveForward);
    runFor(controller, 1.0, 60.0);
    QVERIFY(std::abs(targetDistanceM(ts::CameraPose{0.0, 0.0, 1.0, 0.0, 0.0},
                                     controller.camera().pose()) -
                     5.0e4) < 50.0);
    QCOMPARE(controller.camera().pose().targetLonDeg, 0.0);

    ts::CameraController round = makeMapController(
        "miller", ts::CameraPose{40.0, 170.0, 2.0e7, 0.0, 0.0});
    const double mapRange = mapRangeM(round.camera());
    round.setMotion(ts::MoveRight);
    bool crossed = false;
    double previousLon = 170.0;
    for (int i = 0; i < 400 && !crossed; ++i) {
      round.advance(0.05);
      const double lon = round.camera().pose().targetLonDeg;
      crossed = lon < previousLon;
      previousLon = lon;
      QVERIFY(std::abs(round.camera().pose().targetLatDeg - 40.0) < 1e-9);
    }
    QVERIFY(crossed);
    QVERIFY(previousLon < -170.0);
    QVERIFY(std::abs(mapRangeM(round.camera()) / mapRange - 1.0) < 1e-9);

    // Up for long enough stops at the top of the map.
    ts::CameraController up = makeMapController(
        "web-mercator", ts::CameraPose{70.0, 0.0, 5.0e6, 0.0, 0.0});
    up.setMotion(ts::MoveForward);
    runFor(up, 20.0, 20.0);
    QCOMPARE(up.camera().pose().targetLatDeg,
             up.camera().mapProjection()->maximumLatDeg());
    QVERIFY(std::isfinite(up.camera().pose().rangeM));
  }

  void mapInertiaIsFrameRateIndependent()
  {
    const ts::CameraPose start{20.0, 30.0, 2.0e6, 0.0, 0.0};
    std::optional<ts::CameraPose> reference;
    for (const double rate : frameRatesHz) {
      ts::CameraController controller = makeMapController("mercator", start);
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
      // A steady drag has the same velocity however it was sampled.
      QVERIFY(targetDistanceM(*reference, end) < 0.01 * glideM);
    }
    // Held still before letting go: no glide.
    ts::CameraController still = makeMapController("mercator", start);
    QVERIFY(still.beginPan(QPointF(300.0, 300.0), 0.0));
    still.dragTo(QPointF(350.0, 300.0), 0.05);
    still.endDrag(0.6);
    QVERIFY(!still.isAnimating());
  }

  void mapFlyToArrives()
  {
    ts::CameraController controller = makeMapController(
        "mercator", ts::CameraPose{10.0, 10.0, 2.0e6, 0.0, 0.0});
    const ts::CameraPose destination{-33.9, 151.2, 5.0e5, 77.0, 33.0};
    controller.flyTo(destination, 1.0);
    runFor(controller, 1.1, 60.0);
    QVERIFY(!controller.isAnimating());
    QCOMPARE(controller.camera().pose().targetLatDeg, -33.9);
    QCOMPARE(controller.camera().pose().targetLonDeg, 151.2);
    QCOMPARE(controller.camera().pose().rangeM, 5.0e5);
    // Still a map: north up, straight down.
    QCOMPARE(controller.camera().pose().headingDeg, 0.0);
    QCOMPARE(controller.camera().pose().tiltDeg, 0.0);
  }

  // --- Scale bar
  // --------------------------------------------------------------

  void scaleBarIsRoundAndTrue()
  {
    // The bar is 1, 2 or 5 × 10ⁿ metres, fits the width asked for, and
    // two points that far apart at the centre are that far apart on
    // screen: on the globe and on every map.
    const ts::CameraPose pose{60.0, 25.0, 3.0e5, 0.0, 0.0};
    std::vector<ts::Camera> cameras = {makeCamera(pose)};
    for (const char* id : projectionIds) {
      cameras.push_back(makeMapCamera(id, pose));
    }
    for (const ts::Camera& camera : cameras) {
      const std::optional<ts::ScaleBar> bar = ts::scaleBarFor(camera, 140.0);
      if (!bar) {
        QFAIL("bar: no value");
      }
      QVERIFY(bar->widthPx <= 140.0 && bar->widthPx > 140.0 / 2.5 - 1e-9);
      const double mantissa =
          bar->lengthM / std::pow(10.0, std::floor(std::log10(bar->lengthM)));
      QVERIFY(std::abs(mantissa - 1.0) < 1e-9 ||
              std::abs(mantissa - 2.0) < 1e-9 ||
              std::abs(mantissa - 5.0) < 1e-9);
      // 3D and maps agree at the centre: the same bar (here 50 km).
      QCOMPARE(bar->lengthM, 5.0e4);
      QVERIFY(std::abs(bar->latDeg - 60.0) < 1e-6);
      QVERIFY(!bar->variesAcrossView);
      const ts::GeodesicDirect west =
          ts::geodesicDirect(60.0, 25.0, 270.0, 0.5 * bar->lengthM);
      const ts::GeodesicDirect east =
          ts::geodesicDirect(60.0, 25.0, 90.0, 0.5 * bar->lengthM);
      const ts::ScreenPoint left = onScreen(camera, west.latDeg, west.lonDeg);
      const ts::ScreenPoint right = onScreen(camera, east.latDeg, east.lonDeg);
      QVERIFY(left.depthM > 0.0 && right.depthM > 0.0);
      QVERIFY(std::abs((right.xPx - left.xPx) / bar->widthPx - 1.0) < 2e-3);
    }
  }

  void scaleBarKnowsWhereItHolds()
  {
    // A Mercator map of half the world is at very different scales top
    // and bottom; the bar says so and gives its latitude.
    const ts::Camera world =
        makeMapCamera("mercator", ts::CameraPose{40.0, 0.0, 2.0e7, 0.0, 0.0});
    const ts::ScaleBar wide = barFor(world, 140.0);
    QVERIFY(wide.lengthM > 0.0 && wide.variesAcrossView);
    QVERIFY(std::abs(wide.latDeg - 40.0) < 1e-6);
    // The whole globe in view: the scale falls away towards the limb.
    const ts::ScaleBar globe =
        barFor(makeCamera(ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0}), 140.0);
    QVERIFY(globe.lengthM > 0.0 && globe.variesAcrossView);
    // Close in, one bar holds for the whole view.
    const ts::ScaleBar close = barFor(
        makeMapCamera("mercator", ts::CameraPose{40.0, 0.0, 2.0e3, 0.0, 0.0}),
        140.0);
    QVERIFY(!close.variesAcrossView);
    QCOMPARE(close.lengthM, 200.0);
    // The same range at a higher latitude is the same ground distance:
    // the map is drawn to the scale of its centre.
    const ts::ScaleBar north = barFor(
        makeMapCamera("mercator", ts::CameraPose{70.0, 0.0, 2.0e3, 0.0, 0.0}),
        140.0);
    QCOMPARE(north.lengthM, 200.0);
    QVERIFY(std::abs(north.widthPx / close.widthPx - 1.0) < 1e-3);
    // Nothing to measure with no width, or looking past the globe.
    QVERIFY(!ts::scaleBarFor(world, 0.0));
    // One metre up: centimetres.
    const ts::ScaleBar ground =
        barFor(makeCamera(ts::CameraPose{10.0, 10.0, 1.0, 0.0, 0.0}), 140.0);
    QVERIFY(ground.lengthM <= 0.2 && ground.lengthM >= 0.05);
  }

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
