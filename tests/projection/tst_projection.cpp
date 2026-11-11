// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <QSet>
#include <QtTest>

#include <proj.h>

#include "concurrency.h"
#include "offscreengl.h"
#include "ts/deps/projcontext.h"
#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/projection/projection.h"

namespace
{

/** The Phase 4 plan's tolerance for projections: one millimetre. */
constexpr double toleranceM = 1e-3;

/** A millimetre on the ground as an angle, degrees (at the equator). */
constexpr double toleranceDeg = 1e-8;

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

  ts::Geodetic inverse(const ts::MapPoint& point) const
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

/** Latitudes of the global grid inside @p projection's domain. */
std::vector<double> gridLatitudes(const ts::Projection& projection)
{
  std::vector<double> lats = {projection.minimumLatDeg(),
                              projection.maximumLatDeg(), 1e-9, -1e-9};
  for (double lat = -90.0; lat <= 90.0; lat += 2.5) {
    if (projection.containsLatitude(lat)) {
      lats.push_back(lat);
    }
  }
  return lats;
}

/** Longitudes of the global grid, with the antimeridian's two sides. */
std::vector<double> gridLongitudes()
{
  std::vector<double> lons = {-180.0, -179.999999, 179.999999};
  for (double lon = -172.5; lon < 180.0; lon += 7.5) {
    lons.push_back(lon);
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
  vec4 steps;  // dLon per sample, dLat per sample, first sample, component
  vec4 which;  // projection index
};
out vec4 fragColor;
void main()
{
  float sampleIndex = floor(gl_FragCoord.x) + steps.z;
  vec2 d = vec2(steps.x, steps.y) * sampleIndex;
  vec2 result = tsProject(int(which.x), d, r0, r1);
  uint bits = floatBitsToUint(steps.w < 0.5 ? result.x : result.y);
  fragColor = vec4(float(bits & 255u), float((bits >> 8) & 255u),
                   float((bits >> 16) & 255u), float(bits >> 24)) / 255.0;
}
)";

struct ProbeBlock {
  std::array<float, 4> r0{};
  std::array<float, 4> r1{};
  std::array<float, 4> steps{};
  std::array<float, 4> which{};
};

} // namespace

/** Unit tests for ts/projection. */
class TestProjection : public QObject
{
  Q_OBJECT

private slots:
  void listOfProjections()
  {
    const std::vector<const ts::Projection*>& all = ts::projections();
    QCOMPARE(all.size(), std::size_t{3});
    QSet<QString> ids;
    QSet<QByteArray> functions;
    for (std::size_t i = 0; i < all.size(); ++i) {
      const ts::Projection* projection = all[i];
      QVERIFY(!projection->id().isEmpty());
      QVERIFY(!projection->displayName().isEmpty());
      ids.insert(projection->id());
      functions.insert(projection->glslFunction());
      QCOMPARE(ts::projectionById(projection->id()), projection);
      QCOMPARE(ts::projectionIndex(projection), static_cast<int>(i));
      QCOMPARE(projection->centralMeridianDeg(), 0.0);
    }
    QCOMPARE(ids.size(), 3);
    QCOMPARE(functions.size(), 3);
    QCOMPARE(all.front()->id(), QStringLiteral("mercator"));
    QVERIFY(ts::projectionById(QStringLiteral("web-mercator")) != nullptr);
    QVERIFY(ts::projectionById(QStringLiteral("miller")) != nullptr);
    QVERIFY(ts::projectionById(QStringLiteral("no-such")) == nullptr);
    QVERIFY(ts::projectionById(QString()) == nullptr);
    const ts::MercatorProjection other;
    QCOMPARE(ts::projectionIndex(&other), -1);
    QCOMPARE(ts::projectionIndex(nullptr), -1);
  }

  void againstProj_data()
  {
    QTest::addColumn<int>("index");
    QTest::newRow("mercator") << 0;
    QTest::newRow("web-mercator") << 1;
    QTest::newRow("miller") << 2;
  }

  void againstProj()
  {
    // Forward and inverse agree with PROJ to a millimetre over a global
    // grid, the limits of the domain and both sides of the antimeridian.
    QFETCH(int, index);
    const ts::Projection& projection =
        *ts::projections().at(static_cast<std::size_t>(index));
    const ProjPipeline proj(projection.projString());
    QVERIFY2(proj.isValid(), qPrintable(projection.projString()));
    double worstForwardM = 0.0;
    double worstInverseDeg = 0.0;
    for (const double lat : gridLatitudes(projection)) {
      for (const double lon : gridLongitudes()) {
        const ts::MapPoint ours = projection.forward(lat, lon);
        // PROJ puts −180° at +πa, we at −πa: the same meridian.
        ts::MapPoint theirs = proj.forward(lat, lon);
        if (lon == -180.0) {
          theirs.xM = -std::abs(theirs.xM);
        }
        worstForwardM = std::max({worstForwardM, std::abs(ours.xM - theirs.xM),
                                  std::abs(ours.yM - theirs.yM)});
        const std::optional<ts::Geodetic> back = projection.inverse(theirs);
        QVERIFY(back.has_value());
        const ts::Geodetic projBack = proj.inverse(theirs);
        worstInverseDeg = std::max(
            {worstInverseDeg, std::abs(back->latDeg - projBack.latDeg),
             std::abs(ts::normaliseLonDeg(back->lonDeg - projBack.lonDeg))});
      }
    }
    QVERIFY2(worstForwardM < toleranceM,
             qPrintable(QString::number(worstForwardM, 'g', 6)));
    QVERIFY2(worstInverseDeg < toleranceDeg,
             qPrintable(QString::number(worstInverseDeg, 'g', 6)));
  }

