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
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <QFile>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include "mockhttpserver.h"
#include "ts/maths/angles.h"
#include "ts/network/networkclient.h"
#include "ts/orbits/earthframes.h"
#include "ts/orbits/elementsource.h"
#include "ts/orbits/orbitalelements.h"
#include "ts/orbits/orbitcomputer.h"
#include "ts/orbits/satellitedraws.h"
#include "ts/orbits/satellitelayer.h"
#include "ts/orbits/satellitepasses.h"
#include "ts/orbits/satellitetrack.h"
#include "ts/orbits/sgp4.h"

namespace
{

QByteArray readData(const char* name)
{
  QFile file(QStringLiteral(TS_ORBITS_DATA_DIR "/") + QLatin1String(name));
  if (!file.open(QIODevice::ReadOnly)) {
    return QByteArray();
  }
  return file.readAll();
}

/** One line of tcppver.out: time, position, velocity. */
struct ReferenceLine {
  double minutes = 0.0;
  ts::Vec3 positionKm;
  ts::Vec3 velocityKmS;
};

/** A satellite's lines in tcppver.out. */
struct ReferenceCase {
  int32_t catalogue = 0;
  std::vector<ReferenceLine> lines;
};

std::vector<ReferenceCase> readReference()
{
  std::vector<ReferenceCase> cases;
  const QStringList lines = QString::fromLatin1(readData("tcppver.out"))
                                .remove(QLatin1Char('\r'))
                                .split(QLatin1Char('\n'));
  for (const QString& line : lines) {
    const QStringList fields = line.simplified().split(QLatin1Char(' '));
    if (fields.size() == 2 && fields.at(1) == QLatin1String("xx")) {
      cases.push_back(ReferenceCase{fields.at(0).toInt(), {}});
      continue;
    }
    if (fields.size() < 7 || cases.empty()) {
      continue;
    }
    ReferenceLine ref;
    ref.minutes = fields.at(0).toDouble();
    ref.positionKm = {fields.at(1).toDouble(), fields.at(2).toDouble(),
                      fields.at(3).toDouble()};
    ref.velocityKmS = {fields.at(4).toDouble(), fields.at(5).toDouble(),
                       fields.at(6).toDouble()};
    cases.back().lines.push_back(ref);
  }
  return cases;
}

/** A verification TLE with its start, stop and step (minutes). */
struct VerificationCase {
  ts::OrbitalElements elements;
  double startMin = 0.0;
  double stopMin = 0.0;
  double stepMin = 0.0;
};

std::vector<VerificationCase> readVerificationTles()
{
  std::vector<VerificationCase> cases;
  const QStringList lines = QString::fromLatin1(readData("SGP4-VER.TLE"))
                                .remove(QLatin1Char('\r'))
                                .split(QLatin1Char('\n'));
  ts::TleOptions options;
  options.checkChecksums = false;
  for (int32_t i = 0; i + 1 < static_cast<int32_t>(lines.size()); ++i) {
    if (!lines.at(i).startsWith(QLatin1String("1 "))) {
      continue;
    }
    const QString& line2 = lines.at(i + 1);
    const std::optional<ts::OrbitalElements> elements =
        ts::parseTle(lines.at(i), line2.left(69), QString(), nullptr, options);
    if (!elements) {
      continue;
    }
    const QStringList times =
        line2.mid(69).simplified().split(QLatin1Char(' '));
    VerificationCase c;
    c.elements = *elements;
    c.startMin = times.value(0).toDouble();
    c.stopMin = times.value(1).toDouble();
    c.stepMin = times.value(2).toDouble();
    cases.push_back(c);
  }
  return cases;
}

/** What propagating one case gave: the lines, and the error it ended on. */
struct CaseOutput {
  std::vector<ReferenceLine> lines;
  /** The error that ended it, or None. */
  ts::Sgp4Error error = ts::Sgp4Error::None;
  /** True if the epoch itself gave no position. */
  bool failedAtEpoch = false;
};

ReferenceLine lineOf(double minutes, const ts::Sgp4Result& result)
{
  return ReferenceLine{minutes, result.teme.positionKm,
                       result.teme.velocityKmS};
}

/**
 * Runs a case as Vallado's testcpp does: the epoch, then each step from
 * the start to the stop (and the stop itself), stopping at an error.
 */
CaseOutput runCase(const VerificationCase& c)
{
  const ts::Sgp4 sgp4(c.elements);
  CaseOutput out;
  const ts::Sgp4Result atEpoch = sgp4.propagate(0.0);
  if (atEpoch.error != ts::Sgp4Error::None &&
      atEpoch.error != ts::Sgp4Error::Decayed) {
    out.error = atEpoch.error;
    out.failedAtEpoch = true;
    return out;
  }
  out.lines.push_back(lineOf(0.0, atEpoch));
  double t = c.startMin;
  while (t <= c.stopMin) {
    if (t != c.startMin || t != 0.0) {
      const ts::Sgp4Result r = sgp4.propagate(t);
      if (!r.ok()) {
        out.error = r.error;
        return out;
      }
      out.lines.push_back(lineOf(t, r));
    }
    t += c.stepMin;
  }
  if (t - c.stopMin < c.stepMin - 1e-6) {
    const ts::Sgp4Result r = sgp4.propagate(c.stopMin);
    if (!r.ok()) {
      out.error = r.error;
      return out;
    }
    out.lines.push_back(lineOf(c.stopMin, r));
  }
  return out;
}

double largest(const ts::Vec3& a, const ts::Vec3& b)
{
  return std::max(
      {std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

const char* const issLine1 =
    "1 25544U 98067A   26324.17237002  .00010040  00000+0  18678-3 0  9996";
const char* const issLine2 =
    "2 25544  51.6318 248.1735 0007426  45.4169 314.7424 15.49502307595936";

const char* const vanguardLine1 =
    "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
const char* const vanguardLine2 =
    "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";

ts::OrbitalElements tle(const char* line1, const char* line2)
{
  return ts::parseTle(QLatin1String(line1), QLatin1String(line2))
      .value_or(ts::OrbitalElements());
}

std::shared_ptr<const ts::Sgp4> issSgp4()
{
  return std::make_shared<const ts::Sgp4>(tle(issLine1, issLine2));
}

ts::JulianDate at(ts::JulianDate base, double minutes)
{
  ts::JulianDate time = base;
  time.fraction += minutes / 1440.0;
  return time;
}

ts::NetworkSettings testNetwork()
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.retryDelay = std::chrono::milliseconds(20);
  settings.attempts = 1;
  settings.timeout = std::chrono::milliseconds(5000);
  return settings;
}

const std::atomic<bool> notCancelled{false};

} // namespace

/** Tests of ts/orbits. */
class TestOrbits : public QObject
{
  Q_OBJECT

private slots:
  void verificationSet();
  void verificationErrors();
  void coverage();
  void errorCodes();
  void epochEdges();
  void gravityModels();
  void modes();
  void parseTle();
  void parseTleErrors();
  void threeLineForms();
  void alpha5();
  void parseOmm();
  void groupFile();
  void formatTle();
  void julianDates();
  void siderealTime();
  void temeToEcef();
  void satelliteStates();
  void timeSpans();
  void trackSpans();
  void trackReusesPoints();
  void antimeridian();
  void groundTrackSplits();
  void footprints();
  void lookAngles();
  void passes();
  void geostationaryPass();
  void orbitComputer();
  void celestrakUrls();
  void downloadAndCache();
  void downloadFailures();
  void satelliteSources();
  void satelliteStyles();
  void satelliteDraws();
};

void TestOrbits::verificationSet()
{
  const std::vector<VerificationCase> cases = readVerificationTles();
  const std::vector<ReferenceCase> reference = readReference();
  QCOMPARE(cases.size(), std::size_t{33});
  QCOMPARE(reference.size(), cases.size());
  int32_t compared = 0;
  double worstKm = 0.0;
  double worstKmS = 0.0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const ReferenceCase& ref = reference[i];
    QCOMPARE(cases[i].elements.catalogueNumber, ref.catalogue);
    const CaseOutput out = runCase(cases[i]);
    // A case that fails at its epoch has one line in the reference,
    // copied from the case before (testcpp prints what it had).
    if (out.failedAtEpoch) {
      QCOMPARE(ref.lines.size(), std::size_t{1});
      continue;
    }
    QVERIFY2(out.lines.size() == ref.lines.size(),
             qPrintable(QStringLiteral("satellite %1: %2 lines, %3 expected")
                            .arg(ref.catalogue)
                            .arg(out.lines.size())
                            .arg(ref.lines.size())));
    for (std::size_t j = 0; j < ref.lines.size(); ++j) {
      const ReferenceLine& e = ref.lines[j];
      const ReferenceLine& a = out.lines[j];
      QCOMPARE(a.minutes, e.minutes);
      const double km = largest(a.positionKm, e.positionKm);
      const double kmS = largest(a.velocityKmS, e.velocityKmS);
      worstKm = std::max(worstKm, km);
      worstKmS = std::max(worstKmS, kmS);
      QVERIFY2(km < 2e-7 && kmS < 2e-7,
               qPrintable(QStringLiteral("satellite %1 at %2 min: off by "
                                         "%3 km, %4 km/s")
                              .arg(ref.catalogue)
                              .arg(e.minutes)
                              .arg(km)
                              .arg(kmS)));
      ++compared;
    }
  }
  // Every line but the 33 headers and the one copied line.
  QCOMPARE(compared, 700 - 33 - 1);
  qInfo("Largest differences: %.3g km, %.3g km/s", worstKm, worstKmS);
}

void TestOrbits::verificationErrors()
{
  // The errors the test set ends on, in order (as python-sgp4 checks).
  std::vector<int32_t> errors;
  for (const VerificationCase& c : readVerificationTles()) {
    const CaseOutput out = runCase(c);
    if (out.error != ts::Sgp4Error::None) {
      errors.push_back(static_cast<int32_t>(out.error));
    }
  }
  QCOMPARE(errors, (std::vector<int32_t>{1, 1, 6, 6, 4, 3, 6}));
}

void TestOrbits::coverage()
{
  // The test set covers near-Earth and deep-space orbits, the 12- and
  // 24-hour resonances, high eccentricity, low perigees and inclinations
  // that go negative.
  std::vector<VerificationCase> cases = readVerificationTles();
  int32_t nearEarth = 0;
  int32_t deep = 0;
  int32_t halfDay = 0;
  int32_t oneDay = 0;
  int32_t eccentric = 0;
  int32_t lowPerigee = 0;
  for (const VerificationCase& c : cases) {
    const ts::Sgp4 sgp4(c.elements);
    const double n = sgp4.meanMotionRadPerMin();
    (sgp4.isDeepSpace() ? deep : nearEarth) += 1;
    halfDay += sgp4.isDeepSpace() && n >= 8.26e-3 && n <= 9.24e-3 &&
                       c.elements.eccentricity >= 0.5
                   ? 1
                   : 0;
    oneDay +=
        sgp4.isDeepSpace() && n > 0.0034906585 && n < 0.0052359877 ? 1 : 0;
    eccentric += c.elements.eccentricity > 0.7 ? 1 : 0;
    const double perigeeKm =
        std::pow(sgp4.meanMotionRadPerMin() / 0.0743669161, -2.0 / 3.0) *
            (1.0 - c.elements.eccentricity) * 6378.135 -
        6378.135;
    lowPerigee += perigeeKm < 156.0 ? 1 : 0;
  }
  qInfo("near-Earth %d, deep space %d, 12 h %d, 24 h %d, e > 0.7 %d, "
        "perigee < 156 km %d",
        nearEarth, deep, halfDay, oneDay, eccentric, lowPerigee);
  QVERIFY(nearEarth >= 8);
  QVERIFY(deep >= 20);
  QVERIFY(halfDay >= 3);
  QVERIFY(oneDay >= 3);
  QVERIFY(eccentric >= 3);
  QVERIFY(lowPerigee >= 3);

  // INTELSAT 902's inclination goes negative at 9313 minutes; the state
  // stays continuous across it.
  const VerificationCase* intelsat = nullptr;
  for (const VerificationCase& c : cases) {
    if (c.elements.catalogueNumber == 26975) {
      intelsat = &c;
    }
  }
  if (intelsat == nullptr) {
    QFAIL("INTELSAT 902 missing");
  }
  const ts::Sgp4 sgp4(intelsat->elements);
  const ts::Sgp4Result before = sgp4.propagate(9312.9);
  const ts::Sgp4Result at = sgp4.propagate(9313.0);
  const ts::Sgp4Result after = sgp4.propagate(9313.1);
  QVERIFY(before.ok() && at.ok() && after.ok());
  // The change of position over 12 s agrees with the velocity.
  const ts::Vec3 differenced =
      (after.teme.positionKm - before.teme.positionKm) / 12.0;
  QVERIFY(ts::length(differenced - at.teme.velocityKmS) < 1e-3);
}

void TestOrbits::errorCodes()
{
  // Each reference error is reported, not returned as a position.
  ts::OrbitalElements e = tle(issLine1, issLine2);
  e.eccentricity = 1.2;
  QCOMPARE(ts::Sgp4(e).epochError(), ts::Sgp4Error::MeanEccentricity);
  QVERIFY(!ts::Sgp4(e).propagate(10.0).ok());

  e = tle(issLine1, issLine2);
  e.meanMotionRevPerDay = 0.0;
  QCOMPARE(ts::Sgp4(e).epochError(), ts::Sgp4Error::MeanMotion);
  QCOMPARE(ts::Sgp4(e).propagate(10.0).error, ts::Sgp4Error::MeanMotion);

  // The cases of the test set that end on each error.
  const std::map<int32_t, ts::Sgp4Error> expected{
      {22312, ts::Sgp4Error::MeanEccentricity},
      {28350, ts::Sgp4Error::MeanEccentricity},
      {28872, ts::Sgp4Error::Decayed},
      {29141, ts::Sgp4Error::Decayed},
      {33333, ts::Sgp4Error::SemiLatusRectum},
      {33334, ts::Sgp4Error::PerturbedEccentricity}};
  for (const VerificationCase& c : readVerificationTles()) {
    const CaseOutput out = runCase(c);
    const std::map<int32_t, ts::Sgp4Error>::const_iterator it =
        expected.find(c.elements.catalogueNumber);
    if (it != expected.end()) {
      QCOMPARE(out.error, it->second);
      QVERIFY(!ts::sgp4ErrorText(out.error).isEmpty());
    }
  }
  // 33334 fails at its epoch: no position at any time.
  for (const VerificationCase& c : readVerificationTles()) {
    if (c.elements.catalogueNumber == 33334) {
      const ts::Sgp4 sgp4(c.elements);
      QCOMPARE(sgp4.epochError(), ts::Sgp4Error::PerturbedEccentricity);
      QVERIFY(!sgp4.propagate(100.0).ok());
    }
  }
  QVERIFY(ts::sgp4ErrorText(ts::Sgp4Error::None).isEmpty());
}

void TestOrbits::epochEdges()
{
  const ts::OrbitalElements iss = tle(issLine1, issLine2);
  const ts::Sgp4 sgp4(iss);
  QCOMPARE(sgp4.epochError(), ts::Sgp4Error::None);
  // At the epoch itself, through a time and through minutes.
  const ts::Sgp4Result atEpoch = sgp4.propagate(iss.epoch);
  const ts::Sgp4Result atZero = sgp4.propagate(0.0);
  QVERIFY(atEpoch.ok());
  QCOMPARE(atEpoch.teme.positionKm.x, atZero.teme.positionKm.x);
  const double radiusKm = ts::length(atZero.teme.positionKm);
  QVERIFY(radiusKm > 6700.0 && radiusKm < 6850.0);
  // Far before and after: a position or a reported error, never garbage.
  for (const double days : {-365.0, -30.0, 30.0, 365.0, 3650.0}) {
    const ts::Sgp4Result r = sgp4.propagate(days * 1440.0);
    if (r.ok()) {
      const double km = ts::length(r.teme.positionKm);
      QVERIFY(std::isfinite(km) && km > 6378.0);
    }
  }
  // A deep-space orbit years from its epoch (GPS).
  const ts::ElementSetList gps = ts::parseElementSets(readData("gps-ops.tle"));
  QVERIFY(!gps.elements.empty());
  const ts::Sgp4 navstar(gps.elements.front());
  QVERIFY(navstar.isDeepSpace());
  for (const double days : {-3650.0, -1.0, 1.0, 3650.0}) {
    const ts::Sgp4Result r = navstar.propagate(days * 1440.0);
    QVERIFY(r.ok());
    const double km = ts::length(r.teme.positionKm);
    QVERIFY2(km > 25000.0 && km < 28000.0, qPrintable(QString::number(km)));
  }

  // Across a year boundary: an epoch late on 31 December, propagated
  // into January, matches the same orbit with its epoch in the new year.
  ts::OrbitalElements late = iss;
  late.epoch = ts::JulianDate::fromDateTime(
      QDateTime(QDate(2025, 12, 31), QTime(23, 0), QTimeZone::utc()));
  const ts::Sgp4 lateSgp4(late);
  const QDateTime newYear(QDate(2026, 1, 1), QTime(1, 0), QTimeZone::utc());
  const ts::Sgp4Result a =
      lateSgp4.propagate(ts::JulianDate::fromDateTime(newYear));
  const ts::Sgp4Result b = lateSgp4.propagate(120.0);
  QVERIFY(a.ok());
  QVERIFY(largest(a.teme.positionKm, b.teme.positionKm) < 1e-9);
  const QStringList lines = ts::formatTle(late);
  QVERIFY2(lines.at(0).mid(18, 14) == QLatin1String("25365.95833333"),
           qPrintable(lines.at(0)));
}

void TestOrbits::gravityModels()
{
  // Vallado's C++ values for Vanguard 1 (as python-sgp4 checks them);
  // WGS 72 is the default.
  const ts::OrbitalElements v = tle(vanguardLine1, vanguardLine2);
  const double t = 309.67110720001529;
  struct Expected {
    ts::GravityModel model;
    ts::Vec3 positionKm;
  };
  const std::vector<Expected> expected{
      {ts::GravityModel::Wgs72Old,
       {-3754.251473242793, 7876.346815095482, 4719.220855042922}},
      {ts::GravityModel::Wgs72,
       {-3754.2514743216166, 7876.346817439062, 4719.220856478582}},
      {ts::GravityModel::Wgs84,
       {-3754.2437675772426, 7876.3549956188945, 4719.227897029576}}};
  for (const Expected& e : expected) {
    const ts::Sgp4Result r = ts::Sgp4(v, e.model).propagate(t);
    QVERIFY(r.ok());
    QVERIFY2(
        largest(r.teme.positionKm, e.positionKm) < 1e-8,
        qPrintable(QString::number(largest(r.teme.positionKm, e.positionKm))));
  }
  const ts::Sgp4Result byDefault = ts::Sgp4(v).propagate(t);
  QVERIFY(largest(byDefault.teme.positionKm, expected[1].positionKm) < 1e-8);
}

void TestOrbits::modes()
{
  // The operational mode's sidereal time differs a little from the
  // improved one; positions agree to metres near the epoch.
  const ts::OrbitalElements v = tle(vanguardLine1, vanguardLine2);
  const ts::Sgp4Result improved = ts::Sgp4(v).propagate(1440.0);
  const ts::Sgp4Result afspc =
      ts::Sgp4(v, ts::GravityModel::Wgs72, ts::Sgp4Mode::Afspc)
          .propagate(1440.0);
  QVERIFY(improved.ok() && afspc.ok());
  QVERIFY(largest(improved.teme.positionKm, afspc.teme.positionKm) < 1e-3);
}

void TestOrbits::parseTle()
{
  QString error;
  const std::optional<ts::OrbitalElements> e =
      ts::parseTle(QLatin1String(issLine1), QLatin1String(issLine2),
                   QStringLiteral("ISS (ZARYA)             "), &error);
  if (!e) {
    QFAIL(qPrintable(error));
  }
  QCOMPARE(e->name, QStringLiteral("ISS (ZARYA)"));
  QCOMPARE(e->catalogueNumber, 25544);
  QCOMPARE(e->classification, 'U');
  QCOMPARE(e->internationalDesignator, QStringLiteral("1998-067A"));
  QCOMPARE(e->epochTime(), QDateTime(QDate(2026, 11, 20), QTime(4, 8, 12, 770),
                                     QTimeZone::utc()));
  QCOMPARE(e->meanMotionDot, 0.00010040);
  QCOMPARE(e->meanMotionDdot, 0.0);
  QVERIFY(std::fabs(e->bstar - 0.18678e-3) < 1e-15);
  QCOMPARE(e->inclinationDeg, 51.6318);
  QCOMPARE(e->raanDeg, 248.1735);
  QCOMPARE(e->eccentricity, 0.0007426);
  QCOMPARE(e->argOfPerigeeDeg, 45.4169);
  QCOMPARE(e->meanAnomalyDeg, 314.7424);
  QCOMPARE(e->meanMotionRevPerDay, 15.49502307);
  QCOMPARE(e->revolutionNumber, 59593);
  QCOMPARE(e->elementSetNumber, 999);
  QVERIFY(std::fabs(e->periodMinutes() - 92.93) < 0.01);
  QCOMPARE(e->displayName(), QStringLiteral("ISS (ZARYA)"));
  QVERIFY(
      std::fabs(e->ageDays(QDateTime(QDate(2026, 11, 21), QTime(4, 8, 12, 770),
                                     QTimeZone::utc())) -
                1.0) < 1e-6);
  // Negative values in the exponent fields and ndot.
  const ts::OrbitalElements v = tle(
      "1 04632U 70093B   04031.91070959 -.00000084  00000-0  10000-3 0  9955",
      "2 04632  11.4628 273.1101 1450506 207.6000 143.9350  1.20231981 44145");
  QCOMPARE(v.meanMotionDot, -0.00000084);
  QVERIFY(std::fabs(v.bstar - 1e-4) < 1e-18);
  QCOMPARE(v.internationalDesignator, QStringLiteral("1970-093B"));
  const ts::OrbitalElements d = tle(
      "1 29238U 06022G   06177.28732010  .00766286  10823-4  13334-2 0   101",
      "2 29238  51.5595 213.7903 0202579  95.2503 267.9010 15.73823839  1061");
  QVERIFY(std::fabs(d.meanMotionDdot - 0.10823e-4) < 1e-18);
  QVERIFY(std::fabs(d.bstar - 0.13334e-2) < 1e-16);
}

void TestOrbits::parseTleErrors()
{
  const QString good1 = QLatin1String(issLine1);
  const QString good2 = QLatin1String(issLine2);
  QString error;
  // Bad checksum.
  QString bad = good1;
  bad[68] = QLatin1Char('7');
  QVERIFY(!ts::parseTle(bad, good2, QString(), &error));
  QVERIFY(error.contains(QLatin1String("checksum")));
  // Accepted when checksums aren't checked.
  ts::TleOptions lax;
  lax.checkChecksums = false;
  QVERIFY(ts::parseTle(bad, good2, QString(), nullptr, lax));
  // A field moved by a column.
  QVERIFY(!ts::parseTle(QLatin1Char(' ') + good1.left(68), good2, QString(),
                        &error));
  QVERIFY(!ts::parseTle(good1,
                        good2.left(10) + QLatin1Char(' ') + good2.mid(10, 58),
                        QString(), &error, lax));
  QVERIFY(error.contains(QLatin1String("column")));
  // Too short.
  QVERIFY(!ts::parseTle(good1.left(50), good2, QString(), &error));
  QVERIFY(error.contains(QLatin1String("short")));
  // Lines swapped.
  QVERIFY(!ts::parseTle(good2, good1, QString(), &error));
  // Catalogue numbers that differ.
  QString other = good2;
  other.replace(2, 5, QStringLiteral("25545"));
  QVERIFY(!ts::parseTle(good1, other, QString(), &error, lax));
  QVERIFY(error.contains(QLatin1String("catalogue")));
  // A letter where a number should be.
  QString letters = good2;
  letters[9] = QLatin1Char('x');
  QVERIFY(!ts::parseTle(good1, letters, QString(), &error, lax));
  // Characters outside ASCII.
  QString wide = good1;
  wide[30] = QChar(0x00a0);
  QVERIFY(!ts::parseTle(wide, good2, QString(), &error, lax));
}

void TestOrbits::threeLineForms()
{
  const QString text =
      QStringLiteral("# comment\n"
                     "0 ISS (ZARYA)\n%1\n%2\n\n"
                     "VANGUARD 1\r\n%3\r\n%4\r\n"
                     "%1\n%2\n"
                     "2 00005 orphan\n"
                     "1 00005 orphan\n")
          .arg(QLatin1String(issLine1), QLatin1String(issLine2),
               QLatin1String(vanguardLine1), QLatin1String(vanguardLine2));
  const ts::ElementSetList list = ts::parseTleText(text);
  QCOMPARE(list.elements.size(), std::size_t{3});
  QCOMPARE(list.elements[0].name, QStringLiteral("ISS (ZARYA)"));
  QCOMPARE(list.elements[1].name, QStringLiteral("VANGUARD 1"));
  QCOMPARE(list.elements[1].catalogueNumber, 5);
  // Two-line form: no name; the catalogue number stands in.
  QVERIFY(list.elements[2].name.isEmpty());
  QCOMPARE(list.elements[2].displayName(), QStringLiteral("25544"));
  QCOMPARE(list.errors.size(), 2);
}

void TestOrbits::alpha5()
{
  QCOMPARE(ts::parseCatalogueNumber(QStringLiteral("25544")),
           std::optional<int32_t>(25544));
  QCOMPARE(ts::parseCatalogueNumber(QStringLiteral("    5")),
           std::optional<int32_t>(5));
  QCOMPARE(ts::parseCatalogueNumber(QStringLiteral("A0000")),
           std::optional<int32_t>(100000));
  QCOMPARE(ts::parseCatalogueNumber(QStringLiteral("J2931")),
           std::optional<int32_t>(182931));
  QCOMPARE(ts::parseCatalogueNumber(QStringLiteral("P1234")),
           std::optional<int32_t>(231234));
  QCOMPARE(ts::parseCatalogueNumber(QStringLiteral("Z9999")),
           std::optional<int32_t>(339999));
  QVERIFY(!ts::parseCatalogueNumber(QStringLiteral("I0000")));
  QVERIFY(!ts::parseCatalogueNumber(QStringLiteral("O0000")));
  QVERIFY(!ts::parseCatalogueNumber(QStringLiteral("12a45")));
  // Round trip through the writer.
  ts::OrbitalElements e = tle(issLine1, issLine2);
  for (const int32_t n : {5, 99999, 100000, 182931, 231234, 339999}) {
    e.catalogueNumber = n;
    const QStringList lines = ts::formatTle(e);
    const std::optional<ts::OrbitalElements> back =
        ts::parseTle(lines.at(0), lines.at(1));
    if (!back) {
      QFAIL("not read back");
    }
    QCOMPARE(back->catalogueNumber, n);
  }
}

void TestOrbits::parseOmm()
{
  const ts::ElementSetList list = ts::parseOmmJson(readData("stations.json"));
  QVERIFY(list.errors.isEmpty());
  QVERIFY(!list.elements.empty());
  const ts::OrbitalElements& iss = list.elements.front();
  QCOMPARE(iss.name, QStringLiteral("ISS (ZARYA)"));
  QCOMPARE(iss.catalogueNumber, 25544);
  QCOMPARE(iss.internationalDesignator, QStringLiteral("1998-067A"));
  QCOMPARE(iss.epochTime(), QDateTime(QDate(2026, 11, 20), QTime(4, 8, 12, 770),
                                      QTimeZone::utc()));
  QCOMPARE(iss.meanMotionRevPerDay, 15.49502307);
  QCOMPARE(iss.bstar, 0.00018678039);
  // Space-Track writes the numbers as strings.
  QJsonObject strings;
  strings.insert(QStringLiteral("OBJECT_NAME"), QStringLiteral("TEST"));
  strings.insert(QStringLiteral("NORAD_CAT_ID"), QStringLiteral("43013"));
  strings.insert(QStringLiteral("EPOCH"),
                 QStringLiteral("2026-01-02T03:04:05.500000Z"));
  for (const char* key :
       {"MEAN_MOTION", "ECCENTRICITY", "INCLINATION", "RA_OF_ASC_NODE",
        "ARG_OF_PERICENTER", "MEAN_ANOMALY"}) {
    strings.insert(QLatin1String(key), QStringLiteral("0.5"));
  }
  QString error;
  const std::optional<ts::OrbitalElements> s = ts::parseOmm(strings, &error);
  if (!s) {
    QFAIL(qPrintable(error));
  }
  QCOMPARE(s->catalogueNumber, 43013);
  QCOMPARE(s->epochTime(),
           QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5, 500), QTimeZone::utc()));
  strings.remove(QStringLiteral("EPOCH"));
  QVERIFY(!ts::parseOmm(strings, &error));
  QVERIFY(!error.isEmpty());
  // A single object, and something that isn't JSON.
  QCOMPARE(ts::parseOmmJson(QJsonDocument(strings).toJson()).errors.size(), 1);
  QCOMPARE(ts::parseOmmJson("[nonsense").errors.size(), 1);
}

