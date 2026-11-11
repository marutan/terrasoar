// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include <QtTest>

#include <proj.h>

#include "concurrency.h"
#include "ts/deps/projcontext.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodesic.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/geoformat.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/maths/ray.h"
#include "ts/maths/vec3.h"

namespace
{

/** Round-trip tolerance from the Phase 1 plan: one millimetre. */
constexpr double roundTripToleranceM = 1e-3;

/** Heights for the global grid: deep below the surface to geostationary. */
constexpr std::array<double, 6> gridHeightsM = {-1.0e6, -1.0e4, 0.0,
                                                8848.0, 1.0e5,  3.6e7};

QString dataFile(const char* name)
{
  return QDir(QStringLiteral(TS_TEST_DATA_DIR)).filePath(QLatin1String(name));
}

/** Difference of two longitudes in degrees, in [-180, 180). */
double lonDifferenceDeg(double a, double b)
{
  return ts::normaliseLonDeg(a - b);
}

/** One line of the GeographicLib geodesic test set. */
struct GeodesicCase {
  double lat1Deg = 0.0;
  double lon1Deg = 0.0;
  double azimuth1Deg = 0.0;
  double lat2Deg = 0.0;
  double lon2Deg = 0.0;
  double azimuth2Deg = 0.0;
  double distanceM = 0.0;
};

std::vector<GeodesicCase> readGeodesicCases()
{
  std::vector<GeodesicCase> cases;
  QFile file(dataFile("geodtest-sample.dat"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return cases;
  }
  while (!file.atEnd()) {
    const QByteArray line = file.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#')) {
      continue;
    }
    const QList<QByteArray> fields = line.simplified().split(' ');
    if (fields.size() < 7) {
      continue;
    }
    GeodesicCase c;
    c.lat1Deg = fields.at(0).toDouble();
    c.lon1Deg = fields.at(1).toDouble();
    c.azimuth1Deg = fields.at(2).toDouble();
    c.lat2Deg = fields.at(3).toDouble();
    c.lon2Deg = fields.at(4).toDouble();
    c.azimuth2Deg = fields.at(5).toDouble();
    c.distanceM = fields.at(6).toDouble();
    cases.push_back(c);
  }
  return cases;
}

/** PROJ's EPSG:4979 (lat, lon, h) → EPSG:4978 (ECEF) transform. */
class ProjGeocentric
{
public:
  explicit ProjGeocentric(const ts::ProjContext& context) : m_context(context)
  {
    PJ* transform = proj_create_crs_to_crs(context.get(), "EPSG:4979",
                                           "EPSG:4978", nullptr);
    if (transform != nullptr) {
      m_transform = proj_normalize_for_visualization(context.get(), transform);
      proj_destroy(transform);
    }
  }

  ~ProjGeocentric() { proj_destroy(m_transform); }

  ProjGeocentric(const ProjGeocentric&) = delete;
  ProjGeocentric& operator=(const ProjGeocentric&) = delete;
  ProjGeocentric(ProjGeocentric&&) = delete;
  ProjGeocentric& operator=(ProjGeocentric&&) = delete;

  bool isValid() const { return m_transform != nullptr; }

  QString errorString() const { return m_context.lastErrorString(); }

  ts::Vec3 toEcef(const ts::Geodetic& position) const
  {
    // Normalised axis order: longitude, latitude, height.
    const PJ_COORD out = proj_trans(
        m_transform, PJ_FWD,
        proj_coord(position.lonDeg, position.latDeg, position.heightM, 0.0));
    return ts::Vec3{out.xyz.x, out.xyz.y, out.xyz.z};
  }

  ts::Geodetic toGeodetic(const ts::Vec3& positionEcef) const
  {
    const PJ_COORD out = proj_trans(
        m_transform, PJ_INV,
        proj_coord(positionEcef.x, positionEcef.y, positionEcef.z, 0.0));
    return ts::Geodetic{out.lpz.phi, out.lpz.lam, out.lpz.z};
  }

private:
  const ts::ProjContext& m_context;
  PJ* m_transform = nullptr;
};

} // namespace

