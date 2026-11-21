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
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "ts/adsb/adsbdecoder.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftexport.h"
#include "ts/adsb/aircraftlayer.h"
#include "ts/adsb/aircraftreport.h"
#include "ts/adsb/aircrafttracks.h"
#include "ts/adsb/cpr.h"
#include "ts/adsb/modesmessage.h"
#include "ts/adsb/receiverfeed.h"
#include "ts/geo/geodetic.h"
#include "ts/perf/processstats.h"
#include "ts/tracks/movingtrack.h"
#include "ts/tracks/trackdraws.h"

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
// A latitude, then a longitude, as named.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
bool near(ts::CprPosition a, double latDeg, double lonDeg, double metres)
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
                                          const ts::AdsbReadOptions& options,
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

/**
 * Returns @p value's value, or a default one: for after a check that it
 * has one (QVERIFY), which the static analysis can't follow.
 */
template <typename T>
T got(const std::optional<T>& value)
{
  return value.value_or(T());
}

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
  void layerJson();
  void styleFilters();
  void exportTrack();
  void receiverSbs();
  void receiverBeastReconnects();
  void receiverRecordingReadsBack();
  void liveBuilder();
  void dayRecording();
  void readingRate();
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
  QCOMPARE(got(m).downlinkFormat, 17);
  QCOMPARE(got(m).typeCode, 4);
  QCOMPARE(ts::formatIcaoAddress(got(m).address), QStringLiteral("4840D6"));
  QCOMPARE(got(got(m).callsign), QStringLiteral("KLM1023"));
  QVERIFY(!got(m).addressFromParity);
}

void TestAdsb::airbornePosition()
{
  const std::optional<ts::ModeSMessage> even =
      decodeHex("8D40621D58C382D690C8AC2863A7");
  const std::optional<ts::ModeSMessage> odd =
      decodeHex("8D40621D58C386435CC412692AD6");
  QVERIFY(even && odd && even->cpr && odd->cpr);
  QCOMPARE(got(got(even).cpr).lat, 93000U);
  QCOMPARE(got(got(even).cpr).lon, 51372U);
  QCOMPARE(got(got(odd).cpr).lat, 74158U);
  QCOMPARE(got(got(odd).cpr).lon, 50194U);
  QVERIFY(!got(got(even).cpr).odd && got(got(odd).cpr).odd);
  QCOMPARE(got(got(even).altitudeFt), 38000);
  QCOMPARE(ts::cprLongitudeZones(52.2572021484375), 36);
  // The even frame is newer (1457996402 against 1457996400).
  const std::optional<ts::CprPosition> p =
      ts::cprGlobalAirborne(got(got(even).cpr), got(got(odd).cpr), false);
  QVERIFY(p);
  QCOMPARE(got(p).latDeg, 52.2572021484375);
  QCOMPARE(got(p).lonDeg, 3.91937255859375);

  ts::AdsbDecoder decoder;
  QVERIFY(!got(decoder.decode(ts::modeSFromHex("8D40621D58C386435CC412692AD6"),
                              1457996400.0))
               .position);
  const std::optional<ts::AircraftReport> report = decoder.decode(
      ts::modeSFromHex("8D40621D58C382D690C8AC2863A7"), 1457996402.0);
  QVERIFY(report && report->position);
  QCOMPARE(got(got(report).position).latDeg, 52.2572021484375);
  QCOMPARE(got(got(report).baroAltitudeFt), 38000.0);
}