void TestOrbits::groupFile()
{
  // A whole CelesTrak group, as TLEs and as OMM JSON: the same satellites,
  // at the same places.
  const ts::ElementSetList tles =
      ts::parseElementSets(readData("stations.tle"));
  const ts::ElementSetList omms =
      ts::parseElementSets(readData("stations.json"));
  QVERIFY(tles.errors.isEmpty());
  QVERIFY(omms.errors.isEmpty());
  QVERIFY(tles.elements.size() >= 10);
  // The TLE form leaves out what has a six-digit catalogue number, which
  // only OMM can write.
  std::size_t sixDigits = 0;
  for (const ts::OrbitalElements& b : omms.elements) {
    sixDigits += b.catalogueNumber > 99999 ? 1 : 0;
  }
  QCOMPARE(tles.elements.size() + sixDigits, omms.elements.size());
  for (std::size_t i = 0; i < tles.elements.size(); ++i) {
    const ts::OrbitalElements& a = tles.elements[i];
    const ts::OrbitalElements& b = omms.elements[i];
    QCOMPARE(a.catalogueNumber, b.catalogueNumber);
    QCOMPARE(a.name, b.name);
    // TLE epochs have 8 decimals of a day (under a millisecond).
    QVERIFY(std::fabs(a.epoch.minutesSince(b.epoch)) < 1e-4);
    const ts::Sgp4Result ra = ts::Sgp4(a).propagate(b.epoch);
    const ts::Sgp4Result rb = ts::Sgp4(b).propagate(b.epoch);
    if (ra.ok() && rb.ok()) {
      QVERIFY2(largest(ra.teme.positionKm, rb.teme.positionKm) < 0.01,
               qPrintable(a.name));
    }
  }
}

