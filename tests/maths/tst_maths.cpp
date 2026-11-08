// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>

#include <QtTest>

#include "concurrency.h"
#include "ts/maths/angles.h"
#include "ts/maths/mat4.h"
#include "ts/maths/ray.h"
#include "ts/maths/splitdouble.h"
#include "ts/maths/vec3.h"
#include "ts/maths/vec4.h"

namespace
{

bool nearlyEqual(const ts::Vec3& a, const ts::Vec3& b, double tolerance)
{
  return ts::distance(a, b) <= tolerance;
}

bool nearlyEqual(const ts::Mat4& a, const ts::Mat4& b, double tolerance)
{
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      if (std::abs(a.at(row, column) - b.at(row, column)) > tolerance) {
        return false;
      }
    }
  }
  return true;
}

/** A rotation about z by @p angleRad (built by hand for the tests). */
ts::Mat4 rotationZ(double angleRad)
{
  ts::Mat4 result;
  result.set(0, 0, std::cos(angleRad));
  result.set(0, 1, -std::sin(angleRad));
  result.set(1, 0, std::sin(angleRad));
  result.set(1, 1, std::cos(angleRad));
  return result;
}

/** A simple perspective projection (OpenGL convention) for the tests. */
ts::Mat4 perspective()
{
  const double nearM = 1.0;
  const double farM = 100.0;
  ts::Mat4 result;
  result.set(2, 2, -(farM + nearM) / (farM - nearM));
  result.set(2, 3, -2.0 * farM * nearM / (farM - nearM));
  result.set(3, 2, -1.0);
  result.set(3, 3, 0.0);
  return result;
}

} // namespace

/** Unit tests for ts/maths. */
class TestMaths : public QObject
{
  Q_OBJECT

private slots:
  void angles()
  {
    QCOMPARE(ts::degToRad(180.0), ts::pi);
    QCOMPARE(ts::radToDeg(ts::pi / 2.0), 90.0);
    QVERIFY(std::abs(ts::radToDeg(ts::degToRad(37.25)) - 37.25) < 1e-12);
    static_assert(ts::degToRad(0.0) == 0.0, "degToRad is constexpr");
  }

  void vectorArithmetic()
  {
    const ts::Vec3 a{1.0, 2.0, 3.0};
    const ts::Vec3 b{-4.0, 0.5, 2.0};
    QVERIFY((a + b == ts::Vec3{-3.0, 2.5, 5.0}));
    QVERIFY((a - b == ts::Vec3{5.0, 1.5, 1.0}));
    QVERIFY((-a == ts::Vec3{-1.0, -2.0, -3.0}));
    QVERIFY((a * 2.0 == ts::Vec3{2.0, 4.0, 6.0}));
    QVERIFY((2.0 * a == a * 2.0));
    QVERIFY((a / 2.0 == ts::Vec3{0.5, 1.0, 1.5}));
    ts::Vec3 c = a;
    c += b;
    c -= a;
    c *= 3.0;
    QVERIFY((c == b * 3.0));
    QVERIFY(a != b);
    QVERIFY((ts::Vec3{} == ts::Vec3{0.0, 0.0, 0.0}));
  }

  void dotCrossLength()
  {
    const ts::Vec3 x{1.0, 0.0, 0.0};
    const ts::Vec3 y{0.0, 1.0, 0.0};
    const ts::Vec3 z{0.0, 0.0, 1.0};
    QCOMPARE(ts::dot(x, y), 0.0);
    QVERIFY((ts::cross(x, y) == z));
    QVERIFY((ts::cross(y, z) == x));
    QVERIFY((ts::cross(z, x) == y));
    QVERIFY((ts::cross(y, x) == -z));
    QCOMPARE(ts::length(ts::Vec3{3.0, 4.0, 12.0}), 13.0);
    QCOMPARE(ts::lengthSquared(ts::Vec3{3.0, 4.0, 12.0}), 169.0);
    QCOMPARE(ts::distance(x, y), std::sqrt(2.0));
    QVERIFY(std::abs(ts::length(ts::normalized(ts::Vec3{1e7, -3e6, 2e5})) -
                     1.0) < 1e-15);
    QVERIFY((ts::normalized(ts::Vec3{}) == ts::Vec3{}));
    // ECEF-sized values keep full precision.
    const ts::Vec3 ecef{6378137.0, 0.0, 0.0};
    QCOMPARE(ts::length(ecef + ts::Vec3{0.001, 0.0, 0.0}), 6378137.001);
  }

