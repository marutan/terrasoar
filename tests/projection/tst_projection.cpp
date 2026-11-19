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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <proj.h>

#include "concurrency.h"
#include "nationalgriddata.h"
#include "offscreengl.h"
#include "ts/deps/projcontext.h"
#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/projection/datumshift.h"
#include "ts/projection/nationalgrid.h"
#include "ts/projection/projection.h"
#include "ts/projection/rotatedmercator.h"

namespace
{

/** The Phase 4 plan's tolerance for projections: one millimetre. */
constexpr double toleranceM = 1e-3;

/** A millimetre on the ground as an angle, degrees (at the equator). */
constexpr double toleranceDeg = 1e-8;

/** Stands in for a point inverse() didn't give: far from any real one, so
 *  the comparison that follows fails. */
constexpr ts::Geodetic nowhere{1e9, 1e9, -1.0};

/** A PROJ transformation from a PROJ string (radians in, metres out). */
class ProjPipeline
{
public:
  explicit ProjPipeline(const QString& definition)
      : m_transform(
            proj_create(m_context.get(), definition.toUtf8().constData()))
  {
  }
  ~ProjPipeline() { proj_destroy(m_transform); }
  ProjPipeline(const ProjPipeline&) = delete;
  ProjPipeline& operator=(const ProjPipeline&) = delete;
  ProjPipeline(ProjPipeline&&) = delete;
  ProjPipeline& operator=(ProjPipeline&&) = delete;

  bool isValid() const { return m_transform != nullptr; }

  ts::MapPoint forward(double latDeg, double lonDeg) const
  {
    const PJ_COORD out = proj_trans(
        m_transform, PJ_FWD,
        proj_coord(ts::degToRad(lonDeg), ts::degToRad(latDeg), 0.0, 0.0));
    return ts::MapPoint{out.xy.x, out.xy.y};
  }

  ts::Geodetic inverse(ts::MapPoint point) const
  {
    const PJ_COORD out = proj_trans(m_transform, PJ_INV,
                                    proj_coord(point.xM, point.yM, 0.0, 0.0));
    return ts::Geodetic{ts::radToDeg(out.lp.phi), ts::radToDeg(out.lp.lam),
                        0.0};
  }

private:
  ts::ProjContext m_context;
  PJ* m_transform = nullptr;
};

/** Stands in for offsets a grid didn't give. */
constexpr std::array<double, 2> noOffset{1e9, 1e9};

/** Stands in for a grid reference that didn't parse. */
constexpr ts::MapPoint noPoint{-1.0, -1.0};

/** Latitudes of the global grid inside @p projection's domain. */
std::vector<double> gridLatitudes(const ts::Projection& projection)
{
  std::vector<double> lats = {projection.minimumLatDeg(),
                              projection.maximumLatDeg(), 1e-9, -1e-9};
  for (int32_t step = -36; step <= 36; ++step) {
    const double lat = 2.5 * step;
    if (projection.containsLatitude(lat)) {
      lats.push_back(lat);
    }
  }
  return lats;
}

/** Longitudes of the global grid, with the antimeridian's two sides. */
std::vector<double> gridLongitudes(const ts::Projection& projection)
{
  const double limit = projection.maximumLonOffsetDeg();
  if (limit >= 180.0) {
    std::vector<double> lons = {-180.0, -179.999999, 179.999999};
    for (int32_t step = -23; step < 24; ++step) {
      lons.push_back(7.5 * step);
    }
    return lons;
  }
  // The domain's edges and a grid inside it, about the central meridian.
  const double centre = projection.centralMeridianDeg();
  std::vector<double> lons = {centre - limit, centre + limit,
                              centre - limit + 1e-6, centre + 1e-9};
  for (int32_t step = 1; -limit + 2.5 * step < limit; ++step) {
    lons.push_back(centre - limit + 2.5 * step);
  }
  return lons;
}

/** The ground distance of a difference in latitude and longitude, metres. */
double groundDistanceM(const ts::Geodetic& a, const ts::Geodetic& b)
{
  return ts::geodesicInverse(a.latDeg, a.lonDeg, b.latDeg, b.lonDeg).distanceM;
}

// The fragment shader of glslMatchesCpp(): pixel i computes the offset
// of sample i and writes one component as the float's four bytes.
const char* const probeVertexGlsl = R"(
void main()
{
  vec2 ndc = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

const char* const probeFragmentGlsl = R"(
layout(std140) uniform Probe {
  vec4 r0;
  vec4 r1;
  vec4 r2;
  vec4 r3;
  vec4 steps;  // dLon per sample, dLat per sample, first sample, component
  vec4 which;  // projection index
};
out vec4 fragColor;
void main()
{
  float sampleIndex = floor(gl_FragCoord.x) + steps.z;
  vec2 d = vec2(steps.x, steps.y) * sampleIndex;
  vec2 result = tsProject(int(which.x), d, r0, r1, r2, r3);
  uint bits = floatBitsToUint(steps.w < 0.5 ? result.x : result.y);
  fragColor = vec4(float(bits & 255u), float((bits >> 8) & 255u),
                   float((bits >> 16) & 255u), float(bits >> 24)) / 255.0;
}
)";

struct ProbeBlock {
  std::array<float, 4> r0{};
  std::array<float, 4> r1{};
  std::array<float, 4> r2{};
  std::array<float, 4> r3{};
  std::array<float, 4> steps{};
  std::array<float, 4> which{};
};

/**
 * Returns whichever of PROJ's position @p theirs and its copies a turn of
 * the centre line away is nearest @p ours: Rotated Mercator runs on
 * across PROJ's cut. Others' positions as they are.
 */
ts::MapPoint nearestCopy(const ts::Projection& projection,
                         // Ours, then PROJ's.
                         // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                         ts::MapPoint ours, ts::MapPoint theirs)
{
  const ts::RotatedMercatorProjection* rotated =
      dynamic_cast<const ts::RotatedMercatorProjection*>(&projection);
  if (rotated == nullptr) {
    return theirs;
  }
  const ts::MapPoint turn = rotated->alongTurnM();
  ts::MapPoint best = theirs;
  for (const double k : {-1.0, 1.0}) {
    const ts::MapPoint shifted{theirs.xM + k * turn.xM,
                               theirs.yM + k * turn.yM};
    if (std::hypot(shifted.xM - ours.xM, shifted.yM - ours.yM) <
        std::hypot(best.xM - ours.xM, best.yM - ours.yM)) {
      best = shifted;
    }
  }
  return best;
}

/** True unless the point is outside @p projection's own domain, where
 *  the map draws nothing. */
// Latitude then longitude, as everywhere.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool drawnAt(const ts::Projection& projection, double latDeg, double lonDeg)
{
  return !projection.hasOwnDomain() || projection.contains(latDeg, lonDeg);
}

/**
 * Draws the probe of glslMatchesCpp() for @p probe (its steps' component
 * is set here) and returns each of the @p samples offsets, x and y, as the
 * shader computed them.
 */
std::vector<std::array<float, 2>>
runProbe(ts::RenderDevice& device, ts::Pipeline& pipeline,
         ts::RenderTarget& target, ts::GpuBuffer& uniforms,
         const ProbeBlock& probe, int32_t samples)
{
  ProbeBlock block = probe;
  std::array<QImage, 2> images;
  for (std::size_t component = 0; component < images.size(); ++component) {
    block.steps[3] = static_cast<float>(component);
    uniforms.update(0, &block, sizeof(block));
    ts::PassDesc pass;
    pass.target = &target;
    pass.viewportPx = QRect(0, 0, samples, 1);
    device.beginPass(pass);
    device.setPipeline(pipeline);
    device.setUniformBuffer(0, uniforms, 0, sizeof(block));
    device.draw(3, 0, 1);
    device.endPass();
    images.at(component) = device.readPixels(target);
  }
  std::vector<std::array<float, 2>> values(static_cast<std::size_t>(samples));
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t component = 0; component < images.size(); ++component) {
      std::memcpy(&values.at(i).at(component),
                  images.at(component).constScanLine(0) +
                      4 * static_cast<std::ptrdiff_t>(i),
                  4);
    }
  }
  return values;
}

/** What the probe should read back for one offset, and the allowance. */
struct ProbeExpectation {
  /** The offset from the reference, metres. */
  double xM = 0.0;
  /** The same in y. */
  double yM = 0.0;
  /** The largest error allowed in x, metres. */
  double allowedXM = 0.0;
  /** The same in y. */
  double allowedYM = 0.0;
};