void TestOrbits::formatTle()
{
  // TLEs written back are the lines read (fields at their precision).
  const ts::ElementSetList list =
      ts::parseElementSets(readData("stations.tle"));
  const QStringList lines =
      QString::fromLatin1(readData("stations.tle")).split(QLatin1Char('\n'));
  int32_t checked = 0;
  for (const ts::OrbitalElements& e : list.elements) {
    const QStringList written = ts::formatTle(e);
    QVERIFY(lines.contains(written.at(0) + QLatin1Char('\r')) ||
            lines.contains(written.at(0)));
    QVERIFY(lines.contains(written.at(1) + QLatin1Char('\r')) ||
            lines.contains(written.at(1)));
    ++checked;
  }
  QVERIFY(checked > 0);
  // Zero is written "00000+0" (CelesTrak) or "00000-0" (Vallado's files):
  // the same value.
  const ts::OrbitalElements vanguard = tle(vanguardLine1, vanguardLine2);
  const QStringList v = ts::formatTle(vanguard);
  QCOMPARE(v.at(0).left(44), QLatin1String(vanguardLine1).left(44));
  QCOMPARE(v.at(0).mid(52, 16), QLatin1String(vanguardLine1).mid(52, 16));
  QCOMPARE(v.at(1), QLatin1String(vanguardLine2));
  QCOMPARE(ts::parseTle(v.at(0), v.at(1)).value_or(ts::OrbitalElements()),
           vanguard);
  QCOMPARE(ts::tleChecksum(QLatin1String(vanguardLine1)), 3);
}