void TestAdsb::localPosition()
{
  const std::optional<ts::ModeSMessage> even =
      decodeHex("8D40621D58C382D690C8AC2863A7");
  const ts::CprPosition p =
      ts::cprLocal(got(got(even).cpr), ts::CprPosition{52.258, 3.918});
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
  QVERIFY(got(got(even).cpr).surface);
  QVERIFY(got(got(even).onGround));
  QCOMPARE(got(got(even).cpr).lat, 115609U);
  QCOMPARE(got(got(odd).cpr).lon, 110269U);
  // The odd frame is newer; the airfield is the reference.
  const std::optional<ts::CprPosition> p =
      ts::cprGlobalSurface(got(got(even).cpr), got(got(odd).cpr), true,
                           ts::CprPosition{51.990, 4.375});
  QVERIFY(p);
  QVERIFY(std::fabs(got(p).latDeg - 52.320607072215964) < 1e-9);
  QVERIFY(std::fabs(got(p).lonDeg - 4.734734671456474) < 1e-9);

  const std::optional<ts::ModeSMessage> next =
      decodeHex("8C4841753A9A153237AEF0F275BE");
  QVERIFY(next && next->cpr);
  const ts::CprPosition local =
      ts::cprLocal(got(got(next).cpr), ts::CprPosition{52.320607, 4.734735});
  QVERIFY(std::fabs(local.latDeg - 52.32056051997815) < 1e-9);
  QVERIFY(std::fabs(local.lonDeg - 4.735735212053571) < 1e-9);
  QCOMPARE(got(got(next).groundSpeedKt), 17.0);
  QCOMPARE(got(got(next).trackDeg), 92.8125);
}

void TestAdsb::velocity()
{
  const std::optional<ts::ModeSMessage> a =
      decodeHex("8D485020994409940838175B284F");
  QVERIFY(a && a->groundSpeedKt && a->trackDeg);
  QCOMPARE(got(a).typeCode, 19);
  QVERIFY(std::fabs(got(got(a).groundSpeedKt) - 159.20) < 0.01);
  QVERIFY(std::fabs(got(got(a).trackDeg) - 182.88) < 0.01);
  QCOMPARE(got(got(a).verticalRateFtMin), -832);
  QVERIFY(!got(a).verticalRateBaro);
  QCOMPARE(got(got(a).gnssMinusBaroFt), 550);

  const std::optional<ts::ModeSMessage> b =
      decodeHex("8DA05F219B06B6AF189400CBC33F");
  QVERIFY(b && b->headingDeg && b->airspeedKt);
  QVERIFY(std::fabs(got(got(b).headingDeg) - 243.98) < 0.01);
  QCOMPARE(got(got(b).airspeedKt), 375.0);
  QVERIFY(got(b).trueAirspeed);
  QCOMPARE(got(got(b).verticalRateFtMin), -2304);
  QVERIFY(got(b).verticalRateBaro);
  QVERIFY(!got(b).groundSpeedKt);
}

void TestAdsb::surveillanceReplies()
{
  const std::optional<ts::ModeSMessage> altitude = decodeHex("2000171806A983");
  QVERIFY(altitude);
  QCOMPARE(got(altitude).downlinkFormat, 4);
  QVERIFY(got(altitude).addressFromParity);
  QCOMPARE(got(got(altitude).altitudeFt), 36000);
  const std::optional<ts::ModeSMessage> identity = decodeHex("2A00516D492B80");
  QVERIFY(identity);
  QCOMPARE(got(identity).downlinkFormat, 5);
  QCOMPARE(got(got(identity).squawk), QStringLiteral("0356"));
}

