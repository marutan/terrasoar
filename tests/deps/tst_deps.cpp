// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <utility>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <geos_c.h>
#include <proj.h>

#include "concurrency.h"
#include "ts/deps/dependencies.h"
#include "ts/deps/gdaldataset.h"
#include "ts/deps/geoscontext.h"
#include "ts/deps/projcontext.h"
#include "ts/deps/sqliteconnection.h"

namespace
{

QString dataFile(const char* name)
{
  return QDir(QStringLiteral(TS_TEST_DATA_DIR)).filePath(QLatin1String(name));
}

/** Converts lat/lon/height (degrees, metres) to ECEF metres with PROJ. */
bool geodeticToEcef(const ts::ProjContext& context, double latDeg,
                    double lonDeg, double heightM, PJ_COORD* out)
{
  PJ* transform =
      proj_create_crs_to_crs(context.get(), "EPSG:4979", "EPSG:4978", nullptr);
  if (transform == nullptr) {
    return false;
  }
  PJ* normalised = proj_normalize_for_visualization(context.get(), transform);
  proj_destroy(transform);
  if (normalised == nullptr) {
    return false;
  }
  // Normalised axis order: longitude, latitude, height.
  const PJ_COORD in = proj_coord(lonDeg, latDeg, heightM, 0.0);
  *out = proj_trans(normalised, PJ_FWD, in);
  proj_destroy(normalised);
  return std::isfinite(out->xyz.x);
}

/** Reads sample.wkt with GEOS and returns its area, or -1 on failure. */
double sampleWktArea(const ts::GeosContext& context, const QByteArray& wkt)
{
  GEOSContextHandle_t handle = static_cast<GEOSContextHandle_t>(context.get());
  GEOSWKTReader* reader = GEOSWKTReader_create_r(handle);
  GEOSGeometry* geometry =
      GEOSWKTReader_read_r(handle, reader, wkt.constData());
  GEOSWKTReader_destroy_r(handle, reader);
  if (geometry == nullptr) {
    return -1.0;
  }
  double area = -1.0;
  if (GEOSisValid_r(handle, geometry) != 1 ||
      GEOSArea_r(handle, geometry, &area) != 1) {
    area = -1.0;
  }
  GEOSGeom_destroy_r(handle, geometry);
  return area;
}

/**
 * Converts @p wkt to WKB. GEOS's WKT reader and writer set the process-wide
 * C locale while they run, so only call this from one thread.
 */
QByteArray wktToWkb(const QByteArray& wkt)
{
  const ts::GeosContext context;
  GEOSContextHandle_t handle = static_cast<GEOSContextHandle_t>(context.get());
  GEOSWKTReader* reader = GEOSWKTReader_create_r(handle);
  GEOSGeometry* geometry =
      GEOSWKTReader_read_r(handle, reader, wkt.constData());
  GEOSWKTReader_destroy_r(handle, reader);
  if (geometry == nullptr) {
    return QByteArray();
  }
  GEOSWKBWriter* writer = GEOSWKBWriter_create_r(handle);
  std::size_t size = 0;
  unsigned char* bytes = GEOSWKBWriter_write_r(handle, writer, geometry, &size);
  QByteArray wkb(reinterpret_cast<const char*>(bytes), static_cast<int>(size));
  GEOSFree_r(handle, bytes);
  GEOSWKBWriter_destroy_r(handle, writer);
  GEOSGeom_destroy_r(handle, geometry);
  return wkb;
}

/** Reads @p wkb with GEOS and returns its area, or -1 on failure. */
double wkbArea(const ts::GeosContext& context, const QByteArray& wkb)
{
  GEOSContextHandle_t handle = static_cast<GEOSContextHandle_t>(context.get());
  GEOSWKBReader* reader = GEOSWKBReader_create_r(handle);
  GEOSGeometry* geometry = GEOSWKBReader_read_r(
      handle, reader, reinterpret_cast<const unsigned char*>(wkb.constData()),
      static_cast<std::size_t>(wkb.size()));
  GEOSWKBReader_destroy_r(handle, reader);
  if (geometry == nullptr) {
    return -1.0;
  }
  double area = -1.0;
  if (GEOSisValid_r(handle, geometry) != 1 ||
      GEOSArea_r(handle, geometry, &area) != 1) {
    area = -1.0;
  }
  GEOSGeom_destroy_r(handle, geometry);
  return area;
}

QByteArray readSampleWkt()
{
  QFile file(dataFile("sample.wkt"));
  if (!file.open(QIODevice::ReadOnly)) {
    return QByteArray();
  }
  return file.readAll().trimmed();
}

} // namespace

