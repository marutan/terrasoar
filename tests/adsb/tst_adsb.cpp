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
#include <optional>

#include <map>
#include <vector>

#include <QBuffer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTcpSocket>
#include <QtTest>

#include "ts/adsb/adsbdecoder.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftreport.h"
#include "ts/adsb/aircrafttracks.h"
#include "ts/adsb/cpr.h"
#include "ts/adsb/modesmessage.h"
#include "ts/geo/geodetic.h"
#include "ts/tracks/movingtrack.h"

namespace
{

std::optional<ts::ModeSMessage> decodeHex(const char* hex)
{
  return ts::decodeModeS(ts::modeSFromHex(QByteArray(hex)));
}

/**
 * Encodes a position as a CPR frame, as DO-260B's encoding does: the
 * test's own, to make frames at chosen places.
 */
ts::CprFrame encodeCpr(double latDeg, double lonDeg, bool odd, bool surface)
{
  const double span = surface ? 90.0 : 360.0;
  const double scale = 131072.0;
  const double i = odd ? 1.0 : 0.0;
  const double dLat = span / (60.0 - i);
  const double modLat = latDeg - dLat * std::floor(latDeg / dLat);
  const double yz = std::floor(scale * modLat / dLat + 0.5);
  const double rLat = dLat * (yz / scale + std::floor(latDeg / dLat));
  const int32_t nl = ts::cprLongitudeZones(rLat);
  const double dLon = span / std::max(nl - static_cast<int32_t>(i), 1);
  const double modLon = lonDeg - dLon * std::floor(lonDeg / dLon);
  const double xz = std::floor(scale * modLon / dLon + 0.5);
  ts::CprFrame frame;
  frame.lat = static_cast<uint32_t>(std::fmod(yz, scale));
  frame.lon = static_cast<uint32_t>(std::fmod(xz, scale));
  frame.odd = odd;
  frame.surface = surface;
  return frame;
}

/** Positions agree to @p metres. */
bool near(const ts::CprPosition& a, double latDeg, double lonDeg, double metres)
{
  return ts::distanceNm(a, ts::CprPosition{latDeg, lonDeg}) * 1852.0 <= metres;
}

QString dataPath(const char* name)
{
  return QStringLiteral(TS_ADSB_DATA_DIR "/") + QLatin1String(name);
}

QByteArray readData(const char* name)
{
  QFile file(dataPath(name));
  if (!file.open(QIODevice::ReadOnly)) {
    return QByteArray();
  }
  return file.readAll();
}

/** One second of a made-up flight, from truth.csv. */
struct Truth {
  double timeS = 0.0;
  double latDeg = 0.0;
  double lonDeg = 0.0;
  double altitudeFt = 0.0;
};

std::map<uint32_t, std::vector<Truth>> readTruth()
{
  std::map<uint32_t, std::vector<Truth>> truth;
  const QList<QByteArray> lines = readData("truth.csv").split('\n');
  for (int32_t i = 1; i < lines.size(); ++i) {
    const QList<QByteArray> f = lines.at(i).split(',');
    if (f.size() < 5) {
      continue;
    }
    Truth t;
    t.timeS = f.at(0).toDouble();
    t.latDeg = f.at(2).toDouble();
    t.lonDeg = f.at(3).toDouble();
    t.altitudeFt = f.at(4).toDouble();
    truth[f.at(1).toUInt(nullptr, 16)].push_back(t);
  }
  return truth;
}

/** Reads a fixture into tracks. */
std::vector<ts::AircraftTrack> readTracks(const char* name,
                                          ts::AdsbReadOptions options,
                                          ts::AdsbReadResult* result = nullptr)
{
  ts::AircraftTrackBuilder builder;
  const ts::AdsbReadResult read = ts::readAdsbFile(
      dataPath(name), ts::AdsbFormat::Unknown, options,
      [&builder](const ts::AircraftReport& report) { builder.add(report); });
  if (result != nullptr) {
    *result = read;
  }
  return builder.finish();
}

/** The fixtures' start, 2026-11-20T12:00Z. */
constexpr double fixtureStartS = 1795176000.0;

} // namespace

/** Tests of ts/adsb. */
class TestAdsb : public QObject
{
  Q_OBJECT

private slots:
  void parity();
  void identification();
  void airbornePosition();
  void localPosition();
  void surfacePosition();
  void velocity();
  void surveillanceReplies();
  void altitudeCodes();
  void zones();
  void cprRoundTrip_data();
  void cprRoundTrip();
  void cprZoneBoundary();
  void decoderPairs();
  void decoderUnknownAddress();
  void detectFormats();
  void readersAgree_data();
  void readersAgree();
  void readerDetails();
  void malformedLines();
  void csvColumns();
  void beastStream();
  void receiverClock();
  void trackStore();
  void trackGapsAndStale();
  void trailModes();
  void heights();
  void builderOrder();
  void liveFeed();
};

void TestAdsb::parity()
{
  // The 1090 MHz Riddle (Sun, 2021), "Error control".
  QCOMPARE(ts::modeSParity(ts::modeSFromHex("8D406B902015A678D4D220AA4BDA")),
           0U);
  QVERIFY(ts::modeSParity(ts::modeSFromHex("8D4CA251204994B1C36E60A5343D")) !=
          0U);
  QVERIFY(!decodeHex("8D4CA251204994B1C36E60A5343D"));
  // One bit changed anywhere fails.
  QByteArray message = ts::modeSFromHex("8D4840D6202CC371C32CE0576098");
  for (int32_t bit = 0; bit < 112; ++bit) {
    QByteArray corrupt = message;
    corrupt[bit / 8] = static_cast<char>(
        static_cast<uint8_t>(corrupt.at(bit / 8)) ^ (0x80U >> (bit % 8)));
    // In the downlink format the message becomes another, whose parity
    // is an address: unchecked, so used only for aircraft already heard.
    const std::optional<ts::ModeSMessage> decoded = ts::decodeModeS(corrupt);
    QVERIFY2(!decoded || (bit < 5 && decoded->addressFromParity),
             qPrintable(QString::number(bit)));
  }
  QVERIFY(ts::modeSFromHex("8D4840D6202CC371C32CE05760").isEmpty());
  QVERIFY(ts::modeSFromHex("8D4840D6202CC371C32CE05760XX").isEmpty());
}