/** Unit and regression tests for ts/geo. */
class TestGeo : public QObject
{
  Q_OBJECT

private slots:
  void wgs84Constants()
  {
    // Derived values as published in NGA.STND.0036.
    QVERIFY(std::abs(ts::wgs84::semiMinorAxisM - 6356752.314245) < 1e-6);
    QVERIFY(std::abs(ts::wgs84::eccentricitySquared - 0.00669437999014) <
            1e-14);
    QVERIFY(std::abs(ts::wgs84::secondEccentricitySquared - 0.00673949674228) <
            1e-14);
  }

  void normaliseLon_data()
  {
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("expectedDeg");
    QTest::newRow("zero") << 0.0 << 0.0;
    QTest::newRow("east") << 179.5 << 179.5;
    QTest::newRow("antimeridian") << 180.0 << -180.0;
    QTest::newRow("west") << -180.0 << -180.0;
    QTest::newRow("wrap east") << 190.0 << -170.0;
    QTest::newRow("wrap west") << -190.0 << 170.0;
    QTest::newRow("full turns") << 725.0 << 5.0;
    QTest::newRow("negative turns") << -725.0 << -5.0;
  }

  void normaliseLon()
  {
    QFETCH(double, lonDeg);
    QFETCH(double, expectedDeg);
    QVERIFY(std::abs(ts::normaliseLonDeg(lonDeg) - expectedDeg) < 1e-12);
  }