/**
 * Returns the C++ offset of @p dLonRad, @p dLatRad from the reference at
 * latitude @p referenceLat (longitude 0, map position @p reference) and
 * how far the GPU may be from it for samples spanning @p spanDeg.
 */
ProbeExpectation
expectedProbeOffset(const ts::Projection& projection, double referenceLat,
                    ts::MapPoint reference,
                    // Longitude then latitude, as the
                    // probe's offsets.
                    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                    double dLonRad, double dLatRad, double spanDeg)
{
  ProbeExpectation result;
  if (projection.isCylindrical()) {
    const ts::MapPoint expected =
        projection.forward(referenceLat + ts::radToDeg(dLatRad), 0.0);
    // x is linear in the longitude on cylindrical maps.
    result.xM = projection.bounds().widthM() / (2.0 * ts::pi) * dLonRad;
    result.yM = expected.yM - reference.yM;
    // Float arithmetic: a few parts in ten million of the offset, more
    // for offsets of tens of degrees (which only the coarsest tiles have,
    // where a pixel is kilometres).
    const double relative = 2e-6 + 2e-5 * std::abs(dLatRad);
    result.allowedXM = 1e-6 + 2e-6 * std::abs(result.xM);
    result.allowedYM = 1e-6 + relative * std::abs(result.yM);
    return result;
  }
  const ts::MapPoint expected = projection.forward(
      referenceLat + ts::radToDeg(dLatRad), ts::radToDeg(dLonRad));
  result.xM = expected.xM - reference.xM;
  result.yM = expected.yM - reference.yM;
  // The expansion (small spans) or the float formula (large ones): within
  // a hundredth of a texel of a 256-pixel tile of this span, and a
  // millimetre.
  const double texelM =
      ts::degToRad(spanDeg) * ts::wgs84::semiMajorAxisM / 256.0;
  result.allowedXM = 1e-3 + 0.01 * texelM;
  result.allowedYM = result.allowedXM;
  return result;
}

} // namespace

/** Unit tests for ts/projection. */
class TestProjection : public QObject
{
  Q_OBJECT

private slots:
  void listOfProjections()
  {
    const std::vector<const ts::Projection*>& all = ts::projections();
    QCOMPARE(all.size(), std::size_t{11});
    QSet<QString> ids;
    QSet<QByteArray> functions;
    for (std::size_t i = 0; i < all.size(); ++i) {
      const ts::Projection* projection = all[i];
      QVERIFY(!projection->id().isEmpty());
      QVERIFY(!projection->displayName().isEmpty());
      ids.insert(projection->id());
      functions.insert(projection->glslFunction());
      QCOMPARE(ts::projectionById(projection->id()), projection);
      QCOMPARE(ts::projectionIndex(projection), static_cast<int32_t>(i));
      // UTM's default zone (30) is the one west of Greenwich; the
      // National Grid's origin is at 2° W.
      double centre = 0.0;
      if (projection->id() == QLatin1String("utm")) {
        centre = -3.0;
      } else if (projection->id() == QLatin1String("uk-national-grid")) {
        centre = -2.0;
      }
      QCOMPARE(projection->centralMeridianDeg(), centre);
    }
    QCOMPARE(ids.size(), 11);
    QCOMPARE(functions.size(), 11);
    QCOMPARE(all.front()->id(), QStringLiteral("mercator"));
    QVERIFY(ts::projectionById(QStringLiteral("web-mercator")) != nullptr);
    QVERIFY(ts::projectionById(QStringLiteral("miller")) != nullptr);
    QVERIFY(ts::projectionById(QStringLiteral("no-such")) == nullptr);
    QVERIFY(ts::projectionById(QString()) == nullptr);
    // Another instance shares its projection's shader function.
    const ts::MercatorProjection other;
    QCOMPARE(ts::projectionIndex(&other), 0);
    QCOMPARE(ts::projectionIndex(nullptr), -1);
  }

  void againstProj_data()
  {
    QTest::addColumn<int32_t>("index");
    QTest::newRow("mercator") << 0;
    QTest::newRow("web-mercator") << 1;
    QTest::newRow("miller") << 2;
    QTest::newRow("equidistant-cylindrical") << 3;
    QTest::newRow("gall-stereographic") << 4;
    QTest::newRow("gall-peters") << 5;
    QTest::newRow("transverse-mercator") << 6;
    QTest::newRow("utm") << 7;
    QTest::newRow("cassini") << 8;
    // With the Helmert transform (no OSTN15 grid installed).
    QTest::newRow("uk-national-grid") << 9;
    // Inside its own domain.
    QTest::newRow("rotated-mercator") << 10;
  }

  void againstProj()
  {
    // Forward and inverse agree with PROJ to a millimetre over a global
    // grid, the limits of the domain and both sides of the antimeridian.
    QFETCH(int32_t, index);
    const ts::Projection& projection =
        *ts::projections().at(static_cast<std::size_t>(index));
    const ProjPipeline proj(projection.projString());
    QVERIFY2(proj.isValid(), qPrintable(projection.projString()));
    double worstForwardM = 0.0;
    double worstInverseDeg = 0.0;
    for (const double lat : gridLatitudes(projection)) {
      for (const double lon : gridLongitudes(projection)) {
        if (!projection.contains(lat, lon)) {
          continue; // outside a domain of its own: not drawn
        }
        const ts::MapPoint ours = projection.forward(lat, lon);
        // PROJ puts −180° at +πa, we at −πa: the same meridian.
        ts::MapPoint theirs = proj.forward(lat, lon);
        if (lon == -180.0 && projection.repeats()) {
          theirs.xM = -std::abs(theirs.xM);
        }
        theirs = nearestCopy(projection, ours, theirs);
        worstForwardM = std::max({worstForwardM, std::abs(ours.xM - theirs.xM),
                                  std::abs(ours.yM - theirs.yM)});
        const ts::Geodetic back = projection.inverse(theirs).value_or(nowhere);
        // PROJ's inverse of the ellipsoidal equal-area map is a truncated
        // series, out by up to about 1e-8° (and at its south pole, where
        // the northing is flat in the latitude, by 2e-6°); its inverse of
        // the National Grid's Helmert pipeline restarts the OSGB36 height
        // at zero, out by up to 7e-6° in longitude. Where PROJ doesn't
        // come back to the point itself, the point is the reference.
        ts::Geodetic projBack = proj.inverse(theirs);
        if (std::abs(projBack.latDeg - lat) > toleranceDeg ||
            (std::abs(lat) < 90.0 &&
             std::abs(ts::normaliseLonDeg(projBack.lonDeg - lon)) >
                 toleranceDeg)) {
          projBack = ts::Geodetic{lat, lon, 0.0};
        }
        if (std::abs(lat) == 90.0) {
          // Every longitude is the pole.
          worstInverseDeg = std::max(worstInverseDeg,
                                     std::abs(back.latDeg - projBack.latDeg));
          continue;
        }
        worstInverseDeg = std::max(
            {worstInverseDeg, std::abs(back.latDeg - projBack.latDeg),
             std::abs(ts::normaliseLonDeg(back.lonDeg - projBack.lonDeg))});
      }
    }
    QVERIFY2(worstForwardM < toleranceM,
             qPrintable(QString::number(worstForwardM, 'g', 6)));
    QVERIFY2(worstInverseDeg < toleranceDeg,
             qPrintable(QString::number(worstInverseDeg, 'g', 6)));
  }

  void roundTrip_data() { againstProj_data(); }