void TestOrbits::julianDates()
{
  const QDateTime j2000(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::utc());
  const ts::JulianDate jd = ts::JulianDate::fromDateTime(j2000);
  QCOMPARE(jd.day, 2451544.5);
  QCOMPARE(jd.fraction, 0.5);
  QCOMPARE(jd.toDateTime(), j2000);
  const QDateTime before(QDate(1969, 7, 20), QTime(20, 17, 40, 123),
                         QTimeZone::utc());
  const ts::JulianDate old = ts::JulianDate::fromDateTime(before);
  QVERIFY(old.fraction >= 0.0 && old.fraction < 1.0);
  QCOMPARE(old.toDateTime(), before);
  QCOMPARE(jd.minutesSince(ts::JulianDate::fromDateTime(j2000.addSecs(-60))),
           1.0);
  // TLE epochs: day 1.0 is 1 January 0h; leap years count day 366.
  QCOMPARE(tle(vanguardLine1, vanguardLine2).epoch.day, 2451722.5);
  ts::TleOptions lax;
  lax.checkChecksums = false;
  const QString leap = QLatin1String(issLine1).left(18) +
                       QStringLiteral("24366.50000000") +
                       QLatin1String(issLine1).mid(32);
  const std::optional<ts::OrbitalElements> e =
      ts::parseTle(leap, QLatin1String(issLine2), QString(), nullptr, lax);
  if (!e) {
    QFAIL("leap-year epoch not read");
  }
  QCOMPARE(e->epochTime(),
           QDateTime(QDate(2024, 12, 31), QTime(12, 0), QTimeZone::utc()));
  // Two-digit years: 57–99 are 19xx.
  const QString fifties = QLatin1String(issLine1).left(18) +
                          QStringLiteral("57001.00000000") +
                          QLatin1String(issLine1).mid(32);
  const std::optional<ts::OrbitalElements> fiftySeven =
      ts::parseTle(fifties, QLatin1String(issLine2), QString(), nullptr, lax);
  if (!fiftySeven) {
    QFAIL("1957 epoch not read");
  }
  QCOMPARE(fiftySeven->epochTime().date(), QDate(1957, 1, 1));
}

void TestOrbits::siderealTime()
{
  // Vallado, Fundamentals of Astrodynamics and Applications, example 3-5:
  // 1992 August 20, 12:14 UT1, GMST 152.578787810° (to the book's nine
  // decimals, from a Julian date it rounds to six).
  const ts::JulianDate jd = ts::JulianDate::fromDateTime(
      QDateTime(QDate(1992, 8, 20), QTime(12, 14), QTimeZone::utc()));
  QVERIFY(std::fabs(ts::radToDeg(ts::greenwichSiderealRad(jd)) -
                    152.578787810) < 1e-7);
  QVERIFY(std::fabs(ts::radToDeg(ts::greenwichSiderealRad(jd.value())) -
                    152.578787810) < 1e-6);
  // In [0, 2π) everywhere.
  for (int32_t i = 0; i < 25; ++i) {
    const double g = ts::greenwichSiderealRad(2440000.5 + 1234.567 * i);
    QVERIFY(g >= 0.0 && g < 2.0 * ts::pi);
  }
}