  void vec4()
  {
    const ts::Vec3 p{1.0, 2.0, 3.0};
    QVERIFY((ts::Vec4::point(p) == ts::Vec4{1.0, 2.0, 3.0, 1.0}));
    QVERIFY((ts::Vec4::direction(p) == ts::Vec4{1.0, 2.0, 3.0, 0.0}));
    QVERIFY((ts::Vec4::point(p).xyz() == p));
    QVERIFY(ts::Vec4::point(p) != ts::Vec4::direction(p));
  }

  void splitDouble()
  {
    // high + low keeps an Earth-radius coordinate to well under a
    // micrometre, where a float alone is off by up to a quarter metre.
    const double x = 6378137.123456789;
    const ts::SplitFloat split = ts::splitDouble(x);
    QCOMPARE(split.high, static_cast<float>(x));
    QVERIFY(std::abs(static_cast<double>(split.high) - x) > 1e-3);
    QVERIFY(std::abs(static_cast<double>(split.high) +
                     static_cast<double>(split.low) - x) < 1e-7);
    // The relative-to-eye difference in float arithmetic, as on the GPU:
    // for nearby points the high parts subtract exactly.
    const double eye = 6378139.987654321;
    const ts::SplitFloat e = ts::splitDouble(eye);
    const float offset = (split.high - e.high) + (split.low - e.low);
    QVERIFY(std::abs(static_cast<double>(offset) - (x - eye)) < 1e-6);
    const ts::SplitVec3 v = ts::splitVec3(ts::Vec3{x, -x, 0.5});
    QCOMPARE(v.highY, -split.high);
    QCOMPARE(v.lowY, -split.low);
    QCOMPARE(v.highZ, 0.5F);
    QCOMPARE(v.lowZ, 0.0F);
  }

  void ray()
  {
    const ts::Ray ray{ts::Vec3{1.0, 1.0, 1.0}, ts::Vec3{0.0, 0.0, -2.0}};
    QVERIFY((ray.pointAt(0.0) == ray.origin));
    QVERIFY((ray.pointAt(1.5) == ts::Vec3{1.0, 1.0, -2.0}));
  }