void TestAdsb::identification()
{
  const std::optional<ts::ModeSMessage> m =
      decodeHex("8D4840D6202CC371C32CE0576098");
  QVERIFY(m);
  QCOMPARE(m->downlinkFormat, 17);
  QCOMPARE(m->typeCode, 4);
  QCOMPARE(ts::formatIcaoAddress(m->address), QStringLiteral("4840D6"));
  QCOMPARE(*m->callsign, QStringLiteral("KLM1023"));
  QVERIFY(!m->addressFromParity);
}

void TestAdsb::airbornePosition()
{
  const std::optional<ts::ModeSMessage> even =
      decodeHex("8D40621D58C382D690C8AC2863A7");
  const std::optional<ts::ModeSMessage> odd =
      decodeHex("8D40621D58C386435CC412692AD6");
  QVERIFY(even && odd && even->cpr && odd->cpr);
  QCOMPARE(even->cpr->lat, 93000U);
  QCOMPARE(even->cpr->lon, 51372U);
  QCOMPARE(odd->cpr->lat, 74158U);
  QCOMPARE(odd->cpr->lon, 50194U);
  QVERIFY(!even->cpr->odd && odd->cpr->odd);
  QCOMPARE(*even->altitudeFt, 38000);
  QCOMPARE(ts::cprLongitudeZones(52.2572021484375), 36);
  // The even frame is newer (1457996402 against 1457996400).
  const std::optional<ts::CprPosition> p =
      ts::cprGlobalAirborne(*even->cpr, *odd->cpr, false);
  QVERIFY(p);
  QCOMPARE(p->latDeg, 52.2572021484375);
  QCOMPARE(p->lonDeg, 3.91937255859375);

  ts::AdsbDecoder decoder;
  QVERIFY(!decoder
               .decode(ts::modeSFromHex("8D40621D58C386435CC412692AD6"),
                       1457996400.0)
               ->position);
  const std::optional<ts::AircraftReport> report = decoder.decode(
      ts::modeSFromHex("8D40621D58C382D690C8AC2863A7"), 1457996402.0);
  QVERIFY(report && report->position);
  QCOMPARE(report->position->latDeg, 52.2572021484375);
  QCOMPARE(*report->baroAltitudeFt, 38000.0);
}

void TestAdsb::localPosition()
{
  const std::optional<ts::ModeSMessage> even =
      decodeHex("8D40621D58C382D690C8AC2863A7");
  const ts::CprPosition p =
      ts::cprLocal(*even->cpr, ts::CprPosition{52.258, 3.918});
  QCOMPARE(p.latDeg, 52.2572021484375);
  QCOMPARE(p.lonDeg, 3.91937255859375);
}

void TestAdsb::surfacePosition()
{
  const std::optional<ts::ModeSMessage> even =
      decodeHex("8C4841753AAB238733C8CD4020B1");
  const std::optional<ts::ModeSMessage> odd =
      decodeHex("8C4841753A8A35323FAEBDAC702D");
  QVERIFY(even && odd && even->cpr && odd->cpr);
  QVERIFY(even->cpr->surface);
  QVERIFY(*even->onGround);
  QCOMPARE(even->cpr->lat, 115609U);
  QCOMPARE(odd->cpr->lon, 110269U);
  // The odd frame is newer; the airfield is the reference.
  const std::optional<ts::CprPosition> p = ts::cprGlobalSurface(
      *even->cpr, *odd->cpr, true, ts::CprPosition{51.990, 4.375});
  QVERIFY(p);
  QVERIFY(std::fabs(p->latDeg - 52.320607072215964) < 1e-9);
  QVERIFY(std::fabs(p->lonDeg - 4.734734671456474) < 1e-9);

  const std::optional<ts::ModeSMessage> next =
      decodeHex("8C4841753A9A153237AEF0F275BE");
  QVERIFY(next && next->cpr);
  const ts::CprPosition local =
      ts::cprLocal(*next->cpr, ts::CprPosition{52.320607, 4.734735});
  QVERIFY(std::fabs(local.latDeg - 52.32056051997815) < 1e-9);
  QVERIFY(std::fabs(local.lonDeg - 4.735735212053571) < 1e-9);
  QCOMPARE(*next->groundSpeedKt, 17.0);
  QCOMPARE(*next->trackDeg, 92.8125);
}

void TestAdsb::velocity()
{
  const std::optional<ts::ModeSMessage> a =
      decodeHex("8D485020994409940838175B284F");
  QVERIFY(a && a->groundSpeedKt && a->trackDeg);
  QCOMPARE(a->typeCode, 19);
  QVERIFY(std::fabs(*a->groundSpeedKt - 159.20) < 0.01);
  QVERIFY(std::fabs(*a->trackDeg - 182.88) < 0.01);
  QCOMPARE(*a->verticalRateFtMin, -832);
  QVERIFY(!a->verticalRateBaro);
  QCOMPARE(*a->gnssMinusBaroFt, 550);

  const std::optional<ts::ModeSMessage> b =
      decodeHex("8DA05F219B06B6AF189400CBC33F");
  QVERIFY(b && b->headingDeg && b->airspeedKt);
  QVERIFY(std::fabs(*b->headingDeg - 243.98) < 0.01);
  QCOMPARE(*b->airspeedKt, 375.0);
  QVERIFY(b->trueAirspeed);
  QCOMPARE(*b->verticalRateFtMin, -2304);
  QVERIFY(b->verticalRateBaro);
  QVERIFY(!b->groundSpeedKt);
}