  void parameterisedInstances()
  {
    // projectionWith(): defaults give the listed instance, other values a
    // kept instance that shares the listed one's shader index and agrees
    // with PROJ; values are clamped; unknown ids give null.
    const QString id = QStringLiteral("equidistant-cylindrical");
    const ts::Projection* standard = ts::projectionById(id);
    if (standard == nullptr) {
      QFAIL("no equidistant-cylindrical");
    }
    QCOMPARE(ts::projectionWith(id, {}), standard);
    QCOMPARE(
        ts::projectionWith(id, {{QStringLiteral("standardParallel"), 0.0}}),
        standard);
    const ts::Projection* thirty =
        ts::projectionWith(id, {{QStringLiteral("standardParallel"), 30.0}});
    if (thirty == nullptr) {
      QFAIL("no instance for 30°");
    }
    QVERIFY(thirty != standard);
    QCOMPARE(
        ts::projectionWith(id, {{QStringLiteral("standardParallel"), 30.0}}),
        thirty);
    QCOMPARE(ts::projectionIndex(thirty), ts::projectionIndex(standard));
    QCOMPARE(thirty->parameterValues().at(QStringLiteral("standardParallel")),
             30.0);
    const ts::Projection* clamped =
        ts::projectionWith(id, {{QStringLiteral("standardParallel"), 120.0}});
    if (clamped == nullptr) {
      QFAIL("no clamped instance");
    }
    QCOMPARE(clamped->parameterValues().at(QStringLiteral("standardParallel")),
             85.0);
    // Parameterless projections ignore values.
    const ts::Projection* mercator =
        ts::projectionById(QStringLiteral("mercator"));
    QCOMPARE(ts::projectionWith(QStringLiteral("mercator"),
                                {{QStringLiteral("standardParallel"), 30.0}}),
             mercator);
    QVERIFY(ts::projectionWith(QStringLiteral("no-such"), {}) == nullptr);

    const ProjPipeline proj(thirty->projString());
    QVERIFY2(proj.isValid(), qPrintable(thirty->projString()));
    double worstM = 0.0;
    for (const double lat : gridLatitudes(*thirty)) {
      for (const double lon : {-179.0, -45.0, 0.0, 12.5, 179.0}) {
        const ts::MapPoint ours = thirty->forward(lat, lon);
        const ts::MapPoint theirs = proj.forward(lat, lon);
        worstM = std::max({worstM, std::abs(ours.xM - theirs.xM),
                           std::abs(ours.yM - theirs.yM)});
      }
    }
    QVERIFY2(worstM < toleranceM, qPrintable(QString::number(worstM, 'g', 6)));
    // True to scale along the standard parallel.
    QVERIFY(std::abs(thirty->scaleAt(30.0).parallel - 1.0) < 0.01);
  }

  void roundTrip()
  {
    // forward then inverse comes back within a millimetre on the ground.
    QFETCH(int32_t, index);
    const ts::Projection& projection =
        *ts::projections().at(static_cast<std::size_t>(index));
    double worstM = 0.0;
    for (const double lat : gridLatitudes(projection)) {
      for (const double lon : gridLongitudes(projection)) {
        if (!projection.contains(lat, lon)) {
          continue; // outside a domain of its own: not drawn
        }
        const ts::Geodetic back =
            projection.inverse(projection.forward(lat, lon)).value_or(nowhere);
        QCOMPARE(back.heightM, 0.0);
        worstM = std::max(worstM,
                          groundDistanceM(ts::Geodetic{lat, lon, 0.0}, back));
      }
    }
    QVERIFY2(worstM < toleranceM, qPrintable(QString::number(worstM, 'g', 6)));
  }

  void knownValues()
  {
    // Textbook values (Snyder, Map Projections: A Working Manual, and the
    // EPSG guidance note 7-2 example for EPSG:3857).
    const ts::MercatorProjection mercator;
    const ts::WebMercatorProjection web;
    const ts::MillerProjection miller;
    const double a = ts::wgs84::semiMajorAxisM;
    QCOMPARE(mercator.forward(0.0, 0.0).xM, 0.0);
    QCOMPARE(mercator.forward(0.0, 0.0).yM, 0.0);
    QCOMPARE(mercator.forward(0.0, 90.0).xM, a * ts::pi / 2.0);
    // EPSG:3857 example: 24°22′54.433″ N, 100°20′0″ W.
    const ts::MapPoint epsg = web.forward(24.0 + 22.0 / 60.0 + 54.433 / 3600.0,
                                          -(100.0 + 20.0 / 60.0));
    QVERIFY(std::abs(epsg.xM - (-11169055.58)) < 0.01);
    QVERIFY(std::abs(epsg.yM - 2800000.00) < 0.01);
    // Miller on the unit sphere at 50° N: y = 1.25 ln tan(45° + 20°).
    const double expected = 1.25 * std::log(std::tan(ts::degToRad(65.0))) * a;
    QVERIFY(std::abs(miller.forward(50.0, 0.0).yM - expected) < 1e-6);
    // The ellipsoidal Mercator is a little lower than the spherical one.
    QVERIFY(mercator.forward(45.0, 0.0).yM < web.forward(45.0, 0.0).yM);
    QVERIFY(mercator.forward(45.0, 0.0).yM > 0.99 * web.forward(45.0, 0.0).yM);
  }

  void domainAndBounds()
  {
    const ts::MercatorProjection mercator;
    const ts::WebMercatorProjection web;
    const ts::MillerProjection miller;
    // Web Mercator's well-known limit; the ellipsoidal one is a bit higher.
    QVERIFY(std::abs(web.maximumLatDeg() - 85.0511287798066) < 1e-9);
    QVERIFY(std::abs(mercator.maximumLatDeg() - 85.0840590501) < 1e-6);
    QCOMPARE(miller.maximumLatDeg(), 90.0);
    for (const ts::Projection* projection : ts::projections()) {
      QCOMPARE(projection->minimumLatDeg(), -projection->maximumLatDeg());
      QVERIFY(projection->containsLatitude(0.0));
      QVERIFY(projection->containsLatitude(projection->maximumLatDeg()));
      QVERIFY(!projection->containsLatitude(90.5));
      const ts::MapRect bounds = projection->bounds();
      QVERIFY(std::isfinite(bounds.westM) && std::isfinite(bounds.northM));
      QVERIFY(!projection->inverse(ts::MapPoint{std::nan(""), 0.0}));
      if (!projection->repeats()) {
        continue; // nonRepeatingDomains()
      }
      // A whole turn of longitude wide.
      QVERIFY(std::abs(bounds.widthM() -
                       2.0 * projection->forward(0.0, 90.0).xM * 2.0) < 1e-6);
      QCOMPARE(bounds.westM, -bounds.eastM);
      QCOMPARE(bounds.southM, -bounds.northM);
      // Latitudes beyond the domain land on the map's edge.
      QCOMPARE(projection->forward(90.0, 10.0).yM, bounds.northM);
      QCOMPARE(projection->forward(-90.0, 10.0).yM, bounds.southM);
      QVERIFY(std::isfinite(bounds.northM));
      // Off the map there is no point.
      QVERIFY(!projection->inverse(ts::MapPoint{0.0, bounds.northM + 1.0}));
      QVERIFY(!projection->inverse(ts::MapPoint{0.0, bounds.southM - 1.0}));
      QVERIFY(!projection->inverse(ts::MapPoint{bounds.eastM + 1.0, 0.0}));
      QVERIFY(!projection->inverse(ts::MapPoint{bounds.westM - 1.0, 0.0}));
      QVERIFY(!projection->inverse(ts::MapPoint{std::nan(""), 0.0}));
      QVERIFY(projection->inverse(ts::MapPoint{bounds.eastM, bounds.northM}));
    }
    // The Mercator maps are square.
    QVERIFY(std::abs(mercator.bounds().heightM() - mercator.bounds().widthM()) <
            1e-6);
    QVERIFY(std::abs(web.bounds().heightM() - web.bounds().widthM()) < 1e-6);
    // Miller's is about 1.36 times as wide as it is high.
    QVERIFY(std::abs(miller.bounds().widthM() / miller.bounds().heightM() -
                     1.3638) < 1e-3);
  }