  void matrixBasics()
  {
    const ts::Mat4 identity;
    QVERIFY(identity == ts::Mat4::identity());
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        QCOMPARE(identity.at(i, j), i == j ? 1.0 : 0.0);
      }
    }
    // Column-major storage: the translation sits in elements 12-14.
    const ts::Mat4 move = ts::Mat4::translation(ts::Vec3{5.0, 6.0, 7.0});
    QCOMPARE(move.elements()[12], 5.0);
    QCOMPARE(move.elements()[13], 6.0);
    QCOMPARE(move.elements()[14], 7.0);
    QVERIFY((move.transformPoint(ts::Vec3{1.0, 1.0, 1.0}) ==
             ts::Vec3{6.0, 7.0, 8.0}));
    QVERIFY((move.transformDirection(ts::Vec3{1.0, 1.0, 1.0}) ==
             ts::Vec3{1.0, 1.0, 1.0}));
    const ts::Mat4 scale = ts::Mat4::scaling(ts::Vec3{2.0, 3.0, 4.0});
    QVERIFY((scale.transformPoint(ts::Vec3{1.0, 1.0, 1.0}) ==
             ts::Vec3{2.0, 3.0, 4.0}));
    QVERIFY(move.transposed().transposed() == move);
    QCOMPARE(move.transposed().at(3, 0), 5.0);
  }

  void matrixProductOrder()
  {
    // a * b applies b first.
    const ts::Mat4 move = ts::Mat4::translation(ts::Vec3{10.0, 0.0, 0.0});
    const ts::Mat4 turn = rotationZ(ts::pi / 2.0);
    const ts::Vec3 p{1.0, 0.0, 0.0};
    QVERIFY(nearlyEqual((move * turn).transformPoint(p),
                        ts::Vec3{10.0, 1.0, 0.0}, 1e-12));
    QVERIFY(nearlyEqual((turn * move).transformPoint(p),
                        ts::Vec3{0.0, 11.0, 0.0}, 1e-12));
  }

  void matrixInverse()
  {
    const ts::Mat4 transform =
        ts::Mat4::translation(ts::Vec3{6378137.0, -1200.5, 42.0}) *
        rotationZ(0.7) * ts::Mat4::scaling(ts::Vec3{2.0, 0.5, 3.0});
    bool invertible = false;
    const ts::Mat4 inverse = transform.inverted(&invertible);
    QVERIFY(invertible);
    QVERIFY(nearlyEqual(transform * inverse, ts::Mat4(), 1e-9));
    QVERIFY(nearlyEqual(inverse * transform, ts::Mat4(), 1e-9));
    const ts::Vec3 p{123.0, -456.0, 789.0};
    QVERIFY(nearlyEqual(inverse.transformPoint(transform.transformPoint(p)), p,
                        1e-8));

    // A projective matrix too.
    const ts::Mat4 projection = perspective();
    const ts::Mat4 projectionInverse = projection.inverted(&invertible);
    QVERIFY(invertible);
    QVERIFY(nearlyEqual(projection * projectionInverse, ts::Mat4(), 1e-12));
  }

  void singularMatrix()
  {
    const ts::Mat4 flat = ts::Mat4::scaling(ts::Vec3{1.0, 1.0, 0.0});
    bool invertible = true;
    QVERIFY(flat.inverted(&invertible) == ts::Mat4());
    QVERIFY(!invertible);
    // The flag is optional.
    QVERIFY(flat.inverted() == ts::Mat4());
  }

  void perspectiveDivide()
  {
    const ts::Mat4 projection = perspective();
    // The near plane maps to z = -1, the far plane to z = 1.
    QVERIFY(std::abs(projection.transformPoint(ts::Vec3{0.0, 0.0, -1.0}).z +
                     1.0) < 1e-12);
    QVERIFY(std::abs(projection.transformPoint(ts::Vec3{0.0, 0.0, -100.0}).z -
                     1.0) < 1e-12);
  }

  void concurrentOwnInstances()
  {
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [](int thread) {
          const ts::Mat4 transform =
              ts::Mat4::translation(ts::Vec3{1.0 * thread, 2.0, 3.0}) *
              rotationZ(0.1 * thread);
          for (int i = 0; i < 1000; ++i) {
            const ts::Mat4 product = transform * transform.inverted();
            if (!nearlyEqual(product, ts::Mat4(), 1e-12)) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkInverse()
  {
    const ts::Mat4 transform =
        ts::Mat4::translation(ts::Vec3{6378137.0, 0.0, 0.0}) * rotationZ(0.3);
    ts::Mat4 inverse;
    QBENCHMARK
    {
      inverse = transform.inverted();
    }
    QVERIFY(nearlyEqual(transform * inverse, ts::Mat4(), 1e-9));
  }

  void benchmarkTransformPoint()
  {
    const ts::Mat4 transform = perspective() * rotationZ(0.3);
    const ts::Vec3 p{1.0, 2.0, -3.0};
    ts::Vec3 result;
    QBENCHMARK
    {
      result = transform.transformPoint(p);
    }
    QVERIFY(std::isfinite(result.x));
  }
};

QTEST_GUILESS_MAIN(TestMaths)
#include "tst_maths.moc"