void TestAdsb::altitudeCodes()
{
  QVERIFY(!ts::decodeAltitudeCode(0));
  // Q bit: 25 ft steps.
  for (int32_t feet = -1000; feet < 50000; feet += 25) {
    const uint32_t n = static_cast<uint32_t>((feet + 1000) / 25);
    const uint32_t code =
        ((n & 0x7E0U) << 2U) | ((n & 0x10U) << 1U) | 0x10U | (n & 0x0FU);
    QCOMPARE(got(ts::decodeAltitudeCode(code)), feet);
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
    QCOMPARE(got(decoded), feet);
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
    QVERIFY2(near(got(global), lat, lon, tolerance),
             qPrintable(QStringLiteral("%1 %2")
                            .arg(got(global).latDeg, 0, 'f', 7)
                            .arg(got(global).lonDeg, 0, 'f', 7)));
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
  for (int32_t step = -40; step < 40; ++step) {
    const double lat = boundary + 0.0005 * step;
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
  QVERIFY(!got(decoder.decode(odd, 1000.0)).position);
  QVERIFY(!got(decoder.decode(even, 1011.0)).position);
  // Then a newer odd within 10 s of that even makes one.
  const std::optional<ts::AircraftReport> report = decoder.decode(odd, 1015.0);
  QVERIFY(report && report->position);
  QVERIFY(near(got(got(report).position), 52.2657801741, 3.9389, 300.0));
  // Next, decoded locally from there.
  const std::optional<ts::AircraftReport> next = decoder.decode(even, 1016.0);
  QVERIFY(next && next->position);
  QCOMPARE(got(got(next).position).latDeg, 52.2572021484375);
  QCOMPARE(decoder.counts().positions, int64_t{2});

  // A receiver far away refuses the positions.
  ts::AdsbDecoderSettings far;
  far.receiver = ts::CprPosition{-33.0, 151.0};
  ts::AdsbDecoder distant(far);
  distant.decode(odd, 1000.0);
  QVERIFY(!got(distant.decode(even, 1001.0)).position);
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
  heard.address = got(altitude).address;
  heard.callsign = QStringLiteral("TEST");
  QVERIFY(decoder.decode(heard, 11.0));
  const std::optional<ts::AircraftReport> report =
      decoder.decode(ts::modeSFromHex("2000171806A983"), 12.0);
  QVERIFY(report);
  QCOMPARE(got(got(report).baroAltitudeFt), 36000.0);
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
    QCOMPARE(got(messages.back().counter), got(all.back().counter));
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
  QVERIFY(std::fabs(std::fabs(got(middle).lonDeg) - 180.0) < 1e-9);
  QCOMPARE(got(middle).heightM, 1500.0);
  QVERIFY(std::fabs(static_cast<double>(got(middle).trackDeg)) < 1e-4 ||
          std::fabs(static_cast<double>(got(middle).trackDeg) - 360.0) < 1e-4);
  QVERIFY(!track.at(9.0, timing));
  QCOMPARE(got(track.indexAtOrBefore(10.0)), std::size_t{0});
  QCOMPARE(got(track.indexAtOrBefore(25.0)), std::size_t{1});
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
  QCOMPARE(got(track.at(1.5, timing)).latDeg, 1.5);
  // In the gap: at the last report until stale, then not shown.
  QCOMPARE(got(track.at(15.0, timing)).latDeg, 2.0);
  QVERIFY(!track.at(30.0, timing));
  QCOMPARE(got(track.at(100.5, timing)).latDeg, 100.5);
  QCOMPARE(got(track.at(121.0, timing)).latDeg, 101.0);
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
  const ts::AircraftInfo* groundInfo = tracks.at(2).infoAt(100.0);
  QVERIFY(groundInfo != nullptr);
  QCOMPARE(groundInfo->onGround.value_or(false), true);
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

void TestAdsb::layerJson()
{
  ts::AircraftSource file;
  file.path = QStringLiteral("/data/day.beast");
  file.format = ts::AdsbFormat::Beast;
  file.startTimeS = 1.7e9;
  file.sbsLocalTime = false;
  file.receiverPosition = ts::CprPosition{52.0, 4.5};
  const std::optional<ts::AircraftSource> fileBack =
      ts::AircraftSource::fromJson(file.toJson());
  QVERIFY(fileBack);
  QCOMPARE(got(fileBack).path, file.path);
  QCOMPARE(got(fileBack).format, ts::AdsbFormat::Beast);
  QCOMPARE(got(fileBack).startTimeS, 1.7e9);
  QVERIFY(!got(fileBack).sbsLocalTime);
  QCOMPARE(got(got(fileBack).receiverPosition).lonDeg, 4.5);
  ts::AircraftSource receiver;
  receiver.origin = ts::AircraftSource::Origin::Receiver;
  receiver.protocol = ts::ReceiverProtocol::Beast;
  receiver.host = QStringLiteral("receiver.local");
  receiver.port = 30005;
  receiver.recordPath = QStringLiteral("/tmp/x.txt");
  const std::optional<ts::AircraftSource> receiverBack =
      ts::AircraftSource::fromJson(receiver.toJson());
  QVERIFY(receiverBack);
  QCOMPARE(got(receiverBack).protocol, ts::ReceiverProtocol::Beast);
  QCOMPARE(got(receiverBack).host, receiver.host);
  QCOMPARE(got(receiverBack).port, uint16_t{30005});
  QCOMPARE(got(receiverBack).recordPath, receiver.recordPath);
  QVERIFY(ts::AircraftSource::isAircraft(receiver.toJson()));
  QVERIFY(!ts::AircraftSource::fromJson(QJsonObject()));
  QJsonObject broken = receiver.toJson();
  broken.insert(QStringLiteral("port"), 0);
  QVERIFY(!ts::AircraftSource::fromJson(broken));

  ts::AircraftStyle style;
  style.color = QColor(10, 20, 30, 200);
  style.labelAltitude = true;
  style.dropLines = true;
  style.trail.mode = ts::TrailMode::All;
  style.colouring = ts::TrailColouring::VerticalRate;
  style.timing.staleS = 90.0;
  style.callsignFilter = QStringLiteral("KLM");
  style.minimumAltitudeFt = 10000.0;
  style.area = ts::LatLonBox{50.0, 170.0, 55.0, -170.0};
  style.hidden = {0xABC001U};
  style.overrides[0xABC002U].trail = ts::TrailSettings{ts::TrailMode::Off, 0.0};
  const ts::AircraftStyle back = ts::AircraftStyle::fromJson(style.toJson());
  QVERIFY(back == style);
  QVERIFY(ts::AircraftStyle::fromJson(QJsonObject()) == ts::AircraftStyle());
  QCOMPARE(back.trailOf(0xABC002U).mode, ts::TrailMode::Off);
  QCOMPARE(back.trailOf(0xABC003U).mode, ts::TrailMode::All);
}

void TestAdsb::styleFilters()
{
  ts::AircraftStyle style;
  QVERIFY(!style.isFiltered());
  QVERIFY(style.shows(1, QString(), 0.0, 0.0, 1000.0));
  style.callsignFilter = QStringLiteral("klm");
  QVERIFY(style.shows(1, QStringLiteral("KLM1023"), 0.0, 0.0, 0.0));
  QVERIFY(!style.shows(1, QStringLiteral("BAW1"), 0.0, 0.0, 0.0));
  style.callsignFilter.clear();
  style.addressFilter = QStringLiteral("48");
  QVERIFY(style.shows(0x4840D6U, QString(), 0.0, 0.0, 0.0));
  QVERIFY(!style.shows(0x3C0000U, QString(), 0.0, 0.0, 0.0));
  style.addressFilter.clear();
  style.minimumAltitudeFt = 10000.0;
  style.maximumAltitudeFt = 20000.0;
  QVERIFY(style.shows(1, QString(), 0.0, 0.0, 15000.0));
  QVERIFY(!style.shows(1, QString(), 0.0, 0.0, 5000.0));
  QVERIFY(!style.shows(1, QString(), 0.0, 0.0, 25000.0));
  // No altitude: not known to be above the minimum.
  QVERIFY(!style.shows(1, QString(), 0.0, 0.0,
                       std::numeric_limits<double>::quiet_NaN()));
  style.minimumAltitudeFt.reset();
  style.maximumAltitudeFt.reset();
  // An area across the antimeridian.
  style.area = ts::LatLonBox{-20.0, 175.0, -10.0, -175.0};
  QVERIFY(style.shows(1, QString(), -15.0, 179.0, 0.0));
  QVERIFY(style.shows(1, QString(), -15.0, -178.0, 0.0));
  QVERIFY(!style.shows(1, QString(), -15.0, 0.0, 0.0));
  style.area.reset();
  style.hidden.insert(7);
  QVERIFY(!style.shows(7, QString(), 0.0, 0.0, 0.0));
}

void TestAdsb::exportTrack()
{
  ts::AdsbReadOptions options;
  const std::vector<ts::AircraftTrack> tracks =
      readTracks("flights-aircraft.jsonl", options);
  const ts::AircraftTrack& track = tracks.front();
  const std::size_t n = track.track.points().size();
  // KML: a gx:Track with a when and a coord for each report.
  const QByteArray kml = ts::exportAircraftTrack(
      track, QStringLiteral("TSO101"), ts::TrackExportFormat::Kml,
      [](double, double) { return 47.0; });
  QCOMPARE(static_cast<std::size_t>(kml.count("<when>")), n);
  QCOMPARE(static_cast<std::size_t>(kml.count("<gx:coord>")), n);
  QVERIFY(kml.contains("<altitudeMode>absolute</altitudeMode>"));
  // Above sea level: the height less the geoid's 47 m.
  const double msl = track.track.points().front().heightM - 47.0;
  QVERIFY(kml.contains(QByteArray::number(msl, 'f', 1)));
  QVERIFY(kml.contains("2026-11-20T12:00:00.000Z"));
  // GPX.
  const QByteArray gpx = ts::exportAircraftTrack(
      track, QStringLiteral("TSO101"), ts::TrackExportFormat::Gpx);
  QCOMPARE(static_cast<std::size_t>(gpx.count("<trkpt ")), n);
  QVERIFY(gpx.contains("<ele>"));
  // CSV: a header and a row a report, the callsign and altitudes in it.
  const QByteArray csv = ts::exportAircraftTrack(
      track, QStringLiteral("TSO101"), ts::TrackExportFormat::Csv);
  const QList<QByteArray> rows = csv.trimmed().split('\n');
  QCOMPARE(static_cast<std::size_t>(rows.size()), n + 1);
  QVERIFY(rows.at(1).contains(",ABC001,TSO101,"));
  QVERIFY(rows.at(1).contains(",30000,"));
  // And it reads back as CSV, at the same places.
  ts::AdsbReadOptions back;
  back.csv = ts::guessCsvColumns(csv.left(512));
  back.csv.time = 1;
  QVERIFY(back.csv.isComplete());
  ts::AircraftTrackBuilder builder;
  const ts::AdsbReadResult result = ts::readAdsbData(
      csv, ts::AdsbFormat::Csv, back,
      [&builder](const ts::AircraftReport& r) { builder.add(r); });
  QCOMPARE(result.problemCount, int64_t{0});
  const std::vector<ts::AircraftTrack> again = builder.finish();
  QCOMPARE(again.front().track.points().size(), n);
  QCOMPARE(ts::trackExportFormatFor(QStringLiteral("a.GPX")),
           ts::TrackExportFormat::Gpx);
  QCOMPARE(ts::trackExportFormatFor(QStringLiteral("a.csv")),
           ts::TrackExportFormat::Csv);
  QCOMPARE(ts::trackExportFormatFor(QStringLiteral("a")),
           ts::TrackExportFormat::Kml);
}

void TestAdsb::receiverSbs()
{
  // A mock receiver sending SBS lines: reports arrive, timed as they come.
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost));
  ts::AircraftSource source;
  source.origin = ts::AircraftSource::Origin::Receiver;
  source.protocol = ts::ReceiverProtocol::Sbs;
  source.host = QStringLiteral("127.0.0.1");
  source.port = server.serverPort();
  ts::ReceiverFeed feed(source, nullptr, nullptr);
  QSignalSpy ready(&feed, &ts::ReceiverFeed::reportsReady);
  feed.start();
  QVERIFY(server.waitForNewConnection(5000));
  QTcpSocket* client = server.nextPendingConnection();
  QVERIFY(QTest::qWaitFor(
      [&]() { return feed.state() == ts::ReceiverFeed::State::Connected; },
      5000));
  const QByteArray lines = readData("flights.sbs").left(4000);
  // Sent in two pieces, split inside a line.
  client->write(lines.left(1500));
  client->flush();
  QVERIFY(QTest::qWaitFor([&]() { return !ready.isEmpty(); }, 5000));
  client->write(lines.mid(1500));
  client->flush();
  std::vector<ts::AircraftReport> reports;
  const double before =
      static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0 - 1.0;
  QVERIFY(QTest::qWaitFor(
      [&]() {
        for (const ts::AircraftReport& r : feed.takeReports()) {
          reports.push_back(r);
        }
        return reports.size() >= 30;
      },
      5000));
  QVERIFY(reports.front().timeS >= before);
  QVERIFY(std::any_of(
      reports.begin(), reports.end(),
      [](const ts::AircraftReport& r) { return r.position.has_value(); }));
  QCOMPARE(feed.connections(), 1);
  feed.stop();
  QCOMPARE(feed.state(), ts::ReceiverFeed::State::Stopped);
}

void TestAdsb::receiverBeastReconnects()
{
  // A Beast receiver that drops the connection: the feed connects again
  // (after a second) and goes on decoding.
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost));
  ts::AircraftSource source;
  source.origin = ts::AircraftSource::Origin::Receiver;
  source.protocol = ts::ReceiverProtocol::Beast;
  source.host = QStringLiteral("127.0.0.1");
  source.port = server.serverPort();
  ts::ReceiverFeed feed(source, nullptr, nullptr);
  feed.start();
  QVERIFY(server.waitForNewConnection(5000));
  QTcpSocket* first = server.nextPendingConnection();
  const QByteArray beast = readData("flights.beast");
  first->write(beast.left(20000));
  first->flush();
  int64_t positions = 0;
  QVERIFY(QTest::qWaitFor(
      [&]() {
        for (const ts::AircraftReport& r : feed.takeReports()) {
          positions += r.position ? 1 : 0;
        }
        return positions > 10;
      },
      5000));
  // Dropped.
  first->disconnectFromHost();
  QVERIFY(QTest::qWaitFor(
      [&]() { return feed.state() == ts::ReceiverFeed::State::Reconnecting; },
      5000));
  QVERIFY(!feed.errorString().isEmpty());
  // Back: it connects again and reads on.
  QVERIFY(
      QTest::qWaitFor([&]() { return server.hasPendingConnections(); }, 5000));
  QTcpSocket* second = server.nextPendingConnection();
  QVERIFY(QTest::qWaitFor(
      [&]() { return feed.state() == ts::ReceiverFeed::State::Connected; },
      5000));
  QCOMPARE(feed.connections(), 2);
  second->write(beast.mid(20000, 20000));
  second->flush();
  int64_t more = 0;
  QVERIFY(QTest::qWaitFor(
      [&]() {
        more += static_cast<int64_t>(feed.takeReports().size());
        return more > 10;
      },
      5000));
  // A receiver that isn't there: it keeps trying, waiting longer.
  server.close();
  second->disconnectFromHost();
  QVERIFY(QTest::qWaitFor(
      [&]() { return feed.state() == ts::ReceiverFeed::State::Reconnecting; },
      5000));
  QVERIFY(feed.retryDelayMs() >= 2000);
  feed.stop();
}