  void nonRepeatingDomains()
  {
    // Projections whose domain is limited in longitude: they don't
    // repeat, points outside have no inverse, forward() holds longitudes
    // to the domain, and bounds() encloses the whole domain.
    int32_t limited = 0;
    for (const ts::Projection* projection : ts::projections()) {
      if (projection->hasOwnDomain()) {
        continue; // rotatedMercatorDomain()
      }
      const double limit = projection->maximumLonOffsetDeg();
      if (limit >= 180.0) {
        QVERIFY(projection->repeats());
        QVERIFY(projection->contains(0.0, 179.9));
        continue;
      }
      ++limited;
      QVERIFY2(!projection->repeats(), qPrintable(projection->id()));
      const double centre = projection->centralMeridianDeg();
      QVERIFY(projection->contains(10.0, centre + limit));
      QVERIFY(!projection->contains(10.0, centre + limit + 0.1));
      QCOMPARE(projection->clampLonDeg(centre + limit + 20.0), centre + limit);
      QCOMPARE(projection->clampLonDeg(centre - 5.0), centre - 5.0);
      QCOMPARE(projection->forward(20.0, centre + limit + 30.0).xM,
               projection->forward(20.0, centre + limit).xM);
      const ts::MapRect bounds = projection->bounds();
      for (const double lat : {-90.0, -60.0, 0.0, 45.0, 90.0}) {
        for (const double lon : {-limit, -limit / 2.0, 0.0, limit}) {
          const ts::MapPoint p = projection->forward(lat, centre + lon);
          QVERIFY(p.xM >= bounds.westM - 1e-6 && p.xM <= bounds.eastM + 1e-6);
          QVERIFY(p.yM >= bounds.southM - 1e-6 && p.yM <= bounds.northM + 1e-6);
        }
      }
      // The corner of the box is beyond the domain's edge at the equator.
      QVERIFY(!projection->inverse(ts::MapPoint{bounds.eastM, bounds.northM}));
      QVERIFY(!projection->inverse(ts::MapPoint{bounds.eastM + 1e5, 0.0}));
    }
    QCOMPARE(limited, 4);
  }

  void centralMeridian()
  {
    const ts::MercatorProjection pacific(180.0);
    QCOMPARE(pacific.centralMeridianDeg(), -180.0);
    QCOMPARE(pacific.forward(10.0, 180.0).xM, 0.0);
    QVERIFY(pacific.forward(10.0, -170.0).xM > 0.0);
    QVERIFY(pacific.forward(10.0, 170.0).xM < 0.0);
    const ts::MillerProjection shifted(30.0);
    const ts::MillerProjection plain;
    QVERIFY(std::abs(shifted.forward(40.0, 50.0).xM -
                     plain.forward(40.0, 20.0).xM) < 1e-6);
    QCOMPARE(shifted.forward(40.0, 50.0).yM, plain.forward(40.0, 20.0).yM);
    const ts::Geodetic back =
        shifted.inverse(shifted.forward(40.0, -170.0)).value_or(nowhere);
    QVERIFY(std::abs(back.lonDeg + 170.0) < 1e-9);
    // The same projection in PROJ.
    const ProjPipeline proj(shifted.projString());
    QVERIFY(proj.isValid());
    const ts::MapPoint ours = shifted.forward(-33.0, 151.0);
    const ts::MapPoint theirs = proj.forward(-33.0, 151.0);
    QVERIFY(std::abs(ours.xM - theirs.xM) < toleranceM);
    QVERIFY(std::abs(ours.yM - theirs.yM) < toleranceM);
  }

  void scaleFactors()
  {
    // scaleAt() against map distance over ground distance for short steps
    // east and north.
    for (const ts::Projection* projection : ts::projections()) {
      for (const double lat : {-80.0, -45.0, 0.0, 20.0, 60.0, 84.0}) {
        if (!projection->contains(lat, 10.0)) {
          continue;
        }
        const double step = 1e-3;
        const ts::MapPoint west = projection->forward(lat, 10.0 - step);
        const ts::MapPoint east = projection->forward(lat, 10.0 + step);
        const ts::MapPoint north = projection->forward(lat + step, 10.0);
        const ts::MapPoint south = projection->forward(lat - step, 10.0);
        // Lengths on the map (on a map that isn't cylindrical a step east
        // moves north too).
        // Central differences: the scale changes along the step.
        const double parallel =
            std::hypot(east.xM - west.xM, east.yM - west.yM) /
            groundDistanceM({lat, 10.0 - step, 0.0}, {lat, 10.0 + step, 0.0});
        const double meridian =
            std::hypot(north.xM - south.xM, north.yM - south.yM) /
            groundDistanceM({lat - step, 10.0, 0.0}, {lat + step, 10.0, 0.0});
        const ts::MapScale scale = projection->scaleAtPoint(lat, 10.0);
        QVERIFY2(std::abs(scale.parallel / parallel - 1.0) < 1e-6,
                 qPrintable(projection->id()));
        QVERIFY2(std::abs(scale.meridian / meridian - 1.0) < 1e-6,
                 qPrintable(projection->id()));
      }
    }
    // Mercator is conformal: the same scale both ways. Web Mercator is
    // not quite (up to 0.7 %), Miller not at all away from the equator.
    const ts::MercatorProjection mercator;
    const ts::WebMercatorProjection web;
    const ts::MillerProjection miller;
    for (const double lat : {0.0, 30.0, 60.0, 80.0}) {
      const ts::MapScale m = mercator.scaleAt(lat);
      QVERIFY(std::abs(m.parallel / m.meridian - 1.0) < 1e-12);
      const ts::MapScale w = web.scaleAt(lat);
      QVERIFY(std::abs(w.parallel / w.meridian - 1.0) < 0.007);
    }
    QVERIFY(std::abs(web.scaleAt(0.0).meridian / web.scaleAt(0.0).parallel -
                     1.0) > 0.006);
    QVERIFY(miller.scaleAt(60.0).parallel >
            1.2 * miller.scaleAt(60.0).meridian);
    QVERIFY(std::abs(mercator.scaleAt(0.0).parallel - 1.0) < 1e-12);
    QVERIFY(std::abs(mercator.scaleAt(60.0).parallel - 2.0) < 0.01);
  }

  void cylindricalHelpers()
  {
    const ts::MercatorProjection mercator;
    QVERIFY(std::abs(ts::CylindricalProjection::worldWidthM() -
                     40075016.685578) < 1e-5);
    QCOMPARE(mercator.northingM(45.0), mercator.forward(45.0, 99.0).yM);
    QVERIFY(std::abs(mercator.latitudeDeg(mercator.northingM(45.0)) - 45.0) <
            1e-12);
    // Off the map the latitude carries on past the limit.
    QVERIFY(mercator.latitudeDeg(mercator.bounds().northM * 1.1) >
            mercator.maximumLatDeg());
    QCOMPARE(mercator.constants().latitudeFactor, 1.0);
    QVERIFY(mercator.parameters().empty());
  }

  void glslSources()
  {
    const QByteArray glsl = ts::projectionGlsl();
    QVERIFY(glsl.contains("vec2 tsProject(int index, vec2 dLonLatRad"));
    for (const ts::Projection* projection : ts::projections()) {
      const QByteArray declaration = "vec2 " + projection->glslFunction() + "(";
      QVERIFY(glsl.contains(declaration));
      QVERIFY(projection->glslSource().contains(projection->glslFunction()));
    }
    // The reference of a latitude outside the domain is that of the edge.
    const ts::MercatorProjection mercator;
    const ts::ProjectionShaderReference edge =
        mercator.shaderReference(mercator.maximumLatDeg(), 0.0);
    const ts::ProjectionShaderReference beyond =
        mercator.shaderReference(89.0, 0.0);
    QCOMPARE(beyond.r0, edge.r0);
    QCOMPARE(beyond.r1, edge.r1);
    QCOMPARE(edge.r0[3], 0.0F);
  }