void TestAdsb::surveillanceReplies()
{
  const std::optional<ts::ModeSMessage> altitude = decodeHex("2000171806A983");
  QVERIFY(altitude);
  QCOMPARE(altitude->downlinkFormat, 4);
  QVERIFY(altitude->addressFromParity);
  QCOMPARE(*altitude->altitudeFt, 36000);
  const std::optional<ts::ModeSMessage> identity = decodeHex("2A00516D492B80");
  QVERIFY(identity);
  QCOMPARE(identity->downlinkFormat, 5);
  QCOMPARE(*identity->squawk, QStringLiteral("0356"));
}

void TestAdsb::altitudeCodes()
{
  QVERIFY(!ts::decodeAltitudeCode(0));
  // Q bit: 25 ft steps.
  for (int32_t feet = -1000; feet < 50000; feet += 25) {
    const uint32_t n = static_cast<uint32_t>((feet + 1000) / 25);
    const uint32_t code =
        ((n & 0x7E0U) << 2U) | ((n & 0x10U) << 1U) | 0x10U | (n & 0x0FU);
    QCOMPARE(*ts::decodeAltitudeCode(code), feet);
  }
  // Gillham (100 ft): the encoding of ICAO Annex 10 (Gray code in 500 ft
  // with a reflected 100 ft code), written out independently here.
  for (int32_t feet = -1200; feet <= 62700; feet += 100) {
    const int32_t units = (feet + 1200) / 100; // 0 for -1200 ft
    const int32_t n500 = units / 5;
    int32_t n100 = units % 5 + 1; // 1 to 5
    if (n500 % 2 != 0) {
      n100 = 6 - n100;
    }
    const uint32_t gray500 = static_cast<uint32_t>(n500 ^ (n500 >> 1));
    const uint32_t c100 = n100 == 5 ? 7U : static_cast<uint32_t>(n100);
    const uint32_t gray100 = c100 ^ (c100 >> 1U);
    // gray500 = D2 D4 A1 A2 A4 B1 B2 B4; gray100 = C1 C2 C4.
    const uint32_t d2 = (gray500 >> 7U) & 1U;
    const uint32_t d4 = (gray500 >> 6U) & 1U;
    const uint32_t a1 = (gray500 >> 5U) & 1U;
    const uint32_t a2 = (gray500 >> 4U) & 1U;
    const uint32_t a4 = (gray500 >> 3U) & 1U;
    const uint32_t b1 = (gray500 >> 2U) & 1U;
    const uint32_t b2 = (gray500 >> 1U) & 1U;
    const uint32_t b4 = gray500 & 1U;
    const uint32_t c1 = (gray100 >> 2U) & 1U;
    const uint32_t c2 = (gray100 >> 1U) & 1U;
    const uint32_t c4 = gray100 & 1U;
    const uint32_t code = (c1 << 12U) | (a1 << 11U) | (c2 << 10U) | (a2 << 9U) |
                          (c4 << 8U) | (a4 << 7U) | (b1 << 5U) | (b2 << 3U) |
                          (d2 << 2U) | (b4 << 1U) | d4;
    const std::optional<int32_t> decoded = ts::decodeAltitudeCode(code);
    QVERIFY2(decoded, qPrintable(QString::number(feet)));
    QCOMPARE(*decoded, feet);
  }
}

void TestAdsb::zones()
{
  QCOMPARE(ts::cprLongitudeZones(0.0), 59);
  QCOMPARE(ts::cprLongitudeZones(10.4), 59);
  QCOMPARE(ts::cprLongitudeZones(10.5), 58);
  QCOMPARE(ts::cprLongitudeZones(-10.5), 58);
  QCOMPARE(ts::cprLongitudeZones(86.9), 2);
  QCOMPARE(ts::cprLongitudeZones(87.1), 1);
  QCOMPARE(ts::cprLongitudeZones(-90.0), 1);
}

void TestAdsb::cprRoundTrip_data()
{
  QTest::addColumn<double>("lat");
  QTest::addColumn<double>("lon");
  QTest::addColumn<bool>("surface");
  QTest::newRow("amsterdam") << 52.3 << 4.76 << false;
  QTest::newRow("equator") << 0.001 << 0.001 << false;
  QTest::newRow("south") << -33.94 << 151.18 << false;
  QTest::newRow("west antimeridian") << 51.0 << 179.999 << false;
  QTest::newRow("east antimeridian") << -16.0 << -179.999 << false;
  QTest::newRow("near north pole") << 88.5 << 40.0 << false;
  QTest::newRow("near south pole") << -89.2 << -120.0 << false;
  QTest::newRow("zone 87") << 87.2 << 10.0 << false;
  QTest::newRow("surface schiphol") << 52.31 << 4.76 << true;
  QTest::newRow("surface sydney") << -33.94 << 151.18 << true;
  QTest::newRow("surface antimeridian") << -16.6 << 179.99 << true;
  QTest::newRow("surface west") << 40.64 << -73.78 << true;
}

void TestAdsb::cprRoundTrip()
{
  QFETCH(double, lat);
  QFETCH(double, lon);
  QFETCH(bool, surface);
  const ts::CprFrame even = encodeCpr(lat, lon, false, surface);
  const ts::CprFrame odd = encodeCpr(lat, lon, true, surface);
  // Airborne 5.1 m in latitude; surface a quarter.
  const double tolerance = surface ? 3.0 : 12.0;
  const ts::CprPosition reference{lat + 0.3, lon - 0.3};
  for (const bool oddNewer : {false, true}) {
    const std::optional<ts::CprPosition> global =
        surface ? ts::cprGlobalSurface(even, odd, oddNewer, reference)
                : ts::cprGlobalAirborne(even, odd, oddNewer);
    QVERIFY(global);
    QVERIFY2(near(*global, lat, lon, tolerance),
             qPrintable(QStringLiteral("%1 %2")
                            .arg(global->latDeg, 0, 'f', 7)
                            .arg(global->lonDeg, 0, 'f', 7)));
  }
  for (const ts::CprFrame& frame : {even, odd}) {
    QVERIFY(near(ts::cprLocal(frame, reference), lat, lon, tolerance));
  }
}