void TestAdsb::receiverRecordingReadsBack()
{
  // Recorded as "time,hex" lines, the session reads back as a recording.
  QTemporaryDir dir;
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost));
  ts::AircraftSource source;
  source.origin = ts::AircraftSource::Origin::Receiver;
  source.protocol = ts::ReceiverProtocol::Avr;
  source.host = QStringLiteral("127.0.0.1");
  source.port = server.serverPort();
  source.recordPath = dir.filePath(QStringLiteral("session.txt"));
  ts::ReceiverFeed feed(source, nullptr, nullptr);
  feed.start();
  QVERIFY(server.waitForNewConnection(5000));
  QTcpSocket* client = server.nextPendingConnection();
  QByteArray avr;
  for (const QByteArray& line : readData("flights-mlat.avr").split('\n')) {
    if (line.size() > 13) {
      avr += '*' + line.mid(13);
      avr += '\n';
    }
    if (avr.size() > 30000) {
      break;
    }
  }
  client->write(avr);
  client->flush();
  int64_t reports = 0;
  QVERIFY(QTest::qWaitFor(
      [&]() {
        reports += static_cast<int64_t>(feed.takeReports().size());
        return feed.messages() >= avr.count('\n');
      },
      5000));
  feed.stop();
  QVERIFY(reports > 100);
  ts::AdsbReadResult result;
  ts::AircraftTrackBuilder builder;
  result = ts::readAdsbFile(
      source.recordPath, ts::AdsbFormat::Unknown, ts::AdsbReadOptions(),
      [&builder](const ts::AircraftReport& r) { builder.add(r); });
  QCOMPARE(result.format, ts::AdsbFormat::Raw);
  QVERIFY(!result.untimed);
  QCOMPARE(result.records, avr.count('\n'));
  QCOMPARE(builder.finish().size(), std::size_t{3});
}