void TestOrbits::temeToEcef()
{
  // Vallado et al. 2006, the TEME example: 2004 April 6, 07:51:28.386009
  // UTC, UT1 − UTC = −0.4399619 s. TEME to PEF (no polar motion, which
  // the paper's ITRF values add, some 10 m). The paper turns the velocity
  // with the day's length (LOD 1.5563 ms), 10⁻⁵ m/s from ours.
  const QDateTime utc(QDate(2004, 4, 6), QTime(7, 51, 28, 386),
                      QTimeZone::utc());
  ts::JulianDate ut1 = ts::JulianDate::fromDateTime(utc);
  ut1.fraction += (0.000009 - 0.4399619) / 86400.0;
  ts::TemeState teme;
  teme.positionKm = {5094.18016210, 6127.64465950, 6380.34453270};
  teme.velocityKmS = {-4.746131487, 0.785818041, 5.531931288};
  const ts::EcefState pef = ts::temeToEcef(teme, ts::greenwichSiderealRad(ut1));
  const ts::Vec3 expectedM =
      ts::Vec3{-1033.47503130, 7901.30558560, 6380.34453270} * 1000.0;
  const ts::Vec3 expectedMS =
      ts::Vec3{-3.225632747, -2.872442511, 5.531931288} * 1000.0;
  QVERIFY2(largest(pef.positionM, expectedM) < 0.01,
           qPrintable(QString::number(largest(pef.positionM, expectedM))));
  QVERIFY2(largest(pef.velocityMS, expectedMS) < 1e-4,
           qPrintable(QString::number(largest(pef.velocityMS, expectedMS))));
  // And back.
  const ts::Vec3 back =
      ts::ecefToTemeKm(pef.positionM, ts::greenwichSiderealRad(ut1));
  QVERIFY(largest(back, teme.positionKm) < 1e-9);
}

void TestOrbits::satelliteStates()
{
  const std::shared_ptr<const ts::Sgp4> iss = issSgp4();
  const ts::SatelliteState state =
      ts::satelliteState(*iss, iss->elements().epoch);
  QVERIFY(state.ok());
  // The ISS: about 420 km up, 7.66 km/s, within its inclination.
  QVERIFY(state.geodetic.heightM > 380e3 && state.geodetic.heightM < 450e3);
  QVERIFY(std::fabs(state.geodetic.latDeg) <= 51.7);
  QVERIFY(std::fabs(state.speedKmS - 7.66) < 0.05);
  // Earth-fixed speed is less (the ground turns under it eastwards).
  QVERIFY(ts::length(state.velocityEcefMS) / 1000.0 < state.speedKmS);
  QVERIFY(std::fabs(ts::length(state.positionEcefM) -
                    ts::length(ts::geodeticToEcef(state.geodetic))) < 1e-3);
  ts::OrbitalElements bad = iss->elements();
  bad.eccentricity = 2.0;
  QVERIFY(!ts::satelliteState(ts::Sgp4(bad), bad.epoch).ok());
}

void TestOrbits::timeSpans()
{
  const ts::TimeSpan orbits{1.5, ts::SpanUnit::Orbits};
  QCOMPARE(orbits.minutes(90.0), 135.0);
  const ts::TimeSpan minutes{45.0, ts::SpanUnit::Minutes};
  QCOMPARE(minutes.minutes(90.0), 45.0);
  QCOMPARE(ts::TimeSpan::fromJson(orbits.toJson()),
           std::optional<ts::TimeSpan>(orbits));
  QCOMPARE(ts::TimeSpan::fromJson(minutes.toJson()),
           std::optional<ts::TimeSpan>(minutes));
  QJsonObject bad = minutes.toJson();
  bad.insert(QStringLiteral("unit"), QStringLiteral("fortnights"));
  QVERIFY(!ts::TimeSpan::fromJson(bad));
  bad = minutes.toJson();
  bad.insert(QStringLiteral("amount"), -1.0);
  QVERIFY(!ts::TimeSpan::fromJson(bad));
  QCOMPARE(ts::trackStepMinutes(92.9), 92.9 / 120.0);
  QCOMPARE(ts::trackStepMinutes(10.0), 0.25);
  QCOMPARE(ts::trackStepMinutes(100000.0), 30.0);
}

void TestOrbits::trackSpans()
{
  // The past and future parts cover exactly the chosen times.
  const std::shared_ptr<const ts::Sgp4> iss = issSgp4();
  ts::TrackSampler sampler(iss);
  const ts::JulianDate now = at(iss->elements().epoch, 123.456);
  const double period = iss->periodMinutes();
  const ts::SatelliteTrack track = sampler.track(now, period, 30.0);
  QVERIFY(track.past.size() > 100);
  QVERIFY(track.future.size() > 30);
  QVERIFY(std::fabs(track.past.front().minutes + period) < 1e-6);
  QVERIFY(std::fabs(track.past.back().minutes) < 1e-9);
  QVERIFY(std::fabs(track.future.front().minutes) < 1e-9);
  QVERIFY(std::fabs(track.future.back().minutes - 30.0) < 1e-6);
  for (const std::vector<ts::TrackPoint>* part : {&track.past, &track.future}) {
    for (std::size_t i = 1; i < part->size(); ++i) {
      const double gap = (*part)[i].minutes - (*part)[i - 1].minutes;
      QVERIFY(gap > 0.0 && gap <= sampler.stepMinutes() + 1e-9);
    }
  }
  // The time's point is where the satellite is then.
  const ts::SatelliteState state = ts::satelliteState(*iss, now);
  QVERIFY(ts::distance(track.past.back().positionEcefM, state.positionEcefM) <
          1.0);
  // One orbit back ends near where it started (the Earth turned under it,
  // so the ground track doesn't).
  const double apart = ts::distance(track.past.front().positionEcefM,
                                    track.past.back().positionEcefM);
  QVERIFY2(apart > 1000e3 && apart < 3000e3,
           qPrintable(QString::number(apart)));
  // No spans: just the point.
  const ts::SatelliteTrack none = sampler.track(now, 0.0, 0.0);
  QCOMPARE(none.past.size(), std::size_t{1});
  QCOMPARE(none.future.size(), std::size_t{1});
}

void TestOrbits::trackReusesPoints()
{
  // Moving the clock computes only the points that come into the spans.
  const std::shared_ptr<const ts::Sgp4> iss = issSgp4();
  ts::TrackSampler sampler(iss);
  const double step = sampler.stepMinutes();
  ts::JulianDate now = at(iss->elements().epoch, 10.0);
  sampler.track(now, 60.0, 60.0);
  const int64_t first = sampler.gridPointsComputed();
  QVERIFY(first > 100);
  sampler.track(now, 60.0, 60.0);
  QCOMPARE(sampler.gridPointsComputed(), first);
  // A fraction of a step: at most one new point at the front.
  now = at(now, 0.4 * step);
  sampler.track(now, 60.0, 60.0);
  QVERIFY(sampler.gridPointsComputed() - first <= 1);
  // Ten steps on: about ten new points.
  const int64_t before = sampler.gridPointsComputed();
  now = at(now, 10.0 * step);
  const ts::SatelliteTrack moved = sampler.track(now, 60.0, 60.0);
  const int64_t added = sampler.gridPointsComputed() - before;
  QVERIFY2(added >= 9 && added <= 11, qPrintable(QString::number(added)));
  QVERIFY(std::fabs(moved.future.back().minutes - 60.0) < 1e-6);
  // A longer future span adds what it newly covers.
  const int64_t spanBefore = sampler.gridPointsComputed();
  const ts::SatelliteTrack longer = sampler.track(now, 60.0, 120.0);
  QVERIFY(std::fabs(longer.future.back().minutes - 120.0) < 1e-6);
  const int64_t spanAdded = sampler.gridPointsComputed() - spanBefore;
  QVERIFY(std::fabs(static_cast<double>(spanAdded) - 60.0 / step) <= 1.5);
  // The same points as computed from scratch.
  ts::TrackSampler fresh(iss);
  const ts::SatelliteTrack again = fresh.track(now, 60.0, 120.0);
  QCOMPARE(again.past.size(), longer.past.size());
  QCOMPARE(again.future.size(), longer.future.size());
  for (std::size_t i = 0; i < again.future.size(); ++i) {
    QVERIFY(ts::distance(again.future[i].positionEcefM,
                         longer.future[i].positionEcefM) < 1e-6);
  }
}

void TestOrbits::antimeridian()
{
  // Eastwards across: ends on +180, starts again on −180.
  const std::vector<std::vector<ts::LonLat>> east = ts::splitAtAntimeridian(
      {{170.0, 0.0}, {178.0, 2.0}, {-178.0, 4.0}, {-170.0, 6.0}});
  QCOMPARE(east.size(), std::size_t{2});
  QCOMPARE(east[0].back().lonDeg, 180.0);
  QCOMPARE(east[1].front().lonDeg, -180.0);
  QCOMPARE(east[0].back().latDeg, 3.0);
  QCOMPARE(east[1].front().latDeg, 3.0);
  QCOMPARE(east[0].size(), std::size_t{3});
  QCOMPARE(east[1].size(), std::size_t{3});
  // Westwards.
  const std::vector<std::vector<ts::LonLat>> west =
      ts::splitAtAntimeridian({{-175.0, 10.0}, {175.0, 20.0}});
  QCOMPARE(west.size(), std::size_t{2});
  QCOMPARE(west[0].back().lonDeg, -180.0);
  QCOMPARE(west[1].front().lonDeg, 180.0);
  QCOMPARE(west[0].back().latDeg, 15.0);
  // Nothing to split.
  QCOMPARE(ts::splitAtAntimeridian({{0.0, 0.0}, {10.0, 10.0}}).size(),
           std::size_t{1});
  QVERIFY(ts::splitAtAntimeridian({}).empty());
}