  void glslMatchesCpp()
  {
    // The shader's projection, read back from the GPU, against the C++
    // one: offsets from a reference point from a millionth of a degree to
    // most of the globe, for every projection and reference latitude.
    ts::test::OffscreenGl gl;
    TS_TEST_REQUIRE_GL(gl);
    ts::RenderDevice& device = gl.device();
    constexpr int32_t samples = 256;
    ts::ShaderDesc shaderDesc;
    shaderDesc.label = QStringLiteral("projection probe");
    shaderDesc.vertexSource = probeVertexGlsl;
    shaderDesc.fragmentSource = ts::projectionGlsl() + probeFragmentGlsl;
    shaderDesc.uniformBlocks = {{QByteArrayLiteral("Probe"), 0}};
    QString error;
    const std::unique_ptr<ts::ShaderProgram> shader =
        device.createShaderProgram(shaderDesc, &error);
    QVERIFY2(shader != nullptr, qPrintable(error));
    ts::PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader.get();
    const std::unique_ptr<ts::Pipeline> pipeline =
        device.createPipeline(pipelineDesc);
    const std::unique_ptr<ts::RenderTarget> target =
        device.createRenderTarget(samples, 1);
    const std::unique_ptr<ts::GpuBuffer> uniforms =
        device.createBuffer(ts::BufferType::Uniform, ts::BufferUsage::Dynamic,
                            sizeof(ProbeBlock), nullptr);
    QVERIFY(pipeline && target && uniforms);

    const std::vector<double> referenceLats = {0.0,  0.001, 30.0,  45.0, -60.0,
                                               80.0, 84.9,  -84.0, 89.0};
    // Total span of the samples in latitude, degrees: tiles of level 0
    // down to level 24 or so.
    const std::vector<double> spans = {170.0, 45.0, 5.0, 0.5, 1e-2, 1e-4, 1e-6};
    double worstRelative = 0.0;
    int32_t compared = 0;
    const std::vector<const ts::Projection*>& all = ts::projections();
    for (std::size_t p = 0; p < all.size(); ++p) {
      const ts::Projection& projection = *all[p];
      for (const double referenceLat : referenceLats) {
        const double clampedReference =
            std::clamp(referenceLat, projection.minimumLatDeg(),
                       projection.maximumLatDeg());
        const ts::MapPoint reference =
            projection.forward(clampedReference, 0.0);
        for (const double span : spans) {
          ProbeBlock block;
          // As the renderers ask: the samples reach half the span.
          const ts::ProjectionShaderReference shaderReference =
              projection.shaderReferenceFor(referenceLat, 0.0,
                                            0.5 * ts::degToRad(span));
          block.r0 = shaderReference.r0;
          block.r1 = shaderReference.r1;
          block.r2 = shaderReference.r2;
          block.r3 = shaderReference.r3;
          const float stepLat =
              static_cast<float>(ts::degToRad(span) / samples);
          const float stepLon = 0.5F * stepLat;
          const float first = -0.5F * samples;
          block.which = {static_cast<float>(p), 0.0F, 0.0F, 0.0F};
          block.steps = {stepLon, stepLat, first, 0.0F};
          const std::vector<std::array<float, 2>> values =
              runProbe(device, *pipeline, *target, *uniforms, block, samples);
          QCOMPARE(device.takeErrors(), QString());
          for (int32_t i = 0; i < samples; ++i) {
            const std::array<float, 2> gpu =
                values.at(static_cast<std::size_t>(i));
            // The same offsets the shader used, as the floats it saw.
            const float index = static_cast<float>(i) + first;
            const double dLonRad = static_cast<double>(stepLon * index);
            const double dLatRad = static_cast<double>(stepLat * index);
            ++compared;
            if (!drawnAt(projection, clampedReference + ts::radToDeg(dLatRad),
                         ts::radToDeg(dLonRad))) {
              continue; // discarded on screen
            }
            const ProbeExpectation expected =
                expectedProbeOffset(projection, clampedReference, reference,
                                    dLonRad, dLatRad, span);
            const double expectedX = expected.xM;
            const double expectedY = expected.yM;
            const double allowedX = expected.allowedXM;
            const double allowedY = expected.allowedYM;
            const double errorX =
                std::abs(static_cast<double>(gpu[0]) - expectedX);
            const double errorY =
                std::abs(static_cast<double>(gpu[1]) - expectedY);
            QVERIFY(std::isfinite(gpu[0]) && std::isfinite(gpu[1]));
            worstRelative =
                std::max({worstRelative, errorX / allowedX, errorY / allowedY});
            if (errorX > allowedX || errorY > allowedY) {
              QFAIL(qPrintable(
                  QStringLiteral("%1 at %2° + %3°: GPU (%4, %5), C++ (%6, %7)")
                      .arg(projection.id())
                      .arg(referenceLat)
                      .arg(ts::radToDeg(dLatRad), 0, 'g', 9)
                      .arg(static_cast<double>(gpu[0]), 0, 'g', 12)
                      .arg(static_cast<double>(gpu[1]), 0, 'g', 12)
                      .arg(expectedX, 0, 'g', 12)
                      .arg(expectedY, 0, 'g', 12)));
            }
          }
        }
      }
    }
    QCOMPARE(compared, static_cast<int32_t>(all.size()) * 9 * 7 * samples);
    qInfo("GLSL against C++: worst error %.2f of the allowance", worstRelative);
  }

  void nationalGridHelmert()
  {
    // Without OSTN15: the Ordnance Survey's test points within 5 m (the
    // Helmert transform's accuracy in Great Britain).
    const ts::Projection& grid =
        *ts::projectionById(QStringLiteral("uk-national-grid"));
    double worstM = 0.0;
    for (const ts::test::Ostn15TestPoint& point :
         ts::test::ostn15TestPoints()) {
      const ts::MapPoint ours = grid.forward(point.latDeg, point.lonDeg);
      worstM = std::max(worstM, std::hypot(ours.xM - point.eastingM,
                                           ours.yM - point.northingM));
    }
    QVERIFY2(worstM < 5.0, qPrintable(QString::number(worstM)));
    QVERIFY(worstM > 0.5); // it is the Helmert, not the grid
    qInfo("Helmert: worst %.2f m from the OSTN15 test points", worstM);
  }

  void nationalGridOstn15()
  {
    // With the real OSTN15 grid, where there is one (TS_OSTN15_GRID, or
    // PROJ's data): the test points within a centimetre (the grid's NTv2
    // form; PROJ is as far out), and PROJ's own result to a millimetre.
    const QString path = ts::test::ostn15GridPath();
    if (path.isEmpty()) {
      QSKIP("No OSTN15 grid: set TS_OSTN15_GRID to "
            "uk_os_OSTN15_NTv2_OSGBtoETRS.tif.");
    }
    QString error;
    const std::shared_ptr<const ts::HorizontalShiftGrid> ostn15 =
        ts::HorizontalShiftGrid::load(path, &error);
    QVERIFY2(ostn15 != nullptr, qPrintable(error));
    const ts::NationalGridProjection grid(ostn15);
    const ProjPipeline proj(grid.projString());
    QVERIFY2(proj.isValid(), qPrintable(grid.projString()));
    double worstOsM = 0.0;
    double worstProjM = 0.0;
    double worstBackDeg = 0.0;
    for (const ts::test::Ostn15TestPoint& point :
         ts::test::ostn15TestPoints()) {
      const ts::MapPoint ours = grid.forward(point.latDeg, point.lonDeg);
      const ts::MapPoint theirs = proj.forward(point.latDeg, point.lonDeg);
      worstOsM = std::max(worstOsM, std::hypot(ours.xM - point.eastingM,
                                               ours.yM - point.northingM));
      worstProjM = std::max(
          worstProjM, std::hypot(ours.xM - theirs.xM, ours.yM - theirs.yM));
      const ts::Geodetic back = grid.inverse(ours).value_or(nowhere);
      worstBackDeg =
          std::max({worstBackDeg, std::abs(back.latDeg - point.latDeg),
                    std::abs(back.lonDeg - point.lonDeg)});
    }
    QVERIFY2(worstOsM < 0.01, qPrintable(QString::number(worstOsM)));
    QVERIFY2(worstProjM < toleranceM, qPrintable(QString::number(worstProjM)));
    QVERIFY2(worstBackDeg < toleranceDeg,
             qPrintable(QString::number(worstBackDeg)));
    qInfo("OSTN15: worst %.4f m from the test points, %.6f m from PROJ",
          worstOsM, worstProjM);
  }