  void roundTrip_data() { againstProj_data(); }

  void roundTrip()
  {
    // forward then inverse comes back within a millimetre on the ground.
    QFETCH(int, index);
    const ts::Projection& projection =
        *ts::projections().at(static_cast<std::size_t>(index));
    double worstM = 0.0;
    for (const double lat : gridLatitudes(projection)) {
      for (const double lon : gridLongitudes()) {
        const std::optional<ts::Geodetic> back =
            projection.inverse(projection.forward(lat, lon));
        QVERIFY(back.has_value());
        QCOMPARE(back->heightM, 0.0);
        worstM = std::max(worstM,
                          groundDistanceM(ts::Geodetic{lat, lon, 0.0}, *back));
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
      QVERIFY(std::abs(bounds.widthM() -
                       2.0 * ts::pi * ts::wgs84::semiMajorAxisM) < 1e-6);
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
    const std::optional<ts::Geodetic> back =
        shifted.inverse(shifted.forward(40.0, -170.0));
    QVERIFY(back.has_value());
    QVERIFY(std::abs(back->lonDeg + 170.0) < 1e-9);
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
        const double step = 1e-3;
        const ts::MapPoint here = projection->forward(lat, 10.0);
        const ts::MapPoint east = projection->forward(lat, 10.0 + step);
        const ts::MapPoint north = projection->forward(lat + step, 10.0);
        const ts::MapPoint south = projection->forward(lat - step, 10.0);
        const double parallel =
            (east.xM - here.xM) /
            groundDistanceM({lat, 10.0, 0.0}, {lat, 10.0 + step, 0.0});
        const double meridian =
            (north.yM - south.yM) /
            groundDistanceM({lat - step, 10.0, 0.0}, {lat + step, 10.0, 0.0});
        const ts::MapScale scale = projection->scaleAt(lat);
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
    QCOMPARE(mercator.parameters().latitudeFactor, 1.0);
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
    constexpr int samples = 256;
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
    int compared = 0;
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
          const ts::ProjectionShaderReference shaderReference =
              projection.shaderReference(referenceLat, 0.0);
          block.r0 = shaderReference.r0;
          block.r1 = shaderReference.r1;
          const float stepLat =
              static_cast<float>(ts::degToRad(span) / samples);
          const float stepLon = 0.5F * stepLat;
          const float first = -0.5F * samples;
          block.which = {static_cast<float>(p), 0.0F, 0.0F, 0.0F};
          std::array<QImage, 2> images;
          for (int component = 0; component < 2; ++component) {
            block.steps = {stepLon, stepLat, first,
                           static_cast<float>(component)};
            uniforms->update(0, &block, sizeof(block));
            ts::PassDesc pass;
            pass.target = target.get();
            pass.viewportPx = QRect(0, 0, samples, 1);
            device.beginPass(pass);
            device.setPipeline(*pipeline);
            device.setUniformBuffer(0, *uniforms, 0, sizeof(block));
            device.draw(3, 0, 1);
            device.endPass();
            images.at(static_cast<std::size_t>(component)) =
                device.readPixels(*target);
          }
          QCOMPARE(device.takeErrors(), QString());
          for (int i = 0; i < samples; ++i) {
            std::array<float, 2> gpu{};
            for (std::size_t component = 0; component < 2; ++component) {
              std::memcpy(&gpu.at(component),
                          images.at(component).constScanLine(0) + 4 * i, 4);
            }
            // The same offsets the shader used, as the floats it saw.
            const float index = static_cast<float>(i) + first;
            const double dLonRad = static_cast<double>(stepLon * index);
            const double dLatRad = static_cast<double>(stepLat * index);
            const ts::MapPoint expected = projection.forward(
                clampedReference + ts::radToDeg(dLatRad), 0.0);
            const double expectedX = ts::wgs84::semiMajorAxisM * dLonRad;
            const double expectedY = expected.yM - reference.yM;
            // Float arithmetic: a few parts in ten million of the offset,
            // more for offsets of tens of degrees (which only the coarsest
            // tiles have, where a pixel is kilometres).
            const double relative = 2e-6 + 2e-5 * std::abs(dLatRad);
            const double errorX =
                std::abs(static_cast<double>(gpu[0]) - expectedX);
            const double errorY =
                std::abs(static_cast<double>(gpu[1]) - expectedY);
            QVERIFY(std::isfinite(gpu[0]) && std::isfinite(gpu[1]));
            worstRelative =
                std::max(worstRelative,
                         errorY / (1e-6 + relative * std::abs(expectedY)));
            if (errorX > 1e-6 + 2e-6 * std::abs(expectedX) ||
                errorY > 1e-6 + relative * std::abs(expectedY)) {
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
            ++compared;
          }
        }
      }
    }
    QCOMPARE(compared, 3 * 9 * 7 * samples);
    qInfo("GLSL against C++: worst error %.2f of the allowance", worstRelative);
  }

  void concurrentSharedInstances()
  {
    // Projections are immutable: many threads share the registered ones.
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [](int thread) {
          bool ok = true;
          for (int i = 0; i < 2000; ++i) {
            for (const ts::Projection* projection : ts::projections()) {
              const double lat = -80.0 + (thread * 7 + i) % 160;
              const double lon = -179.0 + (thread * 13 + i * 3) % 358;
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
      for (int i = 0; i < 1000; ++i) {
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
      for (int i = 0; i < 1000; ++i) {
        sum += mercator.inverse(ts::MapPoint{1.0e4 * i, -1.5e7 + 3.0e4 * i})
                   ->latDeg;
      }
    }
    QVERIFY(std::isfinite(sum));
  }
};

QTEST_MAIN(TestProjection)
#include "tst_projection.moc"