void TestOrbits::groundTrackSplits()
{
  // Two orbits of the ISS cross the antimeridian at least once; every
  // piece stays within ±180° and has no step wider than a few degrees.
  const std::shared_ptr<const ts::Sgp4> iss = issSgp4();
  ts::TrackSampler sampler(iss);
  const ts::SatelliteTrack track =
      sampler.track(iss->elements().epoch, 0.0, 2.0 * iss->periodMinutes());
  std::vector<ts::LonLat> ground;
  ground.reserve(track.future.size());
  for (const ts::TrackPoint& p : track.future) {
    ground.push_back(ts::LonLat{p.geodetic.lonDeg, p.geodetic.latDeg});
  }
  const std::vector<std::vector<ts::LonLat>> pieces =
      ts::splitAtAntimeridian(ground);
  QVERIFY(pieces.size() >= 2);
  std::size_t points = 0;
  for (const std::vector<ts::LonLat>& piece : pieces) {
    points += piece.size();
    for (std::size_t i = 0; i < piece.size(); ++i) {
      QVERIFY(piece[i].lonDeg >= -180.0 && piece[i].lonDeg <= 180.0);
      if (i > 0) {
        QVERIFY(std::fabs(piece[i].lonDeg - piece[i - 1].lonDeg) < 10.0);
      }
    }
  }
  QCOMPARE(points, ground.size() + 2 * (pieces.size() - 1));
}

void TestOrbits::footprints()
{
  // 420 km up, seen down to the horizon: acos(R / (R + h)).
  const double r = 6371008.8;
  QVERIFY(std::fabs(ts::footprintRadiusRad(420e3, 0.0) -
                    std::acos(r / (r + 420e3))) < 1e-12);
  // A higher minimum elevation shrinks it; geostationary sees a lot.
  QVERIFY(ts::footprintRadiusRad(420e3, 10.0) <
          ts::footprintRadiusRad(420e3, 0.0));
  QVERIFY(ts::radToDeg(ts::footprintRadiusRad(35786e3, 0.0)) > 81.0);
  QCOMPARE(ts::footprintRadiusRad(420e3, 90.0) <= 1e-3, true);
  // Every point of the ring is that far from the point below.
  const ts::Geodetic below{40.0, 170.0, 420e3};
  const std::vector<ts::LonLat> ring = ts::footprint(below, 5.0, 72);
  QCOMPARE(ring.size(), std::size_t{72});
  const double radius = ts::footprintRadiusRad(420e3, 5.0);
  for (const ts::LonLat& p : ring) {
    const double lat1 = ts::degToRad(below.latDeg);
    const double lat2 = ts::degToRad(p.latDeg);
    const double dlon = ts::degToRad(p.lonDeg - below.lonDeg);
    const double central =
        std::acos(std::sin(lat1) * std::sin(lat2) +
                  std::cos(lat1) * std::cos(lat2) * std::cos(dlon));
    QVERIFY(std::fabs(central - radius) < 1e-9);
    QVERIFY(p.lonDeg >= -180.0 && p.lonDeg < 180.0);
  }
  // Over a pole the ring goes all the way round in longitude.
  const std::vector<ts::LonLat> polar =
      ts::footprint(ts::Geodetic{89.0, 0.0, 800e3}, 0.0, 90);
  double west = 180.0;
  double east = -180.0;
  for (const ts::LonLat& p : polar) {
    west = std::min(west, p.lonDeg);
    east = std::max(east, p.lonDeg);
  }
  QVERIFY(east - west > 350.0);
}

void TestOrbits::lookAngles()
{
  const ts::Geodetic observer{0.0, 0.0, 0.0};
  // Straight up.
  const ts::LookAngles up = ts::lookAngles(
      observer, ts::geodeticToEcef(ts::Geodetic{0.0, 0.0, 500e3}));
  QVERIFY(std::fabs(up.elevationDeg - 90.0) < 1e-9);
  QVERIFY(std::fabs(up.rangeKm - 500.0) < 1e-6);
  // Due east and due north, on the horizon's plane.
  const ts::Vec3 here = ts::geodeticToEcef(observer);
  const ts::LookAngles east =
      ts::lookAngles(observer, here + ts::Vec3{0.0, 1000e3, 0.0});
  QVERIFY(std::fabs(east.azimuthDeg - 90.0) < 1e-9);
  QVERIFY(std::fabs(east.elevationDeg) < 1e-9);
  const ts::LookAngles north =
      ts::lookAngles(observer, here + ts::Vec3{0.0, 0.0, 1000e3});
  QVERIFY(north.azimuthDeg < 1e-9 || north.azimuthDeg > 360.0 - 1e-9);
  const ts::LookAngles west =
      ts::lookAngles(observer, here + ts::Vec3{10e3, -1000e3, 0.0});
  QVERIFY(std::fabs(west.azimuthDeg - 270.0) < 1e-6);
  QVERIFY(west.elevationDeg > 0.0);
  // Below the horizon.
  QVERIFY(ts::lookAngles(observer, ts::Vec3{0.0, 0.0, 0.0}).elevationDeg <
          -89.0);
}

void TestOrbits::passes()
{
  // The ISS over London: the next pass rises and sets at the minimum
  // elevation, peaks between, and lasts minutes, not hours.
  const std::shared_ptr<const ts::Sgp4> iss = issSgp4();
  const ts::Geodetic london{51.5, -0.13, 20.0};
  const ts::JulianDate from = iss->elements().epoch;
  const std::optional<ts::SatellitePass> pass =
      ts::nextPass(*iss, london, from, 10.0);
  if (!pass) {
    QFAIL("no pass");
  }
  QVERIFY(!pass->alwaysUp);
  const double riseMin = pass->rise.minutesSince(from);
  const double setMin = pass->set.minutesSince(from);
  const double topMin = pass->culmination.minutesSince(from);
  QVERIFY(riseMin >= 0.0 && riseMin < 3.0 * 1440.0);
  QVERIFY(setMin - riseMin > 0.5 && setMin - riseMin < 12.0);
  QVERIFY(topMin > riseMin && topMin < setMin);
  QVERIFY(pass->maxElevationDeg >= 10.0 && pass->maxElevationDeg <= 90.0);
  const std::function<double(ts::JulianDate)> elevationAt =
      [&](ts::JulianDate time) {
        return ts::lookAngles(london,
                              ts::satelliteState(*iss, time).positionEcefM)
            .elevationDeg;
      };
  QVERIFY(std::fabs(elevationAt(pass->rise) - 10.0) < 0.1);
  QVERIFY(std::fabs(elevationAt(pass->set) - 10.0) < 0.1);
  QVERIFY(std::fabs(elevationAt(pass->culmination) - pass->maxElevationDeg) <
          1e-6);
  // A minute either side of the top is lower.
  QVERIFY(elevationAt(at(pass->culmination, -1.0)) < pass->maxElevationDeg);
  QVERIFY(elevationAt(at(pass->culmination, 1.0)) < pass->maxElevationDeg);
  // Searching from the middle of the pass finds the same pass, under way.
  const std::optional<ts::SatellitePass> during =
      ts::nextPass(*iss, london, pass->culmination, 10.0);
  if (!during) {
    QFAIL("no pass under way");
  }
  QVERIFY(during->inProgress);
  QVERIFY(std::fabs(during->rise.minutesSince(pass->rise)) < 1.0 / 30.0);
  QVERIFY(std::fabs(during->set.minutesSince(pass->set)) < 1.0 / 30.0);
  // Never seen from deep in the south at 51.6° inclination, high enough.
  QVERIFY(!ts::nextPass(*iss, ts::Geodetic{-89.0, 0.0, 0.0}, from, 10.0, 1.0));
}

void TestOrbits::geostationaryPass()
{
  // A geostationary satellite (the test set's 28626) seen from under it
  // never sets.
  for (const VerificationCase& c : readVerificationTles()) {
    if (c.elements.catalogueNumber != 28626) {
      continue;
    }
    const ts::Sgp4 geo(c.elements);
    const ts::SatelliteState state = ts::satelliteState(geo, c.elements.epoch);
    QVERIFY(state.ok());
    const ts::Geodetic below{state.geodetic.latDeg, state.geodetic.lonDeg, 0.0};
    const std::optional<ts::SatellitePass> pass =
        ts::nextPass(geo, below, c.elements.epoch, 10.0, 1.0);
    if (!pass) {
      QFAIL("no geostationary pass");
    }
    QVERIFY(pass->alwaysUp);
    QVERIFY(pass->inProgress);
    QVERIFY(pass->maxElevationDeg > 80.0);
  }
}