void TestAdsb::liveBuilder()
{
  // Live data: only the aircraft with new reports are built again, and
  // those not heard for long are forgotten.
  ts::AircraftTrackBuilder builder;
  for (int32_t i = 0; i < 10; ++i) {
    for (const uint32_t address : {1U, 2U}) {
      ts::AircraftReport r;
      r.address = address;
      r.timeS = 100.0 + i;
      r.position = ts::CprPosition{static_cast<double>(i), 0.0};
      builder.add(r);
    }
  }
  QCOMPARE(builder.buildChanged().size(), std::size_t{2});
  QVERIFY(builder.buildChanged().empty());
  ts::AircraftReport r;
  r.address = 2;
  r.timeS = 200.0;
  r.position = ts::CprPosition{20.0, 0.0};
  builder.add(r);
  const std::vector<ts::AircraftTrack> changed = builder.buildChanged();
  QCOMPARE(changed.size(), std::size_t{1});
  QCOMPARE(changed.front().track.points().size(), std::size_t{11});
  builder.forgetBefore(150.0);
  QCOMPARE(builder.finish().size(), std::size_t{1});
}

void TestAdsb::dayRecording()
{
  // A day's recording of a busy receiver: 120 aircraft over 8 hours,
  // reported every 2 s (1.7 million reports), built into tracks within
  // the time and memory budgets, and their trails made.
  const ts::ProcessStats before = ts::currentProcessStats();
  QElapsedTimer timer;
  timer.start();
  ts::AircraftTrackBuilder builder;
  const int32_t aircraft = 120;
  const int32_t steps = 8 * 3600 / 2;
  for (int32_t step = 0; step < steps; ++step) {
    for (int32_t a = 0; a < aircraft; ++a) {
      const double t = 1.8e9 + 2.0 * step + 0.01 * a;
      ts::AircraftReport r;
      r.address = 0x400000U + static_cast<uint32_t>(a);
      r.timeS = t;
      // Gentle curves, so the trails keep some of their points.
      const double phase = 0.0002 * step + a;
      r.position = ts::CprPosition{40.0 + 8.0 * std::sin(phase) + 0.05 * a,
                                   -10.0 + 20.0 * std::cos(0.7 * phase)};
      r.baroAltitudeFt = 30000.0 + 5000.0 * std::sin(0.001 * step);
      r.groundSpeedKt = 450.0;
      r.trackDeg = std::fmod(10.0 * phase, 360.0);
      builder.add(r);
    }
  }
  const std::vector<ts::AircraftTrack> tracks = builder.finish();
  const double buildS = static_cast<double>(timer.elapsed()) / 1000.0;
  QCOMPARE(tracks.size(), static_cast<std::size_t>(aircraft));
  std::size_t points = 0;
  for (const ts::AircraftTrack& track : tracks) {
    points += track.track.points().size();
  }
  QCOMPARE(points, static_cast<std::size_t>(aircraft) *
                       static_cast<std::size_t>(steps));
  const ts::ProcessStats after = ts::currentProcessStats();
  const double usedMb = (static_cast<double>(after.peakResidentBytes) -
                         static_cast<double>(before.residentBytes)) /
                        (1024.0 * 1024.0);
  timer.restart();
  ts::TrackTiming timing;
  ts::TrailSettings all;
  all.mode = ts::TrailMode::All;
  ts::TrailLook look;
  std::size_t trailPoints = 0;
  for (const ts::AircraftTrack& track : tracks) {
    for (const ts::SpaceLine& line :
         ts::trailLines(track.track, timing, all, look)) {
      trailPoints += line.points.size();
    }
  }
  const double trailS = static_cast<double>(timer.elapsed()) / 1000.0;
  // Where each is at a time: a binary search each.
  timer.restart();
  int32_t shown = 0;
  for (int32_t i = 0; i < 100; ++i) {
    for (const ts::AircraftTrack& track : tracks) {
      shown += track.track.at(1.8e9 + 3600.0 + i, timing) ? 1 : 0;
    }
  }
  const double placeMs = static_cast<double>(timer.elapsed()) / 100.0;
  qInfo("%s", qPrintable(QStringLiteral("%1 reports built in %2 s, %3 MB; "
                                        "trails %4 points in %5 s; placed in "
                                        "%6 ms")
                             .arg(points)
                             .arg(buildS, 0, 'f', 2)
                             .arg(usedMb, 0, 'f', 0)
                             .arg(trailPoints)
                             .arg(trailS, 0, 'f', 2)
                             .arg(placeMs, 0, 'f', 2)));
  QCOMPARE(shown, 100 * aircraft);
  // Budgets (generous for sanitizer builds): build 30 s, 700 MB, trails
  // 20 s, placing every aircraft well within a frame.
  QVERIFY(buildS < 30.0);
  QVERIFY(usedMb < 700.0);
  QVERIFY(trailS < 20.0);
  QVERIFY(placeMs < 20.0);
  // Simplification keeps a fraction of the points.
  QVERIFY(trailPoints < points / 4);
}

void TestAdsb::readingRate()
{
  // SBS lines read at well over a hundred thousand a second (the budget is
  // lower, for sanitizer builds).
  QTemporaryDir dir;
  const QString path = dir.filePath(QStringLiteral("big.sbs"));
  {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray sbs = readData("flights.sbs");
    for (int32_t i = 0; i < 80; ++i) {
      file.write(sbs);
    }
  }
  QElapsedTimer timer;
  timer.start();
  ts::AdsbReadOptions options;
  options.sbsLocalTime = false;
  int64_t reports = 0;
  const ts::AdsbReadResult result =
      ts::readAdsbFile(path, ts::AdsbFormat::Unknown, options,
                       [&reports](const ts::AircraftReport&) { ++reports; });
  const double seconds =
      std::max(static_cast<double>(timer.elapsed()) / 1000.0, 1e-3);
  qInfo("%s", qPrintable(QStringLiteral("%1 SBS lines in %2 s")
                             .arg(result.records)
                             .arg(seconds, 0, 'f', 2)));
  QVERIFY(result.records > 100000);
  QCOMPARE(result.problemCount, int64_t{0});
  QVERIFY(static_cast<double>(result.records) / seconds > 20000.0);
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