void TestAdsb::cprZoneBoundary()
{
  // Either side of the boundary between 36 and 35 longitude zones
  // (53.095°): the frames lie in different zones and no position is given.
  const double boundary = 53.095;
  bool refusedSome = false;
  for (double lat = boundary - 0.02; lat < boundary + 0.02; lat += 0.0005) {
    const ts::CprFrame even = encodeCpr(lat - 0.01, 5.0, false, false);
    const ts::CprFrame odd = encodeCpr(lat + 0.01, 5.0, true, false);
    const std::optional<ts::CprPosition> p =
        ts::cprGlobalAirborne(even, odd, true);
    if (!p) {
      refusedSome = true;
    } else {
      QVERIFY(std::fabs(p->latDeg - (lat + 0.01)) < 0.001);
      QVERIFY(std::fabs(p->lonDeg - 5.0) < 0.001);
    }
  }
  QVERIFY(refusedSome);
}

void TestAdsb::decoderPairs()
{
  // Frames more than 10 s apart don't make a pair.
  const QByteArray even = ts::modeSFromHex("8D40621D58C382D690C8AC2863A7");
  const QByteArray odd = ts::modeSFromHex("8D40621D58C386435CC412692AD6");
  ts::AdsbDecoder decoder;
  QVERIFY(!decoder.decode(odd, 1000.0)->position);
  QVERIFY(!decoder.decode(even, 1011.0)->position);
  // Then a newer odd within 10 s of that even makes one.
  const std::optional<ts::AircraftReport> report = decoder.decode(odd, 1015.0);
  QVERIFY(report && report->position);
  QVERIFY(near(*report->position, 52.2657801741, 3.9389, 300.0));
  // Next, decoded locally from there.
  const std::optional<ts::AircraftReport> next = decoder.decode(even, 1016.0);
  QVERIFY(next && next->position);
  QCOMPARE(next->position->latDeg, 52.2572021484375);
  QCOMPARE(decoder.counts().positions, int64_t{2});

  // A receiver far away refuses the positions.
  ts::AdsbDecoderSettings far;
  far.receiver = ts::CprPosition{-33.0, 151.0};
  ts::AdsbDecoder distant(far);
  distant.decode(odd, 1000.0);
  QVERIFY(!distant.decode(even, 1001.0)->position);
  QCOMPARE(distant.counts().positionsRefused, int64_t{1});
}

void TestAdsb::decoderUnknownAddress()
{
  // DF 4 (address from parity) of an aircraft never heard: dropped.
  ts::AdsbDecoder decoder;
  QVERIFY(!decoder.decode(ts::modeSFromHex("2000171806A983"), 10.0));
  QCOMPARE(decoder.counts().unknownAddress, int64_t{1});
  // Heard (a DF 17 with that address), then believed.
  const std::optional<ts::ModeSMessage> altitude = decodeHex("2000171806A983");
  ts::ModeSMessage heard;
  heard.downlinkFormat = 17;
  heard.address = altitude->address;
  heard.callsign = QStringLiteral("TEST");
  QVERIFY(decoder.decode(heard, 11.0));
  const std::optional<ts::AircraftReport> report =
      decoder.decode(ts::modeSFromHex("2000171806A983"), 12.0);
  QVERIFY(report);
  QCOMPARE(*report->baroAltitudeFt, 36000.0);
  // Not after a minute without a checked message.
  QVERIFY(!decoder.decode(ts::modeSFromHex("2000171806A983"), 80.0));
}

void TestAdsb::detectFormats()
{
  QCOMPARE(ts::detectAdsbFormat(readData("flights.sbs").left(4096)),
           ts::AdsbFormat::Sbs);
  QCOMPARE(ts::detectAdsbFormat(readData("flights-timed.txt").left(4096)),
           ts::AdsbFormat::Raw);
  QCOMPARE(ts::detectAdsbFormat(readData("flights-mlat.avr").left(4096)),
           ts::AdsbFormat::Raw);
  QCOMPARE(ts::detectAdsbFormat(readData("flights.beast").left(4096)),
           ts::AdsbFormat::Beast);
  QCOMPARE(ts::detectAdsbFormat(readData("flights-aircraft.jsonl").left(4096)),
           ts::AdsbFormat::ReadsbJson);
  QCOMPARE(ts::detectAdsbFormat(readData("trace_full_abc001.json").left(4096)),
           ts::AdsbFormat::Trace);
  QCOMPARE(ts::detectAdsbFormat(readData("flights.csv").left(4096)),
           ts::AdsbFormat::Csv);
  QCOMPARE(ts::detectAdsbFormat(QByteArray("*8D4840D6202CC371C32CE0576098;\n")),
           ts::AdsbFormat::Raw);
  QCOMPARE(ts::detectAdsbFormat(QByteArray("hello")), ts::AdsbFormat::Unknown);
  for (const ts::AdsbFormat format :
       {ts::AdsbFormat::Sbs, ts::AdsbFormat::Raw, ts::AdsbFormat::Beast,
        ts::AdsbFormat::ReadsbJson, ts::AdsbFormat::Trace,
        ts::AdsbFormat::Csv}) {
    QCOMPARE(ts::adsbFormatFromId(ts::adsbFormatId(format)), format);
  }
}