void TestOrbits::orbitComputer()
{
  // Positions and tracks off the thread, the latest request winning.
  const ts::ElementSetList stations =
      ts::parseElementSets(readData("stations.json"));
  std::vector<std::shared_ptr<const ts::Sgp4>> satellites;
  satellites.reserve(stations.elements.size());
  for (const ts::OrbitalElements& e : stations.elements) {
    satellites.push_back(std::make_shared<const ts::Sgp4>(e));
  }
  ts::OrbitRequest request;
  request.generation = 7;
  request.time = satellites.front()->elements().epoch;
  for (std::size_t i = 0; i < satellites.size(); ++i) {
    request.tasks.push_back(ts::OrbitTask{satellites[i], i == 0, 90.0, 45.0});
  }
  ts::OrbitComputer computer;
  QSignalSpy ready(&computer, &ts::OrbitComputer::resultReady);
  QVERIFY(!computer.result());
  computer.request(request);
  QVERIFY(ready.wait(5000));
  const std::shared_ptr<const ts::OrbitResult> result = computer.result();
  QVERIFY(result);
  QCOMPARE(result->generation, uint64_t{7});
  QCOMPARE(result->states.size(), satellites.size());
  QVERIFY(result->states.front().ok());
  QVERIFY(!result->tracks.front().past.empty());
  QVERIFY(result->tracks[1].past.empty());
  // The same as computing here.
  const ts::SatelliteState here =
      ts::satelliteState(*satellites[3], request.time);
  QVERIFY(ts::distance(here.positionEcefM, result->states[3].positionEcefM) <
          1e-6);
  // Many requests at once: the last is what's left.
  for (uint64_t g = 10; g < 30; ++g) {
    request.generation = g;
    request.time = at(request.time, 1.0);
    computer.request(request);
  }
  QVERIFY(computer.waitForIdle(5000));
  QVERIFY(!computer.isBusy());
  QCOMPARE(computer.result()->generation, uint64_t{29});
  // Track points kept between requests by the synchronous form too.
  std::map<const ts::Sgp4*, std::unique_ptr<ts::TrackSampler>> samplers;
  ts::OrbitComputer::compute(request, samplers);
  QCOMPARE(samplers.size(), std::size_t{1});
  request.tasks.front().track = false;
  ts::OrbitComputer::compute(request, samplers);
  QVERIFY(samplers.empty());
}

void TestOrbits::celestrakUrls()
{
  QCOMPARE(ts::celestrakGroupUrl(QStringLiteral("stations")),
           QUrl(QStringLiteral("https://celestrak.org/NORAD/elements/"
                               "gp.php?GROUP=stations&FORMAT=json")));
  QCOMPARE(
      ts::celestrakGroupUrl(QStringLiteral("gps-ops"), ts::ElementFormat::Tle),
      QUrl(QStringLiteral("https://celestrak.org/NORAD/elements/"
                          "gp.php?GROUP=gps-ops&FORMAT=tle")));
  QCOMPARE(ts::celestrakSatelliteUrl(25544),
           QUrl(QStringLiteral("https://celestrak.org/NORAD/elements/"
                               "gp.php?CATNR=25544&FORMAT=json")));
  const QUrl mock(QStringLiteral("http://127.0.0.1:1234/gp.php"));
  QCOMPARE(ts::celestrakGroupUrl(QStringLiteral("starlink"),
                                 ts::ElementFormat::OmmJson, mock),
           QUrl(QStringLiteral(
               "http://127.0.0.1:1234/gp.php?GROUP=starlink&FORMAT=json")));
  const std::vector<ts::CelestrakGroup> groups = ts::celestrakGroups();
  QVERIFY(groups.size() > 30);
  QStringList ids;
  for (const ts::CelestrakGroup& g : groups) {
    QVERIFY(!g.name.isEmpty() && !g.category.isEmpty());
    QVERIFY(!ids.contains(g.id));
    ids.append(g.id);
  }
  QVERIFY(ids.contains(QStringLiteral("starlink")));
  QCOMPARE(ts::celestrakGroupName(QStringLiteral("stations")),
           QStringLiteral("Space stations"));
  QCOMPARE(ts::celestrakGroupName(QStringLiteral("unknown")),
           QStringLiteral("unknown"));
}

void TestOrbits::downloadAndCache()
{
  // A mock CelesTrak: the group as OMM JSON with an ETag, 304 when asked
  // with it.
  const QByteArray body = readData("stations.json");
  ts::test::MockHttpServer server(
      [&body](const ts::test::MockHttpRequest& request) {
        ts::test::MockHttpResponse response;
        if (request.header("if-none-match") == "\"v1\"") {
          response.status = 304;
          return response;
        }
        response.headers = {{"Content-Type", "application/json"},
                            {"ETag", "\"v1\""}};
        response.body = body;
        return response;
      });
  ts::NetworkClient client(testNetwork());
  QTemporaryDir cache;
  int64_t now = 1'800'000'000;
  ts::ElementDownloadSettings settings;
  settings.cacheDirectory = cache.path();
  settings.clock = [&now]() { return now; };
  const QUrl url = ts::celestrakGroupUrl(QStringLiteral("stations"),
                                         ts::ElementFormat::OmmJson,
                                         server.url(QStringLiteral("/gp.php")));
  QVERIFY(!ts::cachedElementSets(url, settings));
  // First: from the server, kept.
  ts::ElementDownload d =
      ts::downloadElementSets(client, url, settings, false, notCancelled);
  QCOMPARE(d.origin, ts::ElementOrigin::Server);
  QVERIFY(d.ok());
  QVERIFY(d.errorString.isEmpty());
  QCOMPARE(d.downloadedS, now);
  const std::size_t count = d.sets.elements.size();
  QCOMPARE(server.requestCount(), 1);
  QVERIFY(server.requests().front().url.query().contains(
      QLatin1String("GROUP=stations")));
  // Within two hours: from the cache, the server not asked.
  now += 3600;
  d = ts::downloadElementSets(client, url, settings, false, notCancelled);
  QCOMPARE(d.origin, ts::ElementOrigin::Cache);
  QCOMPARE(d.sets.elements.size(), count);
  QCOMPARE(server.requestCount(), 1);
  // Later: asked again with the ETag; 304 keeps the download, renewed.
  now += 7200;
  d = ts::downloadElementSets(client, url, settings, false, notCancelled);
  QCOMPARE(server.requestCount(), 2);
  QCOMPARE(server.requests().back().header("if-none-match"),
           QByteArray("\"v1\""));
  QCOMPARE(d.origin, ts::ElementOrigin::Server);
  QCOMPARE(d.downloadedS, now);
  QCOMPARE(d.sets.elements.size(), count);
  // Forced: asked at once, without the ETag.
  d = ts::downloadElementSets(client, url, settings, true, notCancelled);
  QCOMPARE(server.requestCount(), 3);
  QVERIFY(server.requests().back().header("if-none-match").isEmpty());
  QCOMPARE(d.origin, ts::ElementOrigin::Server);
  // Offline: the cache alone.
  const std::optional<ts::ElementDownload> offline =
      ts::cachedElementSets(url, settings);
  if (!offline) {
    QFAIL("nothing cached");
  }
  QCOMPARE(offline->sets.elements.size(), count);
  QCOMPARE(offline->downloadedS, now);
  // Without a cache directory nothing is kept.
  ts::ElementDownloadSettings uncached;
  QVERIFY(
      ts::downloadElementSets(client, url, uncached, false, notCancelled).ok());
  QVERIFY(!ts::cachedElementSets(url, uncached));
}

void TestOrbits::downloadFailures()
{
  // The server goes wrong in turn: the last download is used, and not
  // replaced by what isn't element sets.
  std::atomic<int32_t> mode{0};
  const QByteArray body = readData("gps-ops.tle");
  ts::test::MockHttpServer server(
      [&mode, &body](const ts::test::MockHttpRequest&) {
        ts::test::MockHttpResponse response;
        switch (mode.load()) {
        case 0:
          response.body = body;
          break;
        case 1:
          response.status = 503;
          break;
        case 2:
          response.body = "No GP data found";
          break;
        default:
          response.hangUp = true;
          break;
        }
        return response;
      });
  ts::NetworkClient client(testNetwork());
  QTemporaryDir cache;
  int64_t now = 1'800'000'000;
  ts::ElementDownloadSettings settings;
  settings.cacheDirectory = cache.path();
  settings.clock = [&now]() { return now; };
  const QUrl url = server.url(QStringLiteral("/gps-ops.txt"));
  // Nothing cached and an error: nothing, with the reason.
  mode = 1;
  ts::ElementDownload d =
      ts::downloadElementSets(client, url, settings, false, notCancelled);
  QVERIFY(!d.ok());
  QCOMPARE(d.origin, ts::ElementOrigin::None);
  QVERIFY(d.errorString.contains(QLatin1String("503")));
  // A good download (TLE text this time).
  mode = 0;
  d = ts::downloadElementSets(client, url, settings, false, notCancelled);
  QCOMPARE(d.origin, ts::ElementOrigin::Server);
  const std::size_t count = d.sets.elements.size();
  QVERIFY(count > 20);
  for (const int32_t failure : {1, 2, 3}) {
    mode = failure;
    now += int64_t{3} * 3600;
    d = ts::downloadElementSets(client, url, settings, false, notCancelled);
    QCOMPARE(d.origin, ts::ElementOrigin::StaleCache);
    QCOMPARE(d.sets.elements.size(), count);
    QVERIFY(!d.errorString.isEmpty());
  }
  const std::optional<ts::ElementDownload> kept =
      ts::cachedElementSets(url, settings);
  if (!kept) {
    QFAIL("nothing cached");
  }
  QCOMPARE(kept->sets.elements.size(), count);
}