/**
 * Dependency tests: GDAL, PROJ, GEOS and SQLite report their versions, open
 * sample files, and work from many threads with one handle per thread.
 */
class TestDeps : public QObject
{
  Q_OBJECT

private slots:
  void versions()
  {
    const ts::DependencyVersions versions = ts::dependencyVersions();
    qInfo("GDAL %s, PROJ %s, GEOS %s, SQLite %s, GeographicLib %s",
          qPrintable(versions.gdal), qPrintable(versions.proj),
          qPrintable(versions.geos), qPrintable(versions.sqlite),
          qPrintable(versions.geographicLib));
    QVERIFY(!versions.gdal.isEmpty());
    QVERIFY(!versions.proj.isEmpty());
    QVERIFY(!versions.geos.isEmpty());
    QVERIFY(!versions.sqlite.isEmpty());
    QVERIFY(!versions.geographicLib.isEmpty());
    // Minimum versions in docs/building.md.
    QVERIFY(QVersionNumber::fromString(versions.gdal) >= QVersionNumber(3, 4));
    QVERIFY(QVersionNumber::fromString(versions.proj) >= QVersionNumber(8));
    QVERIFY(QVersionNumber::fromString(versions.geos) >= QVersionNumber(3, 10));
    QVERIFY(QVersionNumber::fromString(versions.sqlite) >=
            QVersionNumber(3, 31));
    QVERIFY(QVersionNumber::fromString(versions.geographicLib) >=
            QVersionNumber(1, 52));
  }

  void gdalOpensSampleGeoJson()
  {
    const ts::GdalDataset dataset(dataFile("sample.geojson"));
    QVERIFY2(dataset.isOpen(), qPrintable(dataset.errorString()));
    QCOMPARE(dataset.driverShortName(), QStringLiteral("GeoJSON"));
    QCOMPARE(dataset.layerCount(), 1);
    QCOMPARE(dataset.featureCount(0), 2LL);
    QCOMPARE(dataset.featureCount(1), -1LL);
  }

  void gdalReportsMissingFile()
  {
    const ts::GdalDataset dataset(dataFile("does-not-exist.geojson"));
    QVERIFY(!dataset.isOpen());
    QVERIFY(!dataset.errorString().isEmpty());
    QCOMPARE(dataset.layerCount(), 0);
  }

  void gdalMove()
  {
    ts::GdalDataset first(dataFile("sample.geojson"));
    QVERIFY(first.isOpen());
    ts::GdalDataset second(std::move(first));
    QVERIFY(second.isOpen());
    // Checks the moved-from state on purpose.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    QVERIFY(!first.isOpen());
    ts::GdalDataset third;
    third = std::move(second);
    QCOMPARE(third.featureCount(0), 2LL);
  }

  void projGeodeticToEcef()
  {
    const ts::ProjContext context;
    QVERIFY(context.isValid());
    PJ_COORD ecef{};
    // Uses proj.db: the sample file PROJ needs for EPSG codes.
    QVERIFY2(geodeticToEcef(context, 0.0, 0.0, 0.0, &ecef),
             qPrintable(context.lastErrorString()));
    QVERIFY(std::abs(ecef.xyz.x - 6378137.0) < 1e-3);
    QVERIFY(std::abs(ecef.xyz.y) < 1e-3);
    QVERIFY(std::abs(ecef.xyz.z) < 1e-3);

    QVERIFY(geodeticToEcef(context, 90.0, 0.0, 0.0, &ecef));
    QVERIFY(std::abs(ecef.xyz.z - 6356752.314245) < 1e-3);
  }