void TestAdsb::readersAgree_data()
{
  QTest::addColumn<QString>("file");
  QTest::addColumn<int32_t>("aircraft");
  QTest::addColumn<double>("toleranceM");
  QTest::newRow("sbs") << QStringLiteral("flights.sbs") << 3 << 2.0;
  QTest::newRow("raw timed")
      << QStringLiteral("flights-timed.txt") << 3 << 12.0;
  QTest::newRow("avr mlat") << QStringLiteral("flights-mlat.avr") << 3 << 12.0;
  QTest::newRow("beast") << QStringLiteral("flights.beast") << 3 << 12.0;
  QTest::newRow("readsb json")
      << QStringLiteral("flights-aircraft.jsonl") << 3 << 1.0;
  QTest::newRow("trace") << QStringLiteral("trace_full_abc001.json") << 1
                         << 1.0;
  QTest::newRow("trace gzip")
      << QStringLiteral("trace_full_abc001-gzip.json") << 1 << 1.0;
  QTest::newRow("csv") << QStringLiteral("flights.csv") << 3 << 1.0;
}

void TestAdsb::readersAgree()
{
  QFETCH(QString, file);
  QFETCH(int32_t, aircraft);
  QFETCH(double, toleranceM);
  ts::AdsbReadOptions options;
  options.startTimeS = fixtureStartS;
  options.sbsLocalTime = false;
  options.csv = ts::guessCsvColumns(readData("flights.csv").left(4096));
  options.csv.time = 0;
  options.csv.address = 1;
  options.csv.altitude = 4;
  ts::AdsbReadResult result;
  const std::vector<ts::AircraftTrack> tracks =
      readTracks(file.toUtf8().constData(), options, &result);
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QCOMPARE(static_cast<int32_t>(tracks.size()), aircraft);
  const std::map<uint32_t, std::vector<Truth>> truth = readTruth();
  ts::TrackTiming timing;
  timing.gapS = 30.0;
  for (const ts::AircraftTrack& track : tracks) {
    QVERIFY(truth.count(track.address) == 1);
    QVERIFY(track.track.points().size() > 20);
    int32_t compared = 0;
    for (const Truth& t : truth.at(track.address)) {
      if (t.timeS < track.track.firstTimeS() ||
          t.timeS > track.track.lastTimeS()) {
        continue;
      }
      const std::optional<ts::TrackSample> p = track.track.at(t.timeS, timing);
      if (!p) {
        continue;
      }
      // Between reports 2 s apart a straight line is within a metre.
      const double metres =
          ts::distanceNm(ts::CprPosition{p->latDeg, p->lonDeg},
                         ts::CprPosition{t.latDeg, t.lonDeg}) *
          1852.0;
      QVERIFY2(metres <= toleranceM + 1.0,
               qPrintable(QStringLiteral("%1 at %2: %3 m")
                              .arg(ts::formatIcaoAddress(track.address))
                              .arg(t.timeS - fixtureStartS)
                              .arg(metres)));
      ++compared;
    }
    QVERIFY(compared > 100);
    // Every format knows the callsign (the CSV and trace from their
    // fields, the others from identification).
    QCOMPARE(track.lastCallsign().left(3), QStringLiteral("TSO"));
  }
}

void TestAdsb::readerDetails()
{
  ts::AdsbReadOptions options;
  options.sbsLocalTime = false;
  const std::vector<ts::AircraftTrack> json =
      readTracks("flights-aircraft.jsonl", options);
  QCOMPARE(json.size(), std::size_t{3});
  const ts::AircraftInfo* info = json.front().infoAt(fixtureStartS + 10.0);
  QVERIFY(info);
  QCOMPARE(info->callsign, QStringLiteral("TSO101"));
  QCOMPARE(info->registration, QStringLiteral("G-TSOA"));
  QCOMPARE(info->aircraftType, QStringLiteral("A320"));
  QCOMPARE(info->squawk, QStringLiteral("1000"));
  QCOMPARE(info->category, QStringLiteral("A3"));
  // Barometric altitudes are approximate heights; the climb is there.
  const ts::TrackSample& first = json.front().track.points().front();
  QVERIFY(first.heightApproximate);
  QVERIFY(std::fabs(first.heightM - ts::pressureAltitudeToHeightM(30000.0)) <
          1.0);
  QCOMPARE(json.front().altitudes.front().baroFt, 30000.0F);
  QVERIFY(std::isnan(json.front().altitudes.front().geometricFt));
  QVERIFY(std::fabs(static_cast<double>(first.speedMS) -
                    450.0 * ts::knotsToMetresPerSecond) < 0.1);

  // Raw messages: the gap of the third flight is kept.
  ts::AdsbReadResult result;
  options.startTimeS = fixtureStartS;
  const std::vector<ts::AircraftTrack> raw =
      readTracks("flights.beast", options, &result);
  QCOMPARE(result.format, ts::AdsbFormat::Beast);
  QCOMPARE(result.messagesRefused, int64_t{0});
  QVERIFY(!result.untimed);
  ts::TrackTiming timing;
  QCOMPARE(raw.at(2).track.pieces(timing).size(), std::size_t{2});
  QCOMPARE(raw.at(0).track.pieces(timing).size(), std::size_t{1});
  // Across the antimeridian: no point far from the line.
  for (const ts::TrackSample& p : raw.at(1).track.points()) {
    QVERIFY(std::fabs(p.latDeg + 16.5) < 0.1);
    QVERIFY(std::fabs(p.lonDeg) > 179.0);
  }
}