void TestOrbits::satelliteSources()
{
  // Every origin round-trips; incomplete ones aren't sources.
  const ts::SatelliteSource celestrak =
      ts::SatelliteSource::celestrak(QStringLiteral("stations"));
  QVERIFY(celestrak.isDownloaded());
  QCOMPARE(celestrak.url, ts::celestrakGroupUrl(QStringLiteral("stations")));
  QVERIFY(celestrak.attribution.contains(QLatin1String("CelesTrak")));
  const QJsonObject json = celestrak.toJson();
  QVERIFY(ts::SatelliteSource::isSatellites(json));
  std::optional<ts::SatelliteSource> back = ts::SatelliteSource::fromJson(json);
  if (!back) {
    QFAIL("CelesTrak source not read back");
  }
  QCOMPARE(back->toJson(), json);
  ts::SatelliteSource file;
  file.origin = ts::SatelliteSource::Origin::File;
  file.path = QStringLiteral("/data/amateur.tle");
  back = ts::SatelliteSource::fromJson(file.toJson());
  if (!back) {
    QFAIL("file source not read back");
  }
  QCOMPARE(back->path, file.path);
  QVERIFY(!back->isDownloaded());
  ts::SatelliteSource text;
  text.origin = ts::SatelliteSource::Origin::Text;
  text.text = QLatin1String(issLine1);
  QVERIFY(ts::SatelliteSource::fromJson(text.toJson()));
  ts::SatelliteSource url;
  url.origin = ts::SatelliteSource::Origin::Url;
  url.url = QUrl(QStringLiteral("https://example.org/elements.txt"));
  QVERIFY(ts::SatelliteSource::fromJson(url.toJson()));
  // Incomplete, or something else.
  text.text.clear();
  QVERIFY(!ts::SatelliteSource::fromJson(text.toJson()));
  QJsonObject other = json;
  other.insert(QStringLiteral("kind"), QStringLiteral("kml"));
  QVERIFY(!ts::SatelliteSource::fromJson(other));
  other = json;
  other.insert(QStringLiteral("origin"), QStringLiteral("carrier pigeon"));
  QVERIFY(!ts::SatelliteSource::fromJson(other));
}

void TestOrbits::satelliteStyles()
{
  // Defaults: one orbit back and forward, tracks automatic, 14 days.
  const ts::SatelliteStyle defaults;
  QCOMPARE(defaults.past, (ts::TimeSpan{1.0, ts::SpanUnit::Orbits}));
  QCOMPARE(defaults.future, (ts::TimeSpan{1.0, ts::SpanUnit::Orbits}));
  QCOMPARE(defaults.tracks, ts::TrackMode::Automatic);
  QCOMPARE(defaults.maximumAgeDays, 14.0);
  QCOMPARE(ts::SatelliteStyle::fromJson(QJsonObject()), defaults);
  // Everything round-trips.
  ts::SatelliteStyle style;
  style.color = QColor(10, 20, 30, 40);
  style.markerSizePx = 9.0;
  style.labels = false;
  style.tracks = ts::TrackMode::All;
  style.past = ts::TimeSpan{45.0, ts::SpanUnit::Minutes};
  style.future = ts::TimeSpan{2.0, ts::SpanUnit::Orbits};
  style.groundTrack = true;
  style.footprints = ts::FootprintMode::Selected;
  style.footprintElevationDeg = 5.0;
  style.maximumAgeDays = 3.0;
  style.setShown(25544, false);
  style.overrides[25544].future = ts::TimeSpan{90.0, ts::SpanUnit::Minutes};
  QCOMPARE(ts::SatelliteStyle::fromJson(style.toJson()), style);
  // Shown and hidden: by default, with exceptions.
  QVERIFY(!style.isShown(25544));
  QVERIFY(style.isShown(5));
  style.setAllShown(false);
  QVERIFY(!style.isShown(5));
  style.setShown(5, true);
  QVERIFY(style.isShown(5));
  QCOMPARE(style.exceptions, (std::set<int32_t>{5}));
  QCOMPARE(ts::SatelliteStyle::fromJson(style.toJson()).isShown(5), true);
  QCOMPARE(ts::SatelliteStyle::fromJson(style.toJson()).isShown(6), false);
  // A satellite's own spans override the layer's.
  QCOMPARE(style.futureOf(25544), (ts::TimeSpan{90.0, ts::SpanUnit::Minutes}));
  QCOMPARE(style.pastOf(25544), style.past);
  QCOMPARE(style.futureOf(5), style.future);
  // Which draw tracks and footprints.
  ts::SatelliteStyle automatic;
  QVERIFY(automatic.drawsTrack(false, 20));
  QVERIFY(!automatic.drawsTrack(false, 21));
  QVERIFY(automatic.drawsTrack(true, 6000));
  automatic.tracks = ts::TrackMode::None;
  QVERIFY(!automatic.drawsTrack(true, 1));
  automatic.tracks = ts::TrackMode::Selected;
  QVERIFY(!automatic.drawsTrack(false, 1));
  automatic.tracks = ts::TrackMode::All;
  QVERIFY(automatic.drawsTrack(false, 6000));
  QVERIFY(!automatic.drawsFootprint(true));
  automatic.footprints = ts::FootprintMode::Selected;
  QVERIFY(automatic.drawsFootprint(true));
  QVERIFY(!automatic.drawsFootprint(false));
  // Values out of range are clamped; odd ones keep the defaults.
  QJsonObject odd;
  odd.insert(QStringLiteral("markerSizePx"), 1000.0);
  odd.insert(QStringLiteral("color"), QStringLiteral("not a colour"));
  odd.insert(QStringLiteral("tracks"), QStringLiteral("sometimes"));
  const ts::SatelliteStyle clamped = ts::SatelliteStyle::fromJson(odd);
  QCOMPARE(clamped.markerSizePx, 64.0);
  QCOMPARE(clamped.color, defaults.color);
  QCOMPARE(clamped.tracks, defaults.tracks);
}

void TestOrbits::satelliteDraws()
{
  const std::shared_ptr<const ts::Sgp4> iss = issSgp4();
  ts::TrackSampler sampler(iss);
  const ts::SatelliteTrack track =
      sampler.track(iss->elements().epoch, 60.0, 30.0);
  ts::TrackLook look;
  look.periodMinutes = iss->periodMinutes();
  // The past: solid, fading to the oldest end.
  const ts::SpaceLine past = ts::pastTrackLine(track, look);
  QCOMPARE(past.points.size(), track.past.size());
  QVERIFY(!past.dashed);
  QCOMPARE(past.points.front().alpha, look.oldestAlpha);
  QCOMPARE(past.points.back().alpha, 1.0F);
  // The future: dashed, look.dashesPerOrbit dashes an orbit.
  const ts::SpaceLine future = ts::futureTrackLine(track, look);
  QVERIFY(future.dashed);
  const float dashes = future.points.back().along - future.points.front().along;
  QVERIFY(std::fabs(static_cast<double>(dashes) -
                    30.0 / look.periodMinutes * look.dashesPerOrbit) < 1e-3);
  // Ground tracks are on the ground, on the globe only.
  const std::vector<ts::SpaceLine> ground = ts::groundTrackLines(track, look);
  QCOMPARE(ground.size(), std::size_t{2});
  for (const ts::SpaceLine& line : ground) {
    QVERIFY(line.globeOnly);
    for (const ts::SpaceLinePoint& p : line.points) {
      QVERIFY(std::fabs(ts::ecefToGeodetic(p.positionEcef).heightM) < 1e-3);
    }
  }
  // The footprint: a closed ring on the ground; the marker at the place.
  const ts::SatelliteState state =
      ts::satelliteState(*iss, iss->elements().epoch);
  const ts::SpaceLine ring = ts::footprintLine(state, 10.0, look);
  QVERIFY(ring.closed);
  QCOMPARE(ring.points.size(), std::size_t{90});
  const ts::GlobeMarker marker = ts::satelliteMarker(state, {1, 2, 3, 4}, 5.0);
  QCOMPARE(marker.radiusPx, 5.0);
  QVERIFY(ts::distance(marker.positionEcef, state.positionEcefM) < 1e-9);
  // Nothing to draw from nothing.
  QVERIFY(ts::pastTrackLine(ts::SatelliteTrack(), look).points.empty());
  ts::SatelliteState none;
  none.error = ts::Sgp4Error::Decayed;
  QVERIFY(ts::footprintLine(none, 10.0, look).points.empty());
}

QTEST_GUILESS_MAIN(TestOrbits)

#include "tst_orbits.moc"