  void knownPoints_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("heightM");
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");
    QTest::addColumn<double>("z");
    const double a = ts::wgs84::semiMajorAxisM;
    const double b = ts::wgs84::semiMinorAxisM;
    QTest::newRow("equator, prime meridian")
        << 0.0 << 0.0 << 0.0 << a << 0.0 << 0.0;
    QTest::newRow("equator, 90E") << 0.0 << 90.0 << 0.0 << 0.0 << a << 0.0;
    QTest::newRow("equator, 180") << 0.0 << 180.0 << 0.0 << -a << 0.0 << 0.0;
    QTest::newRow("north pole") << 90.0 << 0.0 << 0.0 << 0.0 << 0.0 << b;
    QTest::newRow("south pole") << -90.0 << 0.0 << 0.0 << 0.0 << 0.0 << -b;
    QTest::newRow("1 km up at 90W")
        << 0.0 << -90.0 << 1000.0 << 0.0 << -(a + 1000.0) << 0.0;
  }

  void knownPoints()
  {
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, heightM);
    QFETCH(double, x);
    QFETCH(double, y);
    QFETCH(double, z);
    const ts::Vec3 ecef =
        ts::geodeticToEcef(ts::Geodetic{latDeg, lonDeg, heightM});
    QVERIFY2(ts::distance(ecef, ts::Vec3{x, y, z}) < 1e-6,
             qPrintable(QStringLiteral("got %1 %2 %3")
                            .arg(ecef.x, 0, 'f', 6)
                            .arg(ecef.y, 0, 'f', 6)
                            .arg(ecef.z, 0, 'f', 6)));
    const ts::Geodetic back = ts::ecefToGeodetic(ecef);
    QVERIFY(std::abs(back.latDeg - latDeg) < 1e-12);
    QVERIFY(std::abs(back.heightM - heightM) < 1e-6);
    if (std::abs(latDeg) < 90.0) {
      QVERIFY(std::abs(lonDifferenceDeg(back.lonDeg, lonDeg)) < 1e-12);
    }
  }

  void roundTripGlobalGrid()
  {
    // Every whole degree, poles and both sides of the antimeridian
    // included, at each grid height: geodetic → ECEF → geodetic → ECEF.
    double worstM = 0.0;
    double worstHeightM = 0.0;
    int count = 0;
    for (const double heightM : gridHeightsM) {
      for (int lat = -90; lat <= 90; ++lat) {
        for (int lon = -180; lon <= 180; ++lon) {
          const ts::Geodetic start{static_cast<double>(lat),
                                   static_cast<double>(lon), heightM};
          const ts::Vec3 ecef = ts::geodeticToEcef(start);
          const ts::Geodetic back = ts::ecefToGeodetic(ecef);
          const double errorM = ts::distance(ts::geodeticToEcef(back), ecef);
          worstM = std::max(worstM, errorM);
          worstHeightM =
              std::max(worstHeightM, std::abs(back.heightM - heightM));
          ++count;
        }
      }
    }
    qInfo("%d points; worst ECEF round-trip error %.3g m, worst height %.3g m",
          count, worstM, worstHeightM);
    QVERIFY(worstM < roundTripToleranceM);
    QVERIFY(worstHeightM < roundTripToleranceM);
  }

  void roundTripLatLon()
  {
    // Latitude and longitude come back too, not just the ECEF point.
    for (const double heightM : gridHeightsM) {
      for (int i = 0; i < 25; ++i) {
        const double lat = -89.75 + 7.25 * i;
        for (int j = 0; j < 32; ++j) {
          const double lon = -179.875 + 11.5 * j;
          const ts::Geodetic back = ts::ecefToGeodetic(
              ts::geodeticToEcef(ts::Geodetic{lat, lon, heightM}));
          QVERIFY(std::abs(back.latDeg - lat) < 1e-9);
          QVERIFY(std::abs(lonDifferenceDeg(back.lonDeg, lon)) < 1e-9);
        }
      }
    }
  }

  void nearThePolarAxis()
  {
    // Points a few micrometres to a metre from the axis, where the general
    // formula is close to 0/0.
    for (const double offsetM : {1e-6, 1e-3, 1.0}) {
      const ts::Vec3 ecef{offsetM, 0.0, ts::wgs84::semiMinorAxisM + 50.0};
      const ts::Geodetic position = ts::ecefToGeodetic(ecef);
      QVERIFY(std::abs(position.heightM - 50.0) < roundTripToleranceM);
      QVERIFY(ts::distance(ts::geodeticToEcef(position), ecef) <
              roundTripToleranceM);
    }
    const ts::Geodetic south = ts::ecefToGeodetic(
        ts::Vec3{0.0, 0.0, -ts::wgs84::semiMinorAxisM - 7.0});
    QCOMPARE(south.latDeg, -90.0);
    QCOMPARE(south.lonDeg, 0.0);
    QVERIFY(std::abs(south.heightM - 7.0) < 1e-9);
  }

  void againstProj()
  {
    // EPSG:4979 ↔ EPSG:4978 with PROJ as the reference. The forward
    // conversion is closed form in both, so it must agree at every height.
    // PROJ's inverse iterates with a stopping tolerance and loses accuracy
    // far from the surface (about 2 cm at 1000 km below it and 0.4 m at
    // geostationary height, while ours round-trips to nanometres; see
    // roundTripGlobalGrid), so the inverse is compared within 100 km of the
    // surface only and reported elsewhere.
    const ts::ProjContext context;
    QVERIFY(context.isValid());
    const ProjGeocentric proj(context);
    QVERIFY2(proj.isValid(), qPrintable(proj.errorString()));
    double worstForwardM = 0.0;
    for (const double heightM : gridHeightsM) {
      double worstInverseM = 0.0;
      for (int lat = -90; lat <= 90; lat += 5) {
        for (int lon = -180; lon <= 180; lon += 10) {
          const ts::Geodetic position{static_cast<double>(lat),
                                      static_cast<double>(lon), heightM};
          const ts::Vec3 reference = proj.toEcef(position);
          worstForwardM =
              std::max(worstForwardM,
                       ts::distance(ts::geodeticToEcef(position), reference));
          // Compare the recovered positions as points, so longitude at the
          // poles doesn't matter.
          worstInverseM = std::max(
              worstInverseM,
              ts::distance(ts::geodeticToEcef(proj.toGeodetic(reference)),
                           ts::geodeticToEcef(ts::ecefToGeodetic(reference))));
        }
      }
      qInfo("height %.0f m: worst inverse difference from PROJ %.3g m", heightM,
            worstInverseM);
      if (std::abs(heightM) <= 1.0e5) {
        QVERIFY(worstInverseM < roundTripToleranceM);
      }
    }
    qInfo("worst forward difference from PROJ %.3g m", worstForwardM);
    QVERIFY(worstForwardM < roundTripToleranceM);
  }

  void normals()
  {
    for (int i = -6; i <= 6; ++i) {
      const double lat = 15.0 * i;
      for (int j = -6; j < 6; ++j) {
        const double lon = 30.0 * j;
        const ts::Vec3 normal = ts::surfaceNormalEcef(lat, lon);
        QVERIFY(std::abs(ts::length(normal) - 1.0) < 1e-15);
        // The gradient form agrees on the surface.
        const ts::Vec3 surface =
            ts::geodeticToEcef(ts::Geodetic{lat, lon, 0.0});
        QVERIFY(ts::distance(ts::ellipsoidNormalEcef(surface), normal) < 1e-12);
        // Moving along the normal changes only the height.
        const ts::Geodetic raised =
            ts::ecefToGeodetic(surface + normal * 500.0);
        QVERIFY(std::abs(raised.heightM - 500.0) < 1e-6);
        QVERIFY(std::abs(raised.latDeg - lat) < 1e-9);

        const ts::LocalFrame frame = ts::localFrame(lat, lon);
        QVERIFY(ts::distance(frame.upEcef, normal) < 1e-15);
        QVERIFY(std::abs(ts::dot(frame.eastEcef, frame.northEcef)) < 1e-15);
        QVERIFY(std::abs(ts::dot(frame.eastEcef, frame.upEcef)) < 1e-15);
        QVERIFY(std::abs(ts::dot(frame.northEcef, frame.upEcef)) < 1e-15);
        QVERIFY(std::abs(ts::length(frame.eastEcef) - 1.0) < 1e-15);
        QVERIFY(std::abs(ts::length(frame.northEcef) - 1.0) < 1e-15);
        // Right-handed: east × north = up.
        QVERIFY(ts::distance(ts::cross(frame.eastEcef, frame.northEcef),
                             frame.upEcef) < 1e-15);
      }
    }
    // North points to the north pole from the equator.
    const ts::LocalFrame equator = ts::localFrame(0.0, 0.0);
    QVERIFY(ts::distance(equator.northEcef, ts::Vec3{0.0, 0.0, 1.0}) < 1e-15);
    QVERIFY(ts::distance(equator.eastEcef, ts::Vec3{0.0, 1.0, 0.0}) < 1e-15);
  }

  void rayPickStraightDown_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::newRow("Greenwich") << 51.4769 << -0.0005;
    QTest::newRow("Sydney") << -33.8568 << 151.2153;
    QTest::newRow("near north pole") << 89.999 << 45.0;
    QTest::newRow("antimeridian") << -17.0 << 180.0;
    QTest::newRow("equator") << 0.0 << -78.5;
  }

  void rayPickStraightDown()
  {
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    // From 1000 km above the point, straight down its normal.
    const ts::Vec3 above =
        ts::geodeticToEcef(ts::Geodetic{latDeg, lonDeg, 1.0e6});
    const ts::Ray ray{above, -ts::surfaceNormalEcef(latDeg, lonDeg)};
    const std::optional<ts::Vec3> hit = ts::intersectEllipsoidPoint(ray);
    if (!hit) {
      QFAIL("The ray missed.");
    }
    const ts::Geodetic picked = ts::ecefToGeodetic(*hit);
    QVERIFY(std::abs(picked.heightM) < roundTripToleranceM);
    QVERIFY(std::abs(picked.latDeg - latDeg) < 1e-9);
    QVERIFY(std::abs(lonDifferenceDeg(picked.lonDeg, lonDeg)) < 1e-9);
    // The ray parameter is the distance: the direction has length 1.
    const std::optional<double> t = ts::intersectEllipsoid(ray);
    if (!t) {
      QFAIL("The ray missed.");
    }
    QVERIFY(std::abs(*t - 1.0e6) < 1e-3);
  }

  void rayPickFromFarAway()
  {
    // A camera ten Earth radii away looks at points on the near side; the
    // ray must hit exactly that point, not just close to it.
    const ts::Vec3 camera{10.0 * ts::wgs84::semiMajorAxisM, 0.0, 0.0};
    for (int i = -8; i <= 8; ++i) {
      const double lat = 10.0 * i;
      for (int j = -8; j <= 8; ++j) {
        const double lon = 10.0 * j;
        // From ten radii the visible cap reaches about 84 degrees from the
        // sub-camera point; keep to points well inside it (70 degrees).
        if (std::cos(ts::degToRad(lat)) * std::cos(ts::degToRad(lon)) <
            std::cos(ts::degToRad(70.0))) {
          continue;
        }
        const ts::Vec3 target = ts::geodeticToEcef(ts::Geodetic{lat, lon, 0.0});
        const ts::Ray ray{camera, target - camera};
        const std::optional<ts::Vec3> hit = ts::intersectEllipsoidPoint(ray);
        if (!hit) {
          QFAIL("The ray missed.");
        }
        QVERIFY2(ts::distance(*hit, target) < roundTripToleranceM,
                 qPrintable(QStringLiteral("lat %1 lon %2: %3 m off")
                                .arg(lat)
                                .arg(lon)
                                .arg(ts::distance(*hit, target))));
      }
    }
    // A point on the far side is hidden: the ray hits the near side first.
    const ts::Vec3 farSide = ts::geodeticToEcef(ts::Geodetic{0.0, 180.0, 0.0});
    const std::optional<ts::Vec3> blocked =
        ts::intersectEllipsoidPoint(ts::Ray{camera, farSide - camera});
    if (!blocked) {
      QFAIL("The ray missed.");
    }
    QVERIFY(ts::distance(*blocked, ts::Vec3{ts::wgs84::semiMajorAxisM, 0.0,
                                            0.0}) < roundTripToleranceM);
  }

  void rayMisses()
  {
    const ts::Vec3 camera{3.0 * ts::wgs84::semiMajorAxisM, 0.0, 0.0};
    // Looking away from the Earth.
    QVERIFY(!ts::intersectEllipsoid(ts::Ray{camera, ts::Vec3{1.0, 0.0, 0.0}}));
    // Passing 1 km outside the pole (and outside the equator in y).
    QVERIFY(!ts::intersectEllipsoid(
        ts::Ray{ts::Vec3{camera.x, 0.0, ts::wgs84::semiMinorAxisM + 1000.0},
                ts::Vec3{-1.0, 0.0, 0.0}}));
    QVERIFY(!ts::intersectEllipsoid(
        ts::Ray{ts::Vec3{camera.x, ts::wgs84::semiMajorAxisM + 1000.0, 0.0},
                ts::Vec3{-1.0, 0.0, 0.0}}));
    // Passing 1 km inside them hits.
    QVERIFY(ts::intersectEllipsoid(
        ts::Ray{ts::Vec3{camera.x, 0.0, ts::wgs84::semiMinorAxisM - 1000.0},
                ts::Vec3{-1.0, 0.0, 0.0}}));
    // A zero direction is no ray.
    QVERIFY(!ts::intersectEllipsoid(ts::Ray{camera, ts::Vec3{}}));
  }

  void rayFromInside()
  {
    // From the centre, a ray leaves through the surface in its direction.
    const ts::Vec3 direction = ts::surfaceNormalEcef(30.0, 60.0);
    const std::optional<ts::Vec3> exit =
        ts::intersectEllipsoidPoint(ts::Ray{ts::Vec3{}, direction});
    if (!exit) {
      QFAIL("The ray missed.");
    }
    QVERIFY(std::abs(ts::ecefToGeodetic(*exit).heightM) < roundTripToleranceM);
    QVERIFY(ts::dot(*exit, direction) > 0.0);
  }

  void rhumbLines()
  {
    // Due north a rhumb line is the meridian, the same as the geodesic.
    const ts::GeodesicDirect north = ts::rhumbDirect(10.0, 20.0, 0.0, 5.0e5);
    const ts::GeodesicDirect meridian =
        ts::geodesicDirect(10.0, 20.0, 0.0, 5.0e5);
    QVERIFY(std::abs(north.latDeg - meridian.latDeg) < 1e-9);
    QVERIFY(std::abs(north.lonDeg - 20.0) < 1e-12);
    // Split into ten pieces with the same azimuth it ends in the same
    // place, which the camera's frame-rate independence relies on.
    double lat = 35.0;
    double lon = 139.0;
    for (int i = 0; i < 10; ++i) {
      const ts::GeodesicDirect step = ts::rhumbDirect(lat, lon, 63.0, 1.0e5);
      lat = step.latDeg;
      lon = step.lonDeg;
    }
    const ts::GeodesicDirect once = ts::rhumbDirect(35.0, 139.0, 63.0, 1.0e6);
    QVERIFY(std::abs(lat - once.latDeg) < 1e-11);
    QVERIFY(std::abs(lonDifferenceDeg(lon, once.lonDeg)) < 1e-11);
    QCOMPARE(once.azimuth2Deg, 63.0);
    // The inverse gives back the distance and azimuth.
    const ts::GeodesicInverse back =
        ts::rhumbInverse(35.0, 139.0, once.latDeg, once.lonDeg);
    QVERIFY(std::abs(back.distanceM - 1.0e6) < 1e-6);
    QVERIFY(std::abs(back.azimuth1Deg - 63.0) < 1e-9);
    QCOMPARE(back.azimuth2Deg, back.azimuth1Deg);
  }

  void geodesicKnownCase()
  {
    // JFK to Heathrow, the example in GeographicLib's documentation.
    const ts::GeodesicInverse inverse =
        ts::geodesicInverse(40.6, -73.8, 51.6, -0.5);
    QVERIFY(std::abs(inverse.distanceM - 5551759.4) < 0.1);
    QVERIFY(std::abs(inverse.azimuth1Deg - 51.198882845579824) < 1e-9);
    const ts::GeodesicDirect direct =
        ts::geodesicDirect(40.6, -73.8, inverse.azimuth1Deg, inverse.distanceM);
    QVERIFY(std::abs(direct.latDeg - 51.6) < 1e-12);
    QVERIFY(std::abs(direct.lonDeg + 0.5) < 1e-12);
  }

  void geodesicReferenceData()
  {
    const std::vector<GeodesicCase> cases = readGeodesicCases();
    QCOMPARE(static_cast<int>(cases.size()), 500);
    double worstDistanceM = 0.0;
    double worstPositionDeg = 0.0;
    for (const GeodesicCase& c : cases) {
      // Inverse: the distance is well defined everywhere.
      const ts::GeodesicInverse inverse =
          ts::geodesicInverse(c.lat1Deg, c.lon1Deg, c.lat2Deg, c.lon2Deg);
      worstDistanceM =
          std::max(worstDistanceM, std::abs(inverse.distanceM - c.distanceM));
      // Direct: start, azimuth and distance give the end point.
      const ts::GeodesicDirect direct =
          ts::geodesicDirect(c.lat1Deg, c.lon1Deg, c.azimuth1Deg, c.distanceM);
      worstPositionDeg =
          std::max({worstPositionDeg, std::abs(direct.latDeg - c.lat2Deg),
                    std::abs(c.lat2Deg) < 90.0
                        ? std::abs(lonDifferenceDeg(direct.lonDeg, c.lon2Deg)) *
                              std::cos(ts::degToRad(c.lat2Deg))
                        : 0.0});
    }
    qInfo("%d geodesics: worst distance error %.3g m, worst end point %.3g deg",
          static_cast<int>(cases.size()), worstDistanceM, worstPositionDeg);
    // GeographicLib is accurate to about 15 nm; allow 1 µm.
    QVERIFY(worstDistanceM < 1e-6);
    // 1e-12 degrees is about 0.1 µm on the ground.
    QVERIFY(worstPositionDeg < 1e-12);
  }

  void textForTheUser()
  {
    // Coordinates and distances as the status bar and scale bar show them.
    const QLocale english(QLocale::English, QLocale::UnitedStates);
    QCOMPARE(ts::formatLatitude(51.4769, english),
             QStringLiteral("51.47690° N"));
    QCOMPARE(ts::formatLatitude(-33.9, english), QStringLiteral("33.90000° S"));
    QCOMPARE(ts::formatLatitude(-1e-9, english), QStringLiteral("0.00000° N"));
    QCOMPARE(ts::formatLongitude(-0.0005, english),
             QStringLiteral("0.00050° W"));
    QCOMPARE(ts::formatLongitude(-180.0, english),
             QStringLiteral("180.00000° E"));
    QCOMPARE(ts::formatLongitude(370.0, english),
             QStringLiteral("10.00000° E"));
    QCOMPARE(ts::formatDistance(8.5, english), QStringLiteral("8.5 m"));
    QCOMPARE(ts::formatDistance(850.0, english), QStringLiteral("850 m"));
    QCOMPARE(ts::formatDistance(12345.0, english), QStringLiteral("12.3 km"));
    QCOMPARE(ts::formatDistance(1.2345e7, english),
             QStringLiteral("12,345 km"));
    // Whole degrees, for the scale bar.
    QCOMPARE(ts::formatWholeLatitude(51.4769, english),
             QStringLiteral("51° N"));
    QCOMPARE(ts::formatWholeLatitude(-33.9, english), QStringLiteral("34° S"));
    QCOMPARE(ts::formatWholeLatitude(-0.4, english), QStringLiteral("0° N"));
    QCOMPARE(ts::formatWholeLatitude(-0.6, english), QStringLiteral("1° S"));
    // Round distances in the largest unit that keeps them whole.
    QCOMPARE(ts::formatRoundDistance(2.0e6, english),
             QStringLiteral("2,000 km"));
    QCOMPARE(ts::formatRoundDistance(5000.0, english), QStringLiteral("5 km"));
    QCOMPARE(ts::formatRoundDistance(500.0, english), QStringLiteral("500 m"));
    QCOMPARE(ts::formatRoundDistance(1500.0, english),
             QStringLiteral("1,500 m"));
    QCOMPARE(ts::formatRoundDistance(1.0, english), QStringLiteral("1 m"));
    QCOMPARE(ts::formatRoundDistance(0.2, english), QStringLiteral("20 cm"));
    QCOMPARE(ts::formatRoundDistance(0.05, english), QStringLiteral("5 cm"));
    QCOMPARE(ts::formatRoundDistance(0.0, english), QStringLiteral("0 cm"));
    // Another locale's digits and separators.
    const QLocale german(QLocale::German, QLocale::Germany);
    QCOMPARE(ts::formatRoundDistance(2.0e6, german),
             QStringLiteral("2.000 km"));
    QCOMPARE(ts::formatLatitude(51.4769, german),
             QStringLiteral("51,47690° N"));
  }

  void concurrentSharedGeodesics()
  {
    // Free functions over shared constants and GeographicLib's shared WGS 84
    // object: every thread must get the single-threaded answers.
    const std::vector<GeodesicCase> cases = readGeodesicCases();
    QVERIFY(!cases.empty());
    std::vector<double> expectedM;
    expectedM.reserve(cases.size());
    for (const GeodesicCase& c : cases) {
      expectedM.push_back(
          ts::geodesicInverse(c.lat1Deg, c.lon1Deg, c.lat2Deg, c.lon2Deg)
              .distanceM);
    }
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&cases, &expectedM](int thread) {
          for (std::size_t i = 0; i < cases.size(); ++i) {
            const GeodesicCase& c = cases[i];
            if (ts::geodesicInverse(c.lat1Deg, c.lon1Deg, c.lat2Deg, c.lon2Deg)
                    .distanceM != expectedM[i]) {
              return false;
            }
            const ts::Geodetic position{c.lat1Deg, c.lon1Deg, 100.0 * thread};
            const ts::Geodetic back =
                ts::ecefToGeodetic(ts::geodeticToEcef(position));
            if (std::abs(back.heightM - position.heightM) > 1e-6) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkGeodeticToEcef()
  {
    const ts::Geodetic position{51.4769, -0.0005, 46.0};
    ts::Vec3 result;
    QBENCHMARK
    {
      result = ts::geodeticToEcef(position);
    }
    QVERIFY(result.x > 0.0);
  }

  void benchmarkEcefToGeodetic()
  {
    const ts::Vec3 ecef =
        ts::geodeticToEcef(ts::Geodetic{51.4769, -0.0005, 46.0});
    ts::Geodetic result;
    QBENCHMARK
    {
      result = ts::ecefToGeodetic(ecef);
    }
    QVERIFY(std::abs(result.heightM - 46.0) < 1e-6);
  }

  void benchmarkRayPick()
  {
    const ts::Vec3 camera{10.0 * ts::wgs84::semiMajorAxisM, 0.0, 0.0};
    const ts::Ray ray{
        camera, ts::geodeticToEcef(ts::Geodetic{20.0, 10.0, 0.0}) - camera};
    std::optional<double> t;
    QBENCHMARK
    {
      t = ts::intersectEllipsoid(ray);
    }
    QVERIFY(t.has_value());
  }

  void benchmarkGeodesicInverse()
  {
    ts::GeodesicInverse result;
    QBENCHMARK
    {
      result = ts::geodesicInverse(40.6, -73.8, 51.6, -0.5);
    }
    QVERIFY(result.distanceM > 0.0);
  }
};

QTEST_GUILESS_MAIN(TestGeo)
#include "tst_geo.moc"