void TestAdsb::malformedLines()
{
  QByteArray sbs = readData("flights.sbs");
  QList<QByteArray> lines = sbs.split('\n');
  lines.insert(3, "MSG,3,1,1,ZZZZZZ,1,2026/11/20,12:00:00.000,x");
  lines.insert(6, "garbage line");
  lines.insert(9, "MSG,3,1,1,ABC001,1,2026/13/40,12:00:00.000,2026/11/20,"
                  "12:00:00.000,,1,,,1,1,,,0,0,0,0");
  sbs = lines.join('\n');
  ts::AdsbReadOptions options;
  options.sbsLocalTime = false;
  int64_t reports = 0;
  const ts::AdsbReadResult result =
      ts::readAdsbData(sbs, ts::AdsbFormat::Sbs, options,
                       [&reports](const ts::AircraftReport&) { ++reports; });
  QCOMPARE(result.problemCount, int64_t{3});
  QCOMPARE(result.problems.at(0).line, int64_t{4});
  QCOMPARE(result.problems.at(1).line, int64_t{7});
  QCOMPARE(result.problems.at(2).line, int64_t{10});
  QVERIFY(reports > 1000);

  // Raw: bad hex is a problem, failed parity only counted.
  const QByteArray raw = "1795176000.0,8D4840D6202CC371C32CE0576098\n"
                         "1795176000.1,8D4840D6202CC371C32CE0576099\n"
                         "1795176000.2,XYZ\n"
                         "*0123;\n"
                         "\n";
  const ts::AdsbReadResult rawResult = ts::readAdsbData(
      raw, ts::AdsbFormat::Unknown, options, [](const ts::AircraftReport&) {});
  QCOMPARE(rawResult.format, ts::AdsbFormat::Raw);
  QCOMPARE(rawResult.problemCount, int64_t{1});
  QCOMPARE(rawResult.problems.at(0).line, int64_t{3});
  QCOMPARE(rawResult.messagesRefused, int64_t{1});
  QCOMPARE(rawResult.reports, int64_t{1});

  // Untimed AVR is marked.
  const ts::AdsbReadResult untimed =
      ts::readAdsbData("*8D4840D6202CC371C32CE0576098;\n", ts::AdsbFormat::Raw,
                       options, [](const ts::AircraftReport&) {});
  QVERIFY(untimed.untimed);

  // Not a format at all.
  const ts::AdsbReadResult unknown =
      ts::readAdsbData("hello\nworld\n", ts::AdsbFormat::Unknown, options,
                       [](const ts::AircraftReport&) {});
  QVERIFY(!unknown.error.isEmpty());
  // A broken trace.
  QVERIFY(!ts::readAdsbData("{\"trace\": [", ts::AdsbFormat::Trace, options,
                            [](const ts::AircraftReport&) {})
               .error.isEmpty());
}

void TestAdsb::csvColumns()
{
  const ts::CsvColumns truth =
      ts::guessCsvColumns(readData("truth.csv").left(4096));
  QVERIFY(truth.header);
  QCOMPARE(truth.separator, QLatin1Char(','));
  QCOMPARE(truth.time, 0);
  QCOMPARE(truth.address, 1);
  QCOMPARE(truth.latitude, 2);
  QCOMPARE(truth.longitude, 3);
  QCOMPARE(truth.altitude, 4);
  QCOMPARE(truth.speed, 5);
  QCOMPARE(truth.heading, 6);
  QCOMPARE(truth.verticalRate, 7);
  QCOMPARE(truth.callsign, 8);
  QVERIFY(truth.isComplete());

  const ts::CsvColumns own =
      ts::guessCsvColumns(readData("flights.csv").left(4096));
  QCOMPARE(own.separator, QLatin1Char(';'));
  QCOMPARE(own.latitude, 2);
  QCOMPARE(own.callsign, 7);
  QVERIFY(!own.isComplete()); // "when" and "hex_ident" aren't guessed
  // Its two bad rows.
  ts::AdsbReadOptions options;
  options.csv = own;
  options.csv.time = 0;
  options.csv.address = 1;
  ts::AdsbReadResult result;
  readTracks("flights.csv", options, &result);
  QCOMPARE(result.problemCount, int64_t{2});
  QCOMPARE(result.problems.at(0).line, int64_t{6});
  QCOMPARE(result.problems.at(1).line, int64_t{10});

  // OpenSky's state vectors: metres and metres a second.
  const ts::CsvColumns opensky = ts::guessCsvColumns(
      "time,icao24,lat,lon,velocity,heading,vertrate,callsign,onground,"
      "alert,spi,squawk,baroaltitude,geoaltitude,lastposupdate,lastcontact\n");
  QCOMPARE(opensky.altitude, 12);
  QCOMPARE(opensky.altitudeKind, ts::CsvAltitude::BaroMetres);
  QCOMPARE(opensky.speedUnit, ts::CsvSpeed::MetresPerSecond);

  QCOMPARE(ts::splitCsvLine(QStringLiteral("a,\"b,c\",\"d\"\"e\""),
                            QLatin1Char(',')),
           QStringList({QStringLiteral("a"), QStringLiteral("b,c"),
                        QStringLiteral("d\"e")}));
  const ts::CsvColumns back = ts::CsvColumns::fromJson(own.toJson());
  QCOMPARE(back.separator, own.separator);
  QCOMPARE(back.callsign, own.callsign);
}

void TestAdsb::beastStream()
{
  // The same bytes in pieces of every size give the same messages.
  const QByteArray beast = readData("flights.beast");
  ts::BeastParser whole;
  const std::vector<ts::TimedMessage> all = whole.feed(beast);
  QVERIFY(all.size() > 1000);
  QCOMPARE(whole.skippedBytes(), int64_t{0});
  for (const int32_t piece : {1, 2, 7, 13, 100}) {
    ts::BeastParser parser;
    std::vector<ts::TimedMessage> messages;
    for (int32_t i = 0; i < beast.size(); i += piece) {
      for (const ts::TimedMessage& m : parser.feed(beast.mid(i, piece))) {
        messages.push_back(m);
      }
    }
    QCOMPARE(messages.size(), all.size());
    QCOMPARE(messages.back().bytes, all.back().bytes);
    QCOMPARE(*messages.back().counter, *all.back().counter);
  }
  // Noise before and between frames is skipped.
  ts::BeastParser noisy;
  const std::vector<ts::TimedMessage> found =
      noisy.feed(QByteArray("noise") + beast.left(200));
  QVERIFY(!found.empty());
  QCOMPARE(noisy.skippedBytes(), int64_t{5});
}