  void shiftGrid()
  {
    // A synthetic grid: offsets at the nodes, bilinear between them, the
    // NTv2 sign convention undone, nothing outside.
    const QTemporaryDir dir;
    const QString east = dir.filePath(QStringLiteral("east.tif"));
    const QString west = dir.filePath(QStringLiteral("west.tif"));
    QVERIFY(ts::test::writeFixtureShiftGrid(east, 0.25, false));
    QVERIFY(ts::test::writeFixtureShiftGrid(west, 0.25, true));
    QString error;
    const std::shared_ptr<const ts::HorizontalShiftGrid> grid =
        ts::HorizontalShiftGrid::load(east, &error);
    QVERIFY2(grid != nullptr, qPrintable(error));
    const std::shared_ptr<const ts::HorizontalShiftGrid> westGrid =
        ts::HorizontalShiftGrid::load(west, &error);
    QVERIFY2(westGrid != nullptr, qPrintable(error));
    const double second = ts::degToRad(1.0 / 3600.0);
    for (const std::array<double, 2>& node :
         {std::array<double, 2>{49.0, -9.0}, std::array<double, 2>{61.0, 2.0},
          std::array<double, 2>{52.5, -1.75}}) {
      const std::optional<std::array<double, 2>> offset =
          grid->offsetRad(ts::degToRad(node[0]), ts::degToRad(node[1]));
      QVERIFY(offset.has_value());
      const std::array<double, 2> at = offset.value_or(noOffset);
      QVERIFY(std::abs(at[0] / second -
                       ts::test::fixtureLatOffsetSec(node[0], node[1])) < 1e-5);
      QVERIFY(std::abs(at[1] / second -
                       ts::test::fixtureLonOffsetSec(node[0], node[1])) < 1e-5);
      QCOMPARE(
          westGrid->offsetRad(ts::degToRad(node[0]), ts::degToRad(node[1])),
          offset);
    }
    // Halfway between four nodes: their mean.
    const std::optional<std::array<double, 2>> middle =
        grid->offsetRad(ts::degToRad(52.625), ts::degToRad(-1.875));
    QVERIFY(middle.has_value());
    double mean = 0.0;
    for (const double lat : {52.5, 52.75}) {
      for (const double lon : {-2.0, -1.75}) {
        mean += 0.25 * ts::test::fixtureLatOffsetSec(lat, lon);
      }
    }
    QVERIFY(std::abs(middle.value_or(noOffset)[0] / second - mean) < 1e-5);
    QVERIFY(!grid->offsetRad(ts::degToRad(48.99), 0.0).has_value());
    QVERIFY(
        !grid->offsetRad(ts::degToRad(50.0), ts::degToRad(2.01)).has_value());
    QCOMPARE(grid->insideDeg(50.0, 1.5), 0.5);
    QVERIFY(grid->insideDeg(48.0, 0.0) < 0.0);
    // Outside, the nearest edge's offsets.
    QCOMPARE(grid->clampedOffsetRad(ts::degToRad(70.0), ts::degToRad(1.0)),
             grid->offsetRad(ts::degToRad(61.0), ts::degToRad(1.0))
                 .value_or(noOffset));
    // Not a horizontal offset grid.
    const QString other = dir.filePath(QStringLiteral("other.tif"));
    QFile text(other);
    QVERIFY(text.open(QIODevice::WriteOnly));
    text.write("not a grid");
    text.close();
    QVERIFY(ts::HorizontalShiftGrid::load(other, &error) == nullptr);
    QVERIFY(!error.isEmpty());
  }