  void projReportsBadCrs()
  {
    const ts::ProjContext context;
    PJ* bad = proj_create(context.get(), "EPSG:999999");
    QVERIFY(bad == nullptr);
    QVERIFY(!context.lastErrorString().isEmpty());
  }

  void geosReadsSampleWkt()
  {
    const ts::GeosContext context;
    QVERIFY(context.isValid());
    const QByteArray wkt = readSampleWkt();
    QVERIFY(!wkt.isEmpty());
    QCOMPARE(sampleWktArea(context, wkt), 96.0);
  }

  void geosReportsParseError()
  {
    const ts::GeosContext context;
    QCOMPARE(sampleWktArea(context, QByteArrayLiteral("POLYGON ((0 0, 1")),
             -1.0);
    QVERIFY(!context.lastError().isEmpty());
  }

  void sqliteRoundTrip()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sample.sqlite"));
    {
      ts::SqliteConnection db(path,
                              ts::SqliteConnection::OpenMode::ReadWriteCreate);
      QVERIFY2(db.isOpen(), qPrintable(db.errorString()));
      QVERIFY(db.execute(QStringLiteral(
          "CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (40), (2);")));
    }
    ts::SqliteConnection db(path, ts::SqliteConnection::OpenMode::ReadOnly);
    QVERIFY(db.isOpen());
    bool ok = false;
    QCOMPARE(db.queryInteger(QStringLiteral("SELECT SUM(v) FROM t"), &ok),
             42LL);
    QVERIFY(ok);
    QVERIFY(!db.execute(QStringLiteral("INSERT INTO t VALUES (1)")));
    QVERIFY(!db.errorString().isEmpty());
  }

  void sqliteMissingFileReadOnly()
  {
    const ts::SqliteConnection db(QStringLiteral("/nonexistent/x.sqlite"),
                                  ts::SqliteConnection::OpenMode::ReadOnly);
    QVERIFY(!db.isOpen());
    QVERIFY(!db.errorString().isEmpty());
  }

  /** Every library used from many threads, each with its own handles. */
  void concurrentOwnInstances()
  {
    // GEOS's WKT reader is not safe to run on several threads at once (it
    // sets the process-wide C locale), so the threads read WKB instead.
    const QByteArray wkb = wktToWkb(readSampleWkt());
    QVERIFY(!wkb.isEmpty());
    const QString geojson = dataFile("sample.geojson");
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&](int thread) {
          const ts::ProjContext proj;
          const ts::GeosContext geos;
          ts::SqliteConnection db(
              QStringLiteral(":memory:"),
              ts::SqliteConnection::OpenMode::ReadWriteCreate);
          if (!db.isOpen() ||
              !db.execute(QStringLiteral("CREATE TABLE t (v INTEGER)"))) {
            return false;
          }
          for (int i = 0; i < 20; ++i) {
            PJ_COORD ecef{};
            if (!geodeticToEcef(proj, 0.0, 0.0, 0.0, &ecef) ||
                std::abs(ecef.xyz.x - 6378137.0) > 1e-3) {
              return false;
            }
            if (wkbArea(geos, wkb) != 96.0) {
              return false;
            }
            const ts::GdalDataset dataset(geojson);
            if (dataset.featureCount(0) != 2) {
              return false;
            }
            if (!db.execute(
                    QStringLiteral("INSERT INTO t VALUES (%1)").arg(thread))) {
              return false;
            }
          }
          bool ok = false;
          return db.queryInteger(QStringLiteral("SELECT COUNT(*) FROM t"),
                                 &ok) == 20 &&
                 ok;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkGeodeticToEcef()
  {
    const ts::ProjContext context;
    PJ* transform = proj_create_crs_to_crs(context.get(), "EPSG:4979",
                                           "EPSG:4978", nullptr);
    QVERIFY(transform != nullptr);
    PJ_COORD c = proj_coord(51.5, -0.1, 0.0, 0.0);
    QBENCHMARK
    {
      c = proj_trans(transform, PJ_FWD, proj_coord(51.5, -0.1, 0.0, 0.0));
    }
    QVERIFY(std::isfinite(c.xyz.x));
    proj_destroy(transform);
  }
};

QTEST_GUILESS_MAIN(TestDeps)
#include "tst_deps.moc"