void TestAdsb::receiverClock()
{
  ts::ReceiverClock clock(1000.0);
  QCOMPARE(clock.timeOf(12000000), 1000.0);
  QCOMPARE(clock.timeOf(24000000), 1001.0);
  // Wrapping at 2^48.
  ts::ReceiverClock wrapping(0.0);
  const uint64_t top = (uint64_t{1} << 48U) - 6000000;
  QCOMPARE(wrapping.timeOf(top), 0.0);
  QCOMPARE(wrapping.timeOf(6000000), 1.0);
}

void TestAdsb::trackStore()
{
  ts::MovingTrack track;
  ts::TrackSample a;
  a.timeS = 10.0;
  a.latDeg = 0.0;
  a.lonDeg = 179.9;
  a.heightM = 1000.0;
  a.trackDeg = 350.0F;
  ts::TrackSample b = a;
  b.timeS = 20.0;
  b.lonDeg = -179.9;
  b.heightM = 2000.0;
  b.trackDeg = 10.0F;
  ts::TrackSample duplicate = a;
  duplicate.lonDeg = 5.0;
  // Out of order, with a duplicate.
  track.add(b);
  track.add(a);
  track.add(duplicate);
  QCOMPARE(track.finish(), std::size_t{1});
  QCOMPARE(track.points().size(), std::size_t{2});
  QCOMPARE(track.points().front().lonDeg, 179.9);
  ts::TrackTiming timing;
  const std::optional<ts::TrackSample> middle = track.at(15.0, timing);
  QVERIFY(middle);
  // The shorter way round the antimeridian, and through north.
  QVERIFY(std::fabs(std::fabs(middle->lonDeg) - 180.0) < 1e-9);
  QCOMPARE(middle->heightM, 1500.0);
  QVERIFY(std::fabs(static_cast<double>(middle->trackDeg)) < 1e-4 ||
          std::fabs(static_cast<double>(middle->trackDeg) - 360.0) < 1e-4);
  QVERIFY(!track.at(9.0, timing));
  QCOMPARE(*track.indexAtOrBefore(10.0), std::size_t{0});
  QCOMPARE(*track.indexAtOrBefore(25.0), std::size_t{1});
}

void TestAdsb::trackGapsAndStale()
{
  ts::MovingTrack track;
  for (const double t : {0.0, 1.0, 2.0, 100.0, 101.0}) {
    ts::TrackSample p;
    p.timeS = t;
    p.latDeg = t;
    track.add(p);
  }
  track.finish();
  ts::TrackTiming timing;
  timing.staleS = 20.0;
  timing.gapS = 30.0;
  QCOMPARE(track.pieces(timing).size(), std::size_t{2});
  QCOMPARE(track.at(1.5, timing)->latDeg, 1.5);
  // In the gap: at the last report until stale, then not shown.
  QCOMPARE(track.at(15.0, timing)->latDeg, 2.0);
  QVERIFY(!track.at(30.0, timing));
  QCOMPARE(track.at(100.5, timing)->latDeg, 100.5);
  QCOMPARE(track.at(121.0, timing)->latDeg, 101.0);
  QVERIFY(!track.at(121.5, timing));
}

void TestAdsb::trailModes()
{
  ts::MovingTrack track;
  for (int32_t i = 0; i <= 10; ++i) {
    ts::TrackSample p;
    p.timeS = 10.0 * i;
    p.latDeg = i;
    track.add(p);
  }
  // A gap, then more.
  for (int32_t i = 20; i <= 22; ++i) {
    ts::TrackSample p;
    p.timeS = 10.0 * i;
    p.latDeg = i;
    track.add(p);
  }
  track.finish();
  ts::TrackTiming timing;
  timing.gapS = 30.0;
  ts::TrailSettings off;
  off.mode = ts::TrailMode::Off;
  QVERIFY(track.trail(55.0, off, timing).empty());

  ts::TrailSettings span;
  span.mode = ts::TrailMode::Span;
  span.spanS = 20.0;
  std::vector<std::vector<ts::TrackSample>> runs =
      track.trail(55.0, span, timing);
  QCOMPARE(runs.size(), std::size_t{1});
  // From 35 s to 55 s: 3.5, 4, 5, 5.5.
  QCOMPARE(runs.front().size(), std::size_t{4});
  QCOMPARE(runs.front().front().latDeg, 3.5);
  QCOMPARE(runs.front().back().latDeg, 5.5);
  QCOMPARE(runs.front().back().timeS, 55.0);
  // A span within one report interval.
  span.spanS = 2.0;
  runs = track.trail(55.0, span, timing);
  QCOMPARE(runs.front().size(), std::size_t{2});
  QCOMPARE(runs.front().front().latDeg, 5.3);
  // Moving the clock moves the trail.
  span.spanS = 20.0;
  runs = track.trail(95.0, span, timing);
  QCOMPARE(runs.front().front().latDeg, 7.5);
  QCOMPARE(runs.front().back().latDeg, 9.5);

  ts::TrailSettings all;
  all.mode = ts::TrailMode::All;
  runs = track.trail(215.0, all, timing);
  // Both pieces, broken at the gap; the second up to 21.5.
  QCOMPARE(runs.size(), std::size_t{2});
  QCOMPARE(runs.at(0).size(), std::size_t{11});
  QCOMPARE(runs.at(1).back().latDeg, 21.5);
  // Before the second piece starts, only the first.
  QCOMPARE(track.trail(150.0, all, timing).size(), std::size_t{1});
  // Before anything: nothing.
  QVERIFY(track.trail(-1.0, all, timing).empty());

  const std::optional<ts::TrailSettings> back =
      ts::TrailSettings::fromJson(span.toJson());
  QVERIFY(back && *back == span);
  QVERIFY(!ts::TrailSettings::fromJson(QJsonObject()));
}