  void osgb36Datum()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("grid.tif"));
    QVERIFY(ts::test::writeFixtureShiftGrid(path));
    QString error;
    const ts::Osgb36Datum datum(ts::HorizontalShiftGrid::load(path, &error));
    QVERIFY2(datum.hasGrid(), qPrintable(error));
    QCOMPARE(datum.gridPath(), path);
    const ts::Osgb36Datum helmert(nullptr);
    QVERIFY(!helmert.hasGrid());
    const std::shared_ptr<const ts::HorizontalShiftGrid> loaded =
        ts::HorizontalShiftGrid::load(path, &error);
    QVERIFY(loaded != nullptr);
    const ts::HorizontalShiftGrid& grid = *loaded;

    // Well inside: OSGB36 plus the offset at the OSGB36 point is WGS 84.
    const double lat = ts::degToRad(53.2);
    const double lon = ts::degToRad(-3.1);
    const std::array<double, 2> osgb = datum.fromWgs84(lat, lon);
    const std::array<double, 2> offset =
        grid.offsetRad(osgb[0], osgb[1]).value_or(noOffset);
    QVERIFY(std::abs(osgb[0] + offset[0] - lat) < 1e-14);
    QVERIFY(std::abs(osgb[1] + offset[1] - lon) < 1e-14);
    const std::array<double, 2> back = datum.toWgs84(osgb[0], osgb[1]);
    QVERIFY(std::abs(back[0] - lat) < 1e-13);
    QVERIFY(std::abs(back[1] - lon) < 1e-13);

    // Outside: the Helmert transform; across the blended edge, no jump.
    const std::array<double, 2> outside =
        datum.fromWgs84(ts::degToRad(45.0), ts::degToRad(-3.0));
    QCOMPARE(outside, ts::Osgb36Datum::helmertFromWgs84(ts::degToRad(45.0),
                                                        ts::degToRad(-3.0)));
    QCOMPARE(helmert.fromWgs84(lat, lon),
             ts::Osgb36Datum::helmertFromWgs84(lat, lon));
    const double step = ts::degToRad(1e-7);
    for (int32_t i = 0; i < 25; ++i) {
      const double a = ts::degToRad(48.95 + 0.01 * i);
      const std::array<double, 2> below = datum.fromWgs84(a, lon);
      const std::array<double, 2> above = datum.fromWgs84(a + step, lon);
      // No jump: the change is about the step itself (the fixture is
      // seconds of arc from the Helmert, blended over 0.1°).
      QVERIFY(std::abs((above[0] - below[0]) - step) < 0.1 * step);
      QVERIFY(std::abs(above[1] - below[1]) < 0.1 * step);
    }
  }

  void gridReferences()
  {
    // Formatting, as the Ordnance Survey does (truncated, not rounded).
    QCOMPARE(ts::formatGridReference(0.0, 0.0),
             QStringLiteral("SV 00000 00000"));
    QCOMPARE(ts::formatGridReference(438710.908, 114792.248),
             QStringLiteral("SU 38710 14792"));
    QCOMPARE(ts::formatGridReference(438710.908, 114792.248, 6),
             QStringLiteral("SU 387 147"));
    QCOMPARE(ts::formatGridReference(438710.908, 114792.248, 2),
             QStringLiteral("SU 3 1"));
    QCOMPARE(ts::formatGridReference(438710.908, 114792.248, 0),
             QStringLiteral("SU"));
    QCOMPARE(ts::formatGridReference(132508.0, 830205.0),
             QStringLiteral("NG 32508 30205"));
    QCOMPARE(ts::formatGridReference(500025.0, 500025.0, 4),
             QStringLiteral("OV 00 00"));
    QCOMPARE(ts::formatGridReference(530080.4, 180729.9),
             QStringLiteral("TQ 30080 80729"));
    QCOMPARE(ts::formatGridReference(699999.0, 1299999.0),
             QStringLiteral("JM 99999 99999"));
    QCOMPARE(ts::formatGridReference(460000.0, 1210000.0, 0),
             QStringLiteral("HP"));
    QVERIFY(ts::formatGridReference(-1.0, 10.0).isEmpty());
    QVERIFY(ts::formatGridReference(700000.0, 10.0).isEmpty());
    QVERIFY(ts::formatGridReference(10.0, 1300000.0).isEmpty());
    QVERIFY(ts::formatGridReference(10.0, 10.0, 3).isEmpty());

    // Parsing: the south-west corner of the square named.
    const std::optional<ts::MapPoint> tq =
        ts::parseGridReference(QStringLiteral("TQ 30080 80729"));
    QVERIFY(tq.has_value());
    QCOMPARE(tq.value_or(noPoint).xM, 530080.0);
    QCOMPARE(tq.value_or(noPoint).yM, 180729.0);
    const std::optional<ts::MapPoint> packed =
        ts::parseGridReference(QStringLiteral(" tq3008080729 "));
    QVERIFY(packed.has_value());
    QCOMPARE(packed.value_or(noPoint).xM, 530080.0);
    QCOMPARE(packed.value_or(noPoint).yM, 180729.0);
    const std::optional<ts::MapPoint> su =
        ts::parseGridReference(QStringLiteral("SU 387 147"));
    QVERIFY(su.has_value());
    QCOMPARE(su.value_or(noPoint).xM, 438700.0);
    QCOMPARE(su.value_or(noPoint).yM, 114700.0);
    const std::optional<ts::MapPoint> nn =
        ts::parseGridReference(QStringLiteral("NN"));
    QVERIFY(nn.has_value());
    QCOMPARE(nn.value_or(noPoint).xM, 200000.0);
    QCOMPARE(nn.value_or(noPoint).yM, 700000.0);
    for (const char* bad : {"", "TQ 123 45", "TQ 12345", "IQ 1 1", "ZZ", "AA",
                            "TQ 123456 123456", "T 1 1", "TQ 1a 1"}) {
      QVERIFY2(!ts::parseGridReference(QString::fromLatin1(bad)).has_value(),
               bad);
    }
    // Every test point, there and back.
    for (const ts::test::Ostn15TestPoint& point :
         ts::test::ostn15TestPoints()) {
      const QString reference =
          ts::formatGridReference(point.eastingM, point.northingM);
      const std::optional<ts::MapPoint> parsed =
          ts::parseGridReference(reference);
      QVERIFY2(parsed.has_value(), qPrintable(reference));
      QCOMPARE(parsed.value_or(noPoint).xM, std::floor(point.eastingM));
      QCOMPARE(parsed.value_or(noPoint).yM, std::floor(point.northingM));
    }
  }

  void gridReadout()
  {
    // The National Grid gives its grid reference; projections without a
    // grid of their own, and points outside the lettered squares, none.
    const ts::Projection& grid =
        *ts::projectionById(QStringLiteral("uk-national-grid"));
    const ts::MapPoint london = grid.forward(51.5007, -0.1246);
    QCOMPARE(grid.gridReference(51.5007, -0.1246),
             ts::formatGridReference(london.xM, london.yM));
    QVERIFY(grid.gridReference(51.5007, -0.1246)
                .startsWith(QLatin1String("TQ 30")));
    QVERIFY(grid.gridReference(10.0, -2.0).isEmpty());
    QVERIFY(grid.gridReference(51.5, 90.0).isEmpty()); // outside the domain
    QVERIFY(ts::projections().front()->gridReference(51.5, -0.12).isEmpty());
    QCOMPARE(grid.datumShiftGridId(), QStringLiteral("ostn15"));
    QVERIFY(ts::projections().front()->datumShiftGridId().isEmpty());
  }

  void projectionWithDatumGrid()
  {
    // An installed grid: projectionWith() gives an instance that uses it,
    // the same one each time; without, the standard instance again.
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ostn15.tif"));
    QVERIFY(ts::test::writeFixtureShiftGrid(path));
    const QString id = QStringLiteral("uk-national-grid");
    const ts::Projection* standard = ts::projectionById(id);
    QCOMPARE(ts::projectionWith(id, {}), standard);
    ts::setDatumShiftGrids({{QStringLiteral("ostn15"), path}});
    const ts::Projection* withGrid = ts::projectionWith(id, {});
    QVERIFY(withGrid != standard);
    QCOMPARE(ts::projectionWith(id, {}), withGrid);
    QCOMPARE(ts::projectionIndex(withGrid), ts::projectionIndex(standard));
    QVERIFY(withGrid->projString().contains(QLatin1String("hgridshift")));
    QVERIFY(withGrid->projString().contains(path));
    // The fixture's offsets differ from the real datum shift by tens of
    // metres: a different map position.
    const ts::MapPoint a = standard->forward(53.0, -2.0);
    const ts::MapPoint b = withGrid->forward(53.0, -2.0);
    QVERIFY(std::hypot(a.xM - b.xM, a.yM - b.yM) > 1.0);
    // Other projections are unaffected.
    QCOMPARE(ts::projectionWith(QStringLiteral("mercator"), {}),
             ts::projections().front());
    ts::setDatumShiftGrids({});
    QCOMPARE(ts::projectionWith(id, {}), standard);
    // A grid that can't be read is left out.
    ts::setDatumShiftGrids({{QStringLiteral("ostn15"),
                             dir.filePath(QStringLiteral("missing.tif"))}});
    QVERIFY(ts::datumShiftGrid(QStringLiteral("ostn15")) == nullptr);
    QCOMPARE(ts::projectionWith(id, {}), standard);
  }

  void rotatedMercatorCentreLine()
  {
    // The centre maps to x = y = 0; the centre line (rotated latitude 0)
    // is the straight line through it at the azimuth, and the scale along
    // it is the scale factor: exactly at the centre, and within 2 parts in
    // 10⁴ for 3000 km either way (Hotine's ellipsoidal form is true to
    // scale on its aposphere, not quite on the ellipsoid).
    const std::vector<std::array<double, 4>> sets = {{45.0, 0.0, 45.0, 1.0},
                                                     {52.0, -1.0, 80.0, 0.9996},
                                                     {-33.0, 151.0, -30.0, 1.0},
                                                     {10.0, 100.0, 0.0, 1.0},
                                                     {60.0, -40.0, 90.0, 1.0}};
    double worstScale = 0.0;
    for (const std::array<double, 4>& set : sets) {
      const ts::RotatedMercatorProjection projection(set[0], set[1], set[2],
                                                     set[3]);
      const ts::MapPoint centre = projection.forward(set[0], set[1]);
      QVERIFY(std::hypot(centre.xM, centre.yM) < 1e-6);
      const std::array<double, 2> rotated =
          projection.rotatedDeg(set[0], set[1]);
      QVERIFY(std::abs(rotated[0]) < 1e-9 && std::abs(rotated[1]) < 1e-9);
      QVERIFY(projection.contains(set[0], set[1]));
      const double sinA = std::sin(ts::degToRad(set[2]));
      const double cosA = std::cos(ts::degToRad(set[2]));
      for (int32_t step = -6; step <= 6; ++step) {
        const double t = 5.0e5 * step;
        const ts::Geodetic a =
            projection.inverse(ts::MapPoint{t * sinA, t * cosA})
                .value_or(nowhere);
        const ts::Geodetic b =
            projection
                .inverse(ts::MapPoint{(t + 100.0) * sinA, (t + 100.0) * cosA})
                .value_or(nowhere);
        // On the centre line,
        QVERIFY2(std::abs(projection.rotatedDeg(a.latDeg, a.lonDeg)[0]) < 1e-9,
                 qPrintable(QString::number(t)));
        // and 100 m of it on the map is 100 m / k0 on the ground.
        const double groundM = groundDistanceM(a, b);
        const double scaleError = std::abs(100.0 / groundM / set[3] - 1.0);
        if (step == 0) {
          QVERIFY2(scaleError < 1e-9, qPrintable(QString::number(scaleError)));
        }
        worstScale = std::max(worstScale, scaleError);
      }
    }
    QVERIFY2(worstScale < 2e-4, qPrintable(QString::number(worstScale)));
  }

  void rotatedMercatorDomain()
  {
    // A domain of its own: the centre is deep inside; near the rotated
    // poles and far along the line is outside, has no inverse and clamps
    // onto the edge; bounds() holds the domain.
    const ts::RotatedMercatorProjection projection(52.0, -1.0, 80.0);
    QVERIFY(projection.hasOwnDomain());
    QVERIFY(!ts::projections().front()->hasOwnDomain());
    QVERIFY(std::isinf(ts::projections().front()->domainMarginDeg(10.0, 10.0)));
    QVERIFY(projection.domainMarginDeg(52.0, -1.0) > 50.0);
    const ts::MapRect bounds = projection.bounds();
    // 88° across the line from the centre, and 160° along it.
    const ts::GeodesicDirect across =
        ts::geodesicDirect(52.0, -1.0, 80.0 - 90.0, 88.0 * 111195.0);
    const ts::GeodesicDirect along =
        ts::geodesicDirect(52.0, -1.0, 80.0, 160.0 * 111195.0);
    for (const ts::GeodesicDirect& point : {across, along}) {
      QVERIFY(!projection.contains(point.latDeg, point.lonDeg));
      QVERIFY(projection.domainMarginDeg(point.latDeg, point.lonDeg) < -1.0);
      const ts::Geodetic edge =
          projection.clampToDomain(point.latDeg, point.lonDeg);
      QVERIFY2(std::abs(projection.domainMarginDeg(edge.latDeg, edge.lonDeg)) <
                   1e-6,
               qPrintable(QString::number(
                   projection.domainMarginDeg(edge.latDeg, edge.lonDeg))));
      const ts::MapPoint p = projection.forward(edge.latDeg, edge.lonDeg);
      QVERIFY(p.xM >= bounds.westM - 1e-3 && p.xM <= bounds.eastM + 1e-3);
      QVERIFY(p.yM >= bounds.southM - 1e-3 && p.yM <= bounds.northM + 1e-3);
      // Beyond the edge on the map: no point.
      QVERIFY(
          !projection.inverse(projection.forward(point.latDeg, point.lonDeg)));
    }
    // Inside: clampToDomain() leaves the point alone.
    const ts::Geodetic inside = projection.clampToDomain(40.0, -20.0);
    QCOMPARE(inside.latDeg, 40.0);
    QCOMPARE(inside.lonDeg, -20.0);
    // Off the domain's rectangle on the map, no point.
    QVERIFY(!projection.inverse(ts::MapPoint{bounds.eastM * 2.0, 0.0}));
    // Every point inside maps into bounds().
    for (int32_t i = -35; i <= 35; ++i) {
      for (int32_t j = -71; j < 72; ++j) {
        const double lat = 2.5 * i;
        const double lon = 2.5 * j;
        if (!projection.contains(lat, lon)) {
          continue;
        }
        const ts::MapPoint p = projection.forward(lat, lon);
        QVERIFY(p.xM >= bounds.westM - 1e-3 && p.xM <= bounds.eastM + 1e-3 &&
                p.yM >= bounds.southM - 1e-3 && p.yM <= bounds.northM + 1e-3);
      }
    }
    // The GLSL test exists for it alone.
    QVERIFY(
        ts::projectionDomainGlsl().contains("tsProjectRotatedMercatorOutside"));
    QVERIFY(!ts::projectionDomainGlsl().contains("tsProjectMercatorOutside"));
  }

  void rotatedMercatorThroughPoints()
  {
    // The centre line through two places: halfway along the geodesic, at
    // its azimuth there; both places lie on Hotine's centre line to within
    // a few hundredths of a degree (it isn't quite the geodesic).
    const std::optional<ts::ProjectionParameterValues> values =
        ts::RotatedMercatorProjection::throughPoints(51.4775, -0.4614, 40.6413,
                                                     -73.7781);
    QVERIFY(values.has_value());
    const ts::ProjectionParameterValues v =
        values.value_or(ts::ProjectionParameterValues());
    const double azimuth = v.at(QStringLiteral("azimuth"));
    QVERIFY(azimuth >= -90.0 && azimuth <= 90.0);
    QCOMPARE(v.at(QStringLiteral("scaleFactor")), 1.0);
    const ts::Projection* projection =
        ts::projectionWith(QStringLiteral("rotated-mercator"), v);
    QVERIFY(projection != nullptr);
    const ts::RotatedMercatorProjection line(
        v.at(QStringLiteral("centreLatitude")),
        v.at(QStringLiteral("centreLongitude")), azimuth, 1.0);
    for (const std::array<double, 2>& place :
         {std::array<double, 2>{51.4775, -0.4614},
          std::array<double, 2>{40.6413, -73.7781}}) {
      const double across = line.rotatedDeg(place[0], place[1])[0];
      QVERIFY2(std::abs(across) < 0.05, qPrintable(QString::number(across)));
    }
    // No line through one place twice, or through antipodes.
    QVERIFY(
        !ts::RotatedMercatorProjection::throughPoints(10.0, 20.0, 10.0, 20.0));
    QVERIFY(!ts::RotatedMercatorProjection::throughPoints(10.0, 20.0, -10.0,
                                                          -160.0));
  }

  void glslDomainMatchesCpp()
  {
    // tsOutsideDomain() in the shader against contains() on a grid over
    // the globe, away from the edge (within a tenth of a degree float may
    // decide either way).
    ts::test::OffscreenGl gl;
    TS_TEST_REQUIRE_GL(gl);
    ts::RenderDevice& device = gl.device();
    constexpr int32_t columns = 256;
    ts::ShaderDesc shaderDesc;
    shaderDesc.label = QStringLiteral("domain probe");
    shaderDesc.vertexSource = probeVertexGlsl;
    shaderDesc.fragmentSource = ts::projectionDomainGlsl() + R"(
layout(std140) uniform Domain {
  vec4 d0;
  vec4 d1;
  vec4 which; // projection index, latitude (degrees)
};
out vec4 fragColor;
void main()
{
  vec2 lonLat = vec2(-180.0 + gl_FragCoord.x * (360.0 / 256.0), which.y);
  fragColor = tsOutsideDomain(int(which.x), lonLat, d0, d1) ? vec4(1.0)
                                                            : vec4(0.0);
}
)";
    shaderDesc.uniformBlocks = {{QByteArrayLiteral("Domain"), 0}};
    QString error;
    const std::unique_ptr<ts::ShaderProgram> shader =
        device.createShaderProgram(shaderDesc, &error);
    QVERIFY2(shader != nullptr, qPrintable(error));
    ts::PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader.get();
    const std::unique_ptr<ts::Pipeline> pipeline =
        device.createPipeline(pipelineDesc);
    const std::unique_ptr<ts::RenderTarget> target =
        device.createRenderTarget(columns, 1);
    struct DomainBlock {
      std::array<float, 8> domain{};
      std::array<float, 4> which{};
    };
    const std::unique_ptr<ts::GpuBuffer> uniforms =
        device.createBuffer(ts::BufferType::Uniform, ts::BufferUsage::Dynamic,
                            sizeof(DomainBlock), nullptr);
    QVERIFY(pipeline && target && uniforms);
    int32_t checked = 0;
    for (const std::array<double, 3>& set :
         {std::array<double, 3>{45.0, 0.0, 45.0},
          std::array<double, 3>{-33.0, 151.0, -30.0},
          std::array<double, 3>{60.0, -40.0, 90.0}}) {
      const ts::Projection* projection =
          ts::projectionWith(QStringLiteral("rotated-mercator"),
                             {{QStringLiteral("centreLatitude"), set[0]},
                              {QStringLiteral("centreLongitude"), set[1]},
                              {QStringLiteral("azimuth"), set[2]}});
      QVERIFY(projection != nullptr);
      DomainBlock block;
      block.domain = projection->glslDomainParameters();
      for (int32_t row = 0; row < 90; ++row) {
        const double lat = -89.0 + 2.0 * row;
        block.which = {static_cast<float>(ts::projectionIndex(projection)),
                       static_cast<float>(lat), 0.0F, 0.0F};
        uniforms->update(0, &block, sizeof(block));
        ts::PassDesc pass;
        pass.target = target.get();
        pass.viewportPx = QRect(0, 0, columns, 1);
        device.beginPass(pass);
        device.setPipeline(*pipeline);
        device.setUniformBuffer(0, *uniforms, 0, sizeof(block));
        device.draw(3, 0, 1);
        device.endPass();
        const QImage image = device.readPixels(*target);
        for (int32_t i = 0; i < columns; ++i) {
          const double lon = -180.0 + (i + 0.5) * (360.0 / columns);
          if (std::abs(projection->domainMarginDeg(lat, lon)) < 0.1) {
            continue;
          }
          const bool outside = qRed(image.pixel(i, 0)) > 127;
          QVERIFY2(outside == !projection->contains(lat, lon),
                   qPrintable(QStringLiteral("%1, %2").arg(lat).arg(lon)));
          ++checked;
        }
      }
    }
    QCOMPARE(device.takeErrors(), QString());
    QVERIFY(checked > 3 * 90 * columns * 9 / 10);
  }

  void concurrentSharedInstances()
  {
    // Projections are immutable: many threads share the registered ones.
    const int32_t failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [](int32_t thread) {
          bool ok = true;
          for (int32_t i = 0; i < 2000; ++i) {
            for (const ts::Projection* projection : ts::projections()) {
              const double lat = -80.0 + (thread * 7 + i) % 160;
              const double lon = -179.0 + (thread * 13 + i * 3) % 358;
              if (!projection->contains(lat, lon)) {
                continue;
              }
              const std::optional<ts::Geodetic> back =
                  projection->inverse(projection->forward(lat, lon));
              ok = ok && back && std::abs(back->latDeg - lat) < 1e-9 &&
                   std::abs(back->lonDeg - lon) < 1e-9;
              ok = ok && !ts::projectionGlsl().isEmpty();
            }
          }
          return ok;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkForward()
  {
    const ts::MercatorProjection mercator;
    double sum = 0.0;
    QBENCHMARK
    {
      for (int32_t i = 0; i < 1000; ++i) {
        sum += mercator.forward(-80.0 + 0.16 * i, -170.0 + 0.34 * i).yM;
      }
    }
    QVERIFY(std::isfinite(sum));
  }

  void benchmarkInverse()
  {
    const ts::MercatorProjection mercator;
    double sum = 0.0;
    QBENCHMARK
    {
      for (int32_t i = 0; i < 1000; ++i) {
        sum += mercator.inverse(ts::MapPoint{1.0e4 * i, -1.5e7 + 3.0e4 * i})
                   .value_or(nowhere)
                   .latDeg;
      }
    }
    QVERIFY(std::isfinite(sum));
  }
};

QTEST_MAIN(TestProjection)
#include "tst_projection.moc"