void TestAdsb::heights()
{
  // 11 000 m of geopotential (36 089 ft) is 11 019 m geometric.
  QVERIFY(std::fabs(ts::pressureAltitudeToHeightM(11000.0 / 0.3048) -
                    11019.07) < 0.05);
  QCOMPARE(ts::pressureAltitudeToHeightM(0.0), 0.0);
  ts::AircraftHeightSettings settings;
  settings.undulation = [](double, double) { return 47.0; };
  ts::AircraftTrackBuilder builder(settings);
  ts::AircraftReport baro;
  baro.address = 1;
  baro.timeS = 100.0;
  baro.baroAltitudeFt = 10000.0;
  baro.position = ts::CprPosition{50.0, 5.0};
  builder.add(baro);
  ts::AircraftReport geometric;
  geometric.address = 2;
  geometric.timeS = 100.0;
  geometric.geometricAltitudeFt = 10000.0;
  geometric.baroAltitudeFt = 9500.0;
  geometric.position = ts::CprPosition{50.0, 5.0};
  builder.add(geometric);
  ts::AircraftReport ground;
  ground.address = 3;
  ground.timeS = 100.0;
  ground.onGround = true;
  ground.position = ts::CprPosition{50.0, 5.0};
  builder.add(ground);
  const std::vector<ts::AircraftTrack> tracks = builder.finish();
  const ts::TrackSample& b = tracks.at(0).track.points().front();
  QVERIFY(b.heightApproximate);
  QVERIFY(std::fabs(b.heightM -
                    (ts::pressureAltitudeToHeightM(10000.0) + 47.0)) < 1e-9);
  const ts::TrackSample& g = tracks.at(1).track.points().front();
  QVERIFY(!g.heightApproximate);
  QCOMPARE(g.heightM, 3048.0);
  QCOMPARE(tracks.at(1).altitudes.front().baroFt, 9500.0F);
  QVERIFY(std::isnan(tracks.at(2).track.points().front().heightM));
  QCOMPARE(*tracks.at(2).infoAt(100.0)->onGround, true);
}

void TestAdsb::builderOrder()
{
  // Reports out of order are sorted; altitudes stay with their points.
  ts::AircraftTrackBuilder builder;
  for (const double t : {5.0, 3.0, 4.0, 1.0, 2.0, 3.0}) {
    ts::AircraftReport r;
    r.address = 7;
    r.timeS = t;
    r.baroAltitudeFt = 1000.0 * t;
    r.position = ts::CprPosition{t, 0.0};
    builder.add(r);
  }
  QCOMPARE(builder.reportCount(), int64_t{6});
  const std::vector<ts::AircraftTrack> tracks = builder.finish();
  const ts::AircraftTrack& track = tracks.front();
  QCOMPARE(track.track.points().size(), std::size_t{5});
  for (std::size_t i = 0; i < 5; ++i) {
    QCOMPARE(track.track.points().at(i).latDeg, static_cast<double>(i + 1));
    QCOMPARE(track.altitudes.at(i).baroFt,
             static_cast<float>(1000.0 * static_cast<double>(i + 1)));
  }
  QCOMPARE(builder.reportCount(), int64_t{0});
}

void TestAdsb::liveFeed()
{
  // Only when asked for: TS_ADSB_LIVE=host:port:avr or host:port:beast
  // (a receiver's raw AVR or Beast output). Not in CI.
  const QStringList live =
      qEnvironmentVariable("TS_ADSB_LIVE").split(QLatin1Char(':'));
  if (live.size() != 3) {
    QSKIP("Set TS_ADSB_LIVE=host:port:avr|beast to read a live receiver.");
  }
  QTcpSocket socket;
  socket.connectToHost(live.at(0), live.at(1).toUShort());
  QVERIFY2(socket.waitForConnected(5000), qPrintable(socket.errorString()));
  const bool beast = live.at(2) == QLatin1String("beast");
  ts::AdsbDecoder decoder;
  ts::BeastParser parser;
  QByteArray pending;
  int64_t messages = 0;
  int64_t positions = 0;
  std::vector<ts::CprPosition> seen;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 20000) {
    socket.waitForReadyRead(500);
    const QByteArray data = socket.readAll();
    const double now =
        static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    std::vector<QByteArray> frames;
    if (beast) {
      for (const ts::TimedMessage& m : parser.feed(data)) {
        frames.push_back(m.bytes);
      }
    } else {
      pending.append(data);
      qsizetype end = 0;
      while ((end = pending.indexOf('\n')) >= 0) {
        const std::optional<ts::TimedMessage> m =
            ts::parseRawLine(pending.left(end));
        if (m) {
          frames.push_back(m->bytes);
        }
        pending.remove(0, end + 1);
      }
    }
    for (const QByteArray& frame : frames) {
      ++messages;
      const std::optional<ts::AircraftReport> report =
          decoder.decode(frame, now);
      if (report && report->position) {
        ++positions;
        seen.push_back(*report->position);
      }
    }
  }
  qInfo("%s", qPrintable(QStringLiteral("%1 messages, %2 refused, %3 from "
                                        "unknown addresses, %4 positions")
                             .arg(messages)
                             .arg(decoder.counts().refused)
                             .arg(decoder.counts().unknownAddress)
                             .arg(positions)));
  QVERIFY(messages > 0);
  QVERIFY(positions > 0);
  // Every position within 450 NM of the first (one receiver's reach).
  for (const ts::CprPosition& p : seen) {
    QVERIFY(ts::distanceNm(p, seen.front()) < 450.0);
  }
}

QTEST_GUILESS_MAIN(TestAdsb)

#include "tst_adsb.moc"
