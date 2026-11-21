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
#include <limits>
#include <optional>
#include <vector>

#include <map>

#include <QFile>
#include <QtTest>

#include "ts/ais/aismessage.h"
#include "ts/ais/aisreaders.h"
#include "ts/ais/vesseldraws.h"
#include "ts/ais/vesseltracks.h"
#include "ts/geo/geodetic.h"
#include "ts/tracks/movingtrack.h"

namespace
{

/**
 * Returns @p value's value, or a default one: for after a check that it
 * has one (QVERIFY), which the static analysis can't follow.
 */
template <typename T>
T got(const std::optional<T>& value)
{
  return value.value_or(T());
}

/** Decodes a whole one-sentence message. */
std::optional<ts::AisMessage> decodeLine(const char* line)
{
  const std::optional<ts::AisSentence> sentence =
      ts::parseAisSentence(QByteArray(line));
  if (!sentence) {
    return std::nullopt;
  }
  return ts::decodeAis(sentence->payload, sentence->fillBits);
}

/** Decodes a message from several sentences, in the order given. */
std::optional<ts::AisMessage> decodeLines(const std::vector<const char*>& lines)
{
  ts::AisAssembler assembler;
  for (const char* line : lines) {
    const std::optional<ts::AisSentence> sentence =
        ts::parseAisSentence(QByteArray(line));
    if (!sentence) {
      return std::nullopt;
    }
    const std::optional<ts::AisAssembler::Payload> whole =
        assembler.add(*sentence);
    if (whole) {
      return ts::decodeAis(whole->payload, whole->fillBits);
    }
  }
  return std::nullopt;
}

bool near(std::optional<double> value, double expected, double tolerance)
{
  return value && std::fabs(*value - expected) <= tolerance;
}

QString dataPath(const char* name)
{
  return QStringLiteral(TS_AIS_DATA_DIR "/") + QLatin1String(name);
}

QByteArray readData(const char* name)
{
  QFile file(dataPath(name));
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/** The fixtures' start, 2026-11-20T12:00Z. */
constexpr double fixtureStartS = 1795176000.0;

/** One report of the made-up harbour, from truth.csv. */
struct Truth {
  double timeS = 0.0;
  double latDeg = 0.0;
  double lonDeg = 0.0;
};

std::map<uint32_t, std::vector<Truth>> readTruth()
{
  std::map<uint32_t, std::vector<Truth>> truth;
  const QList<QByteArray> lines = readData("truth.csv").split('\n');
  for (int32_t i = 1; i < lines.size(); ++i) {
    const QList<QByteArray> f = lines.at(i).split(',');
    if (f.size() < 4) {
      continue;
    }
    truth[f.at(1).toUInt()].push_back(
        Truth{f.at(0).toDouble(), f.at(2).toDouble(), f.at(3).toDouble()});
  }
  return truth;
}

/** Reads a fixture into tracks. */
std::vector<ts::VesselTrack> readTracks(const char* name,
                                        const ts::AisReadOptions& options,
                                        ts::AisReadResult* result = nullptr)
{
  ts::VesselTrackBuilder builder;
  const ts::AisReadResult read = ts::readAisFile(
      dataPath(name), ts::AisFormat::Unknown, options,
      [&builder](const ts::VesselReport& report) { builder.add(report); });
  if (result != nullptr) {
    *result = read;
  }
  return builder.finish();
}

/** Metres between two points (small distances). */
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
double metresBetween(double lat1, double lon1, double lat2, double lon2)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const double north = (lat2 - lat1) * 111195.0;
  const double east =
      (lon2 - lon1) * 111195.0 * std::cos(lat1 * 3.14159265358979 / 180.0);
  return std::hypot(north, east);
}

const ts::VesselTrack* trackOf(const std::vector<ts::VesselTrack>& tracks,
                               uint32_t mmsi)
{
  for (const ts::VesselTrack& track : tracks) {
    if (track.mmsi == mmsi) {
      return &track;
    }
  }
  return nullptr;
}

} // namespace

/**
 * Tests of ts/ais. The sentences and their values are pyais's tests
 * (MIT licence, see data/README.md).
 */
class TestAis : public QObject
{
  Q_OBJECT

private slots:
  void sentences();
  void tagBlocks();
  void classA();
  void turnRates();
  void baseStation();
  void staticVoyage();
  void staticVoyageOneSentence();
  void fragmentsOutOfOrder();
  void fragmentsInterleaved();
  void sarAircraft();
  void classB();
  void classBNotAvailable();
  void classBStatic();
  void aidToNavigation();
  void staticClassB();
  void longRange();
  void shortAndUnknown();
  void detectFormats();
  void readersAgree_data();
  void readersAgree();
  void harbourStations();
  void untimedLog();
  void timesInLines();
  void malformedLines();
  void csvColumns();
  void staleTimes();
  void liveBuilder();
  void typeColours();
  void symbols();
  void hulls();
  void courseVectors();
};

void TestAis::sentences()
{
  ts::AisSentenceError error = ts::AisSentenceError::None;
  const std::optional<ts::AisSentence> s = ts::parseAisSentence(
      "!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C", &error);
  QVERIFY(s);
  QCOMPARE(error, ts::AisSentenceError::None);
  QCOMPARE(got(s).count, 1);
  QCOMPARE(got(s).channel, QByteArray("B"));
  QCOMPARE(got(s).sequence, -1);
  QVERIFY(!got(s).own);
  // Checksum.
  QVERIFY(!ts::parseAisSentence(
      "!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5D", &error));
  QCOMPARE(error, ts::AisSentenceError::Checksum);
  // Not AIS.
  QVERIFY(!ts::parseAisSentence(
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
      &error));
  QCOMPARE(error, ts::AisSentenceError::NotAis);
  // Truncated: fields missing (checksum right for what is there).
  const QByteArray body = "AIVDM,1,1,,B";
  const QByteArray truncated = '!' + body + '*' +
                               QByteArray::number(ts::nmeaChecksum(body), 16)
                                   .rightJustified(2, '0')
                                   .toUpper();
  QVERIFY(!ts::parseAisSentence(truncated, &error));
  QCOMPARE(error, ts::AisSentenceError::Broken);
  // Other talkers and VDO.
  const std::optional<ts::AisSentence> own =
      ts::parseAisSentence("!AIVDO,1,1,,A,B5NJ;PP2aUl4ot5Isbl6GwsUkP06,0*35");
  QVERIFY(own && got(own).own);
  QVERIFY(
      ts::parseAisSentence("!ANVDM,1,1,,A,35O5WS1000r9FSHF@jBoLCACp000,0*42"));
  // Fill bits.
  QCOMPARE(ts::aisPayloadBits("88888888880", 2).size(), std::size_t{64});
  QVERIFY(ts::aisPayloadBits("a b", 0).empty());
}

void TestAis::tagBlocks()
{
  // AIS-catcher's and others' NMEA 4.0 tag blocks: time and source.
  const QByteArray sentence = "!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C";
  const QByteArray block = "c:1700000000,s:receiver1";
  const QByteArray line = '\\' + block + '*' +
                          QByteArray::number(ts::nmeaChecksum(block), 16)
                              .rightJustified(2, '0')
                              .toUpper() +
                          '\\' + sentence;
  const std::optional<ts::AisSentence> s = ts::parseAisSentence(line);
  QVERIFY(s);
  QCOMPARE(got(got(s).tagTimeS), 1700000000.0);
  QCOMPARE(got(s).tagSource, QByteArray("receiver1"));
  // Milliseconds.
  const QByteArray msBlock = "c:1700000000500";
  const QByteArray msLine = '\\' + msBlock + '*' +
                            QByteArray::number(ts::nmeaChecksum(msBlock), 16)
                                .rightJustified(2, '0')
                                .toUpper() +
                            '\\' + sentence;
  QCOMPARE(got(got(ts::parseAisSentence(msLine)).tagTimeS), 1700000000.5);
  // A broken tag block's checksum.
  ts::AisSentenceError error = ts::AisSentenceError::None;
  QVERIFY(!ts::parseAisSentence("\\c:1700000000*00\\" + sentence, &error));
  QCOMPARE(error, ts::AisSentenceError::Checksum);
}

void TestAis::classA()
{
  const std::optional<ts::AisMessage> a =
      decodeLine("!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C");
  QVERIFY(a);
  QCOMPARE(got(a).type, 1);
  QCOMPARE(got(a).mmsi, 366053209U);
  QCOMPARE(got(got(a).status), ts::AisNavStatus::RestrictedManoeuvre);
  QCOMPARE(got(got(a).turnDegMin), 0.0);
  QCOMPARE(got(got(a).speedKn), 0.0);
  QVERIFY(!got(a).accurate);
  QVERIFY(near(got(a).lonDeg, -122.341618, 1e-6));
  QVERIFY(near(got(a).latDeg, 37.802118, 1e-6));
  QVERIFY(near(got(a).courseDeg, 219.3, 1e-9));
  QCOMPARE(got(got(a).headingDeg), 1);
  QCOMPARE(got(got(a).second), 59);

  // Heading 511 and rate of turn −128 are "not available".
  const std::optional<ts::AisMessage> b =
      decodeLine("!AIVDM,1,1,,A,15NPOOPP00o?b=bE`UNv4?w428D;,0*24");
  QVERIFY(b);
  QCOMPARE(got(b).mmsi, 367533950U);
  QCOMPARE(got(got(b).status), ts::AisNavStatus::UnderWayEngine);
  QVERIFY(!got(b).turnDegMin);
  QVERIFY(!got(b).headingDeg);
  QVERIFY(got(b).accurate);
  QVERIFY(near(got(b).latDeg, 37.8084, 5e-5));
  QVERIFY(near(got(b).lonDeg, -122.4082, 5e-5));
  // Course 360 is "not available" too.
  QVERIFY(!got(b).courseDeg);
  QCOMPARE(got(got(b).second), 34);

  const std::optional<ts::AisMessage> c =
      decodeLine("!AIVDM,1,1,,B,181:Kjh01ewHFRPDK1s3IRcn06sd,0*08");
  QCOMPARE(got(c).mmsi, 538090443U);
  QVERIFY(near(got(c).speedKn, 10.9, 1e-9));
  QVERIFY(near(got(c).courseDeg, 87.0, 1e-9));
  // This one's checksum is wrong in the source (7E, not 67), so its
  // payload is decoded directly.
  QVERIFY(!decodeLine("!AIVDM,1,1,,A,13@nePh01>PjcO4PGReoJEmL0HJg,0*67"));
  QVERIFY(near(got(ts::decodeAis("13@nePh01>PjcO4PGReoJEmL0HJg", 0)).speedKn,
               7.8, 1e-9));

  // Type 3, moored, and from another talker at anchor.
  const std::optional<ts::AisMessage> moored =
      decodeLine("!AIVDM,1,1,,A,35NSH95001G?wopE`beasVk@0E5:,0*6F");
  QCOMPARE(got(moored).type, 3);
  QCOMPARE(got(got(moored).status), ts::AisNavStatus::Moored);
  QVERIFY(near(got(moored).speedKn, 0.1, 1e-9));
  QVERIFY(near(got(moored).courseDeg, 254.2, 1e-9));
  QCOMPARE(got(got(moored).headingDeg), 217);
  const std::optional<ts::AisMessage> anchored =
      decodeLine("!ANVDM,1,1,,A,35O5WS1000r9FSHF@jBoLCACp000,0*42");
  QCOMPARE(got(anchored).mmsi, 368142220U);
  QCOMPARE(got(got(anchored).status), ts::AisNavStatus::AtAnchor);
  QVERIFY(near(got(anchored).lonDeg, -81.84302, 1e-5));
  QVERIFY(near(got(anchored).latDeg, 38.906152, 1e-6));
  QCOMPARE(got(got(anchored).headingDeg), 104);
  // Type 2 (its checksum is wrong in the source too: 6A, not 68).
  QCOMPARE(got(ts::decodeAis("23aFfl0P00PCR?0MEB@h0?w020S7", 0)).type, 2);
}

void TestAis::turnRates()
{
  const std::vector<std::pair<const char*, double>> cases = {
      {"!AIVDM,1,1,,A,14QIG<5620KF@Gl:L9DI4o8N0P00,0*28", 26.0},
      {"!AIVDM,1,1,,B,13u><=gsQj0mQW:Q1<wRL28P0@:4,0*32", -14.0},
      {"!AIVDM,1,1,,A,14SSRt021O0?bK@MO7H6QUA600Rg,0*12", 3.0},
      {"!AIVDM,1,1,,2,13aB:Hhuh0PHjEFNKJg@11sH08J=,0*1E", -4.0},
      {"!AIVDM,1,1,,A,16:VFv0k0I`KQPpFATG4SgvT40:v,0*7B", -121.0},
      {"!AIVDM,1,1,,B,16:D3F0:15`5ogh<O?bk>1Dd2L1<,0*0B", 71.0},
  };
  // pyais gives degrees a minute, rounded.
  for (const std::pair<const char*, double>& c : cases) {
    const std::optional<ts::AisMessage> m = decodeLine(c.first);
    QVERIFY2(m && got(m).turnDegMin, c.first);
    QCOMPARE(std::round(got(got(m).turnDegMin)), c.second);
  }
}

void TestAis::baseStation()
{
  const std::optional<ts::AisMessage> a =
      decodeLine("!AIVDM,1,1,,A,403OviQuMGCqWrRO9>E6fE700@GO,0*4D");
  QVERIFY(a);
  QCOMPARE(got(a).type, 4);
  QVERIFY(near(got(a).lonDeg, -76.352362, 1e-6));
  QVERIFY(near(got(a).latDeg, 36.883767, 1e-6));
  QVERIFY(got(a).accurate);
  const std::vector<int32_t> utc = got(got(a).utc);
  QCOMPARE(utc.size(), std::size_t{6});
  QCOMPARE(utc.at(0), 2007);
  QCOMPARE(utc.at(1), 5);
  QCOMPARE(utc.at(2), 14);
  QCOMPARE(utc.at(4), 57);
  QCOMPARE(utc.at(5), 39);
  const std::optional<ts::AisMessage> b =
      decodeLine("!AIVDM,1,1,,B,403OtVAv>lba;o?Ia`E`4G?02H6k,0*44");
  QCOMPARE(got(b).mmsi, 3669145U);
  QVERIFY(near(got(b).lonDeg, -122.4648, 5e-5));
  QVERIFY(near(got(b).latDeg, 37.7943, 5e-5));
  QCOMPARE(got(got(b).utc), (std::vector<int32_t>{2019, 11, 9, 10, 41, 11}));
}

void TestAis::staticVoyage()
{
  const std::optional<ts::AisMessage> m =
      decodeLines({"!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@"
                   "Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C",
                   "!AIVDM,2,2,1,A,88888888880,2*25"});
  QVERIFY(m);
  QCOMPARE(got(m).type, 5);
  QCOMPARE(got(got(m).callsign), QStringLiteral("3FOF8"));
  QCOMPARE(got(got(m).name), QStringLiteral("EVER DIADEM"));
  // Cargo.
  QVERIFY(got(got(m).shipType) >= 70 && got(got(m).shipType) <= 79);
  const ts::AisDimensions d = got(got(m).dimensions);
  QCOMPARE(d.toBow, 225);
  QCOMPARE(d.toStern, 70);
  QCOMPARE(d.toPort, 1);
  QCOMPARE(d.toStarboard, 31);
  QVERIFY(near(got(m).draughtM, 12.2, 1e-9));
  QCOMPARE(got(got(m).destination), QStringLiteral("NEW YORK"));
}

void TestAis::staticVoyageOneSentence()
{
  // Its checksum is wrong in the source (36, not 35): the payload
  // directly.
  const std::optional<ts::AisMessage> m =
      ts::decodeAis("53ktrJ82>ia4=50<0020<5=@Dhv0t8T@u<0000001PV854Si0;mR@"
                    "CPH13p0hDm1C3h0000",
                    2);
  QVERIFY(m);
  QCOMPARE(got(m).mmsi, 255801960U);
  QCOMPARE(got(got(m).imo), 9356945U);
  QCOMPARE(got(got(m).callsign), QStringLiteral("CQPC"));
  QCOMPARE(got(got(m).name), QStringLiteral("CASTELO OBIDOS"));
  // Ship type "not available" (0).
  QVERIFY(!got(m).shipType);
  QCOMPARE(got(got(m).dimensions).toBow, 12);
  QCOMPARE(got(got(m).dimensions).toStern, 38);
  QCOMPARE(got(got(m).dimensions).toPort, 8);
  QCOMPARE(got(got(m).dimensions).toStarboard, 5);
  const ts::AisEta eta = got(got(m).eta);
  QCOMPARE(eta.month, 2);
  QCOMPARE(eta.day, 7);
  QCOMPARE(eta.hour, 17);
  QCOMPARE(eta.minute, 0);
  QVERIFY(near(got(m).draughtM, 4.7, 1e-9));
  QCOMPARE(got(got(m).destination), QStringLiteral("VIANA DO CASTELO"));
}

void TestAis::fragmentsOutOfOrder()
{
  const std::optional<ts::AisMessage> m =
      decodeLines({"!AIVDM,2,2,4,A,000000000000000,2*20",
                   "!AIVDM,2,1,4,A,55O0W7`00001L@gCWGA2uItLth@DqtL5@"
                   "F22220j1h742t0Ht0000000,0*08"});
  QVERIFY(m);
  QCOMPARE(got(m).mmsi, 368060190U);
}

void TestAis::fragmentsInterleaved()
{
  // Two messages' fragments interleaved, with a one-sentence message
  // between: both are completed.
  ts::AisAssembler assembler;
  std::vector<ts::AisAssembler::Payload> done;
  for (const char* line : {"!AIVDM,2,1,1,A,538CQ>02A;h?D9QC800pu8@T>"
                           "0P4l9E8L0000017Ah:;;5r50Ahm5;C0,0*07",
                           "!AIVDM,2,1,4,A,55O0W7`00001L@gCWGA2uItLth@DqtL5@"
                           "F22220j1h742t0Ht0000000,0*08",
                           "!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C",
                           "!AIVDM,2,2,4,A,000000000000000,2*20",
                           "!AIVDM,2,2,1,A,F@V@00000000000,2*35"}) {
    const std::optional<ts::AisAssembler::Payload> whole =
        assembler.add(got(ts::parseAisSentence(QByteArray(line))));
    if (whole) {
      done.push_back(*whole);
    }
  }
  QCOMPARE(done.size(), std::size_t{3});
  QCOMPARE(got(ts::decodeAis(done.at(1).payload, done.at(1).fillBits)).mmsi,
           368060190U);
  const std::optional<ts::AisMessage> nordic =
      ts::decodeAis(done.at(2).payload, done.at(2).fillBits);
  QCOMPARE(got(nordic).mmsi, 210035000U);
  QCOMPARE(got(got(nordic).name), QStringLiteral("NORDIC HAMBURG"));
  // A first fragment whose second never comes is dropped in time.
  ts::AisAssembler waiting;
  waiting.add(
      got(ts::parseAisSentence("!AIVDM,2,1,1,A,538CQ>02A;h?D9QC800pu8@T>"
                               "0P4l9E8L0000017Ah:;;5r50Ahm5;C0,0*07")));
  const ts::AisSentence single = got(
      ts::parseAisSentence("!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C"));
  for (int64_t i = 0; i <= ts::AisAssembler::maximumPending; ++i) {
    waiting.add(single);
  }
  QCOMPARE(waiting.dropped(), int64_t{1});
}

void TestAis::sarAircraft()
{
  const std::optional<ts::AisMessage> m =
      decodeLine("!AIVDM,1,1,,B,91b55wi;hbOS@OdQAC062Ch2089h,0*30");
  QVERIFY(m);
  QCOMPARE(got(m).type, 9);
  QCOMPARE(got(m).mmsi, 111232511U);
  QCOMPARE(got(got(m).altitudeM), 303);
  QCOMPARE(got(got(m).speedKn), 42.0);
  QVERIFY(near(got(m).lonDeg, -6.27884, 1e-5));
  QVERIFY(near(got(m).latDeg, 58.144, 1e-6));
  QVERIFY(near(got(m).courseDeg, 154.5, 1e-9));
  QCOMPARE(got(got(m).second), 15);
}

void TestAis::classB()
{
  const std::optional<ts::AisMessage> m =
      decodeLine("!AIVDM,1,1,,A,B5NJ;PP005l4ot5Isbl03wsUkP06,0*76");
  QVERIFY(m);
  QCOMPARE(got(m).type, 18);
  QVERIFY(got(m).isClassB());
  QCOMPARE(got(m).mmsi, 367430530U);
  QCOMPARE(got(got(m).speedKn), 0.0);
  QVERIFY(near(got(m).latDeg, 37.79, 0.005));
  QVERIFY(near(got(m).lonDeg, -122.27, 0.005));
  QCOMPARE(got(got(m).courseDeg), 0.0);
  QVERIFY(!got(m).headingDeg);
  QCOMPARE(got(got(m).second), 55);
  const std::optional<ts::AisMessage> fast =
      decodeLine("!AIVDO,1,1,,A,B5NJ;PP2aUl4ot5Isbl6GwsUkP06,0*35");
  QVERIFY(near(got(fast).speedKn, 67.8, 1e-9));
  QVERIFY(near(got(fast).courseDeg, 10.1, 1e-9));
}

void TestAis::classBNotAvailable()
{
  // Speed 102.3, longitude 181, latitude 91, course 360 and heading 511:
  // all "not available", so missing.
  const std::optional<ts::AisMessage> m =
      decodeLine("!AIVDO,1,1,,,B>qc:003wk?8mP=18D3Q3wgTiT;T,0*13");
  QVERIFY(m);
  QCOMPARE(got(m).mmsi, 1000000000U);
  QVERIFY(!got(m).speedKn);
  QVERIFY(!got(m).latDeg);
  QVERIFY(!got(m).lonDeg);
  QVERIFY(!got(m).courseDeg);
  QVERIFY(!got(m).headingDeg);
  QCOMPARE(got(got(m).second), 31);
}

void TestAis::classBStatic()
{
  const std::optional<ts::AisMessage> m =
      decodeLine("!AIVDM,1,1,,B,C5N3SRgPEnJGEBT>NhWAwwo862PaLELTBJ:"
                 "V00000000S0D:R220,0*0B");
  QVERIFY(m);
  QCOMPARE(got(m).type, 19);
  QCOMPARE(got(m).mmsi, 367059850U);
  QVERIFY(near(got(m).speedKn, 8.7, 1e-9));
  QVERIFY(near(got(m).latDeg, 29.543695, 1e-6));
  QVERIFY(near(got(m).lonDeg, -88.810394, 1e-5));
  QVERIFY(near(got(m).courseDeg, 335.9, 0.05));
  QVERIFY(!got(m).headingDeg);
  QCOMPARE(got(got(m).second), 46);
  QCOMPARE(got(got(m).name), QStringLiteral("CAPT.J.RIMES"));
  QCOMPARE(got(got(m).shipType), 70);
  const ts::AisDimensions d = got(got(m).dimensions);
  QCOMPARE(d.toBow, 5);
  QCOMPARE(d.toStern, 21);
  QCOMPARE(d.toPort, 4);
  QCOMPARE(d.toStarboard, 4);
}

void TestAis::aidToNavigation()
{
  const std::optional<ts::AisMessage> a = decodeLines(
      {"!AIVDM,2,1,7,B,E4eHJhPR37q0000000000000000KUOSc=rq4h00000a,0*4A",
       "!AIVDM,2,2,7,B,@20,4*54"});
  QVERIFY(a);
  QCOMPARE(got(a).type, 21);
  QCOMPARE(got(a).mmsi, 316021442U);
  // A reference point.
  QCOMPARE(got(got(a).aidType), 1);
  QCOMPARE(got(got(a).name), QStringLiteral("DFO2"));
  QVERIFY(got(a).accurate);
  QVERIFY(near(got(a).latDeg, 48.65457, 1e-5));
  QVERIFY(near(got(a).lonDeg, -123.429155, 1e-6));
  QVERIFY(!got(a).dimensions);
  // A starboard-hand mark, and a long name with its extension.
  const std::optional<ts::AisMessage> b = decodeLine(
      "!AIVDM,1,1,,B,E>lt;KLab21@1bb@I@@@@@@@@@@D8k2tnmvs000003v0@,2*52");
  QCOMPARE(got(b).mmsi, 995036013U);
  QCOMPARE(got(got(b).aidType), 25);
  QCOMPARE(got(got(b).name), QStringLiteral("STDB CUT 2"));
  QVERIFY(near(got(b).lonDeg, 115.691833, 1e-6));
  QVERIFY(near(got(b).latDeg, -32.004333, 1e-6));
  const std::optional<ts::AisMessage> long_ = decodeLine(
      "!AIVDM,1,1,,B,E>jHDL1W73nWaanah7S39T7a2h;wror=@5nL`A2AISd002CQ1PDS@0,"
      "4*39");
  QCOMPARE(got(got(long_).name), QStringLiteral("NNG-OSS-S OFFSHORE WINDFARM"));
}

void TestAis::staticClassB()
{
  const std::optional<ts::AisMessage> a =
      decodeLine("!AIVDM,1,1,,A,H52KMeDU653hhhi0000000000000,0*1A");
  QVERIFY(a);
  QCOMPARE(got(a).mmsi, 338091445U);
  QCOMPARE(got(got(a).part), 1);
  // Pleasure craft.
  QCOMPARE(got(got(a).shipType), 37);
  QVERIFY(!got(a).callsign);
  QVERIFY(!got(a).dimensions);
  const std::optional<ts::AisMessage> b =
      decodeLine("!AIVDO,1,1,,A,H8=;nnT000000000000000Wg8Jb0,0*26");
  QCOMPARE(got(b).mmsi, 550696666U);
  QCOMPARE(got(got(b).dimensions).toBow, 317);
  QCOMPARE(got(got(b).dimensions).toStern, 456);
  QCOMPARE(got(got(b).dimensions).toPort, 26);
  QCOMPARE(got(got(b).dimensions).toStarboard, 42);
  // An auxiliary craft gives its mother ship, not its size.
  const std::optional<ts::AisMessage> aux =
      decodeLine("!AIVDO,1,1,,A,H>W@vFTe6??406t2??21J0Wg8Jb0,0*6F");
  QCOMPARE(got(aux).mmsi, 980696666U);
  QCOMPARE(got(got(aux).shipType), 45);
  QCOMPARE(got(got(aux).callsign), QStringLiteral("BOOBAZ"));
  QVERIFY(!got(aux).dimensions);
  const std::optional<ts::AisMessage> c =
      decodeLine("!ANVDM,1,1,,B,H5NuKGTUCBD8SaUG4:omol0hC33t,0*54");
  QCOMPARE(got(got(c).callsign), QStringLiteral("WDJ7574"));
  QCOMPARE(got(got(c).dimensions).toBow, 6);
  QCOMPARE(got(got(c).dimensions).toStern, 19);
  // Part A: the name.
  const std::optional<ts::AisMessage> partA =
      decodeLine("!AIVDO,1,1,,A,H1mg=5@EP4m0hF1<PU000000000,2*77");
  QCOMPARE(got(partA).mmsi, 123456789U);
  QCOMPARE(got(got(partA).part), 0);
}

void TestAis::longRange()
{
  const std::optional<ts::AisMessage> m =
      decodeLine("!AIVDM,1,1,,B,KC5E2b@U19PFdLbMuc5=ROv62<7m,0*16");
  QVERIFY(m);
  QCOMPARE(got(m).type, 27);
  QCOMPARE(got(m).mmsi, 206914217U);
  QCOMPARE(got(got(m).status), ts::AisNavStatus::NotUnderCommand);
  QVERIFY(near(got(m).lonDeg, 137.023333, 1e-6));
  QVERIFY(near(got(m).latDeg, 4.84, 1e-6));
  QCOMPARE(got(got(m).speedKn), 57.0);
  QCOMPARE(got(got(m).courseDeg), 167.0);
  const std::optional<ts::AisMessage> south =
      decodeLine("!AIVDO,1,1,,A,K01;FQh?PbtE3P00,0*75");
  QCOMPARE(got(south).mmsi, 1234567U);
  QVERIFY(near(got(south).lonDeg, -13.368333, 1e-6));
  QVERIFY(near(got(south).latDeg, -50.121667, 1e-6));
}

void TestAis::shortAndUnknown()
{
  // A type 5 that stops early: not decoded.
  QVERIFY(!decodeLine("!AIVDM,2,1,1,A,55?MbV02;H,0*00"));
  QVERIFY(!ts::decodeAis("55?MbV02;H", 0));
  // A type not read: its type is still given, to be counted.
  int32_t type = -1;
  QVERIFY(!ts::decodeAis("8000000000000", 0, &type));
  QCOMPARE(type, 8);
  QVERIFY(!ts::decodeAis(QByteArray(), 0, &type));
  QCOMPARE(type, 0);
}

void TestAis::detectFormats()
{
  QCOMPARE(ts::detectAisFormat(readData("harbour-tagged.nmea").left(4096)),
           ts::AisFormat::Nmea);
  QCOMPARE(ts::detectAisFormat(readData("harbour-prefixed.nmea").left(4096)),
           ts::AisFormat::Nmea);
  QCOMPARE(ts::detectAisFormat(readData("harbour-noaa.csv").left(4096)),
           ts::AisFormat::Csv);
  QCOMPARE(ts::detectAisFormat("hello\n"), ts::AisFormat::Unknown);
  QCOMPARE(ts::aisFormatFromId(ts::aisFormatId(ts::AisFormat::Nmea)),
           ts::AisFormat::Nmea);
  QCOMPARE(ts::aisFormatFromId(ts::aisFormatId(ts::AisFormat::Csv)),
           ts::AisFormat::Csv);
}

void TestAis::readersAgree_data()
{
  QTest::addColumn<QString>("file");
  QTest::addColumn<double>("toleranceM");
  QTest::newRow("tag blocks") << QStringLiteral("harbour-tagged.nmea") << 1.0;
  QTest::newRow("time prefixes")
      << QStringLiteral("harbour-prefixed.nmea") << 1.0;
  QTest::newRow("noaa csv") << QStringLiteral("harbour-noaa.csv") << 2.0;
}

void TestAis::readersAgree()
{
  // Every format of the made-up harbour gives each vessel at the places
  // the truth has, at its times.
  QFETCH(QString, file);
  QFETCH(double, toleranceM);
  ts::AisReadOptions options;
  options.csv = ts::guessAisCsvColumns(readData("harbour-noaa.csv").left(4096));
  ts::AisReadResult result;
  const std::vector<ts::VesselTrack> tracks =
      readTracks(file.toUtf8().constData(), options, &result);
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QCOMPARE(result.problemCount, int64_t{0});
  QCOMPARE(result.checksumErrors, int64_t{0});
  QVERIFY(!result.untimed);
  const std::map<uint32_t, std::vector<Truth>> truth = readTruth();
  int32_t vessels = 0;
  for (const std::pair<const uint32_t, std::vector<Truth>>& entry : truth) {
    const ts::VesselTrack* track = trackOf(tracks, entry.first);
    if (entry.first >= 235000000U && entry.first < 236000000U) {
      ++vessels;
      QVERIFY2(track != nullptr, qPrintable(QString::number(entry.first)));
    }
    if (track == nullptr) {
      continue; // the CSV has vessels only
    }
    for (const Truth& t : entry.second) {
      const std::optional<std::size_t> i =
          track->track.indexAtOrBefore(t.timeS);
      QVERIFY(i);
      const ts::TrackSample& p = track->track.points().at(got(i));
      QCOMPARE(p.timeS, t.timeS);
      QVERIFY2(metresBetween(p.latDeg, p.lonDeg, t.latDeg, t.lonDeg) <=
                   toleranceM,
               qPrintable(QStringLiteral("%1 at %2")
                              .arg(entry.first)
                              .arg(t.timeS - fixtureStartS)));
    }
  }
  QCOMPARE(vessels, 5);
  // Names, from type 5 and 24 (NMEA) or the columns (CSV).
  const ts::VesselTrack* cargo = trackOf(tracks, 235000001U);
  QVERIFY(cargo != nullptr);
  QCOMPARE(cargo->lastName(), QStringLiteral("TSO CARGO ONE"));
  const ts::VesselTrack* yacht = trackOf(tracks, 235000004U);
  QVERIFY(yacht != nullptr);
  QCOMPARE(yacht->lastName(), QStringLiteral("TSO YACHT"));
  const ts::VesselInfo* yachtInfo = yacht->infoAt(fixtureStartS + 300.0);
  QVERIFY(yachtInfo != nullptr);
  QVERIFY(yachtInfo->classB);
  QCOMPARE(yachtInfo->shipType, 36);
  QCOMPARE(yachtInfo->dimensions.toBow + yachtInfo->dimensions.toStern, 12);
  // The yacht sends no heading.
  QVERIFY(std::isnan(yacht->motion.front().headingDeg));
}

void TestAis::harbourStations()
{
  ts::AisReadOptions options;
  ts::AisReadResult result;
  const std::vector<ts::VesselTrack> tracks =
      readTracks("harbour-tagged.nmea", options, &result);
  // Five vessels, a buoy, a base station and a SAR aircraft.
  QCOMPARE(tracks.size(), std::size_t{8});
  QCOMPARE(result.incomplete, int64_t{0});
  const ts::VesselTrack* cargo = trackOf(tracks, 235000001U);
  const ts::VesselInfo* info = cargo->infoAt(fixtureStartS + 100.0);
  QCOMPARE(info->callsign, QStringLiteral("TSO1"));
  QCOMPARE(info->imo, 9000001U);
  QCOMPARE(info->shipType, 70);
  QCOMPARE(info->dimensions.toBow, 150);
  QCOMPARE(info->draughtM, 11.5);
  QCOMPARE(info->destination, QStringLiteral("SOUTHAMPTON"));
  QVERIFY(info->eta.has_value());
  QVERIFY(!info->classB);
  // Under way: heading and course both, course along the track.
  const ts::TrackSample& p = cargo->track.points().at(3);
  QVERIFY(std::fabs(static_cast<double>(p.trackDeg) - 45.0) < 0.2);
  QVERIFY(std::fabs(static_cast<double>(cargo->motion.at(3).headingDeg) -
                    45.0) < 1.0);
  QVERIFY(std::fabs(static_cast<double>(p.speedMS) - 12.0 * 1852.0 / 3600.0) <
          0.06);
  // The tug turns and has a gap in reception.
  const ts::VesselTrack* tug = trackOf(tracks, 235000002U);
  QVERIFY(!std::isnan(tug->motion.front().turnDegMin));
  ts::TrackTiming timing;
  timing.gapS = 60.0;
  QCOMPARE(tug->track.pieces(timing).size(), std::size_t{2});
  // The ferry is moored, the tanker at anchor.
  QCOMPARE(trackOf(tracks, 235000003U)->infoAt(fixtureStartS + 100)->status,
           ts::AisNavStatus::Moored);
  QCOMPARE(trackOf(tracks, 235000005U)->infoAt(fixtureStartS + 100)->status,
           ts::AisNavStatus::AtAnchor);
  // Other stations.
  const ts::VesselTrack* buoy = trackOf(tracks, 992351001U);
  QCOMPARE(buoy->info.front().kind, ts::AisStationKind::AidToNavigation);
  QCOMPARE(buoy->lastName(), QStringLiteral("TSO BUOY 1"));
  QCOMPARE(buoy->info.front().aidType, 14);
  QCOMPARE(trackOf(tracks, 2350001U)->info.front().kind,
           ts::AisStationKind::BaseStation);
  const ts::VesselTrack* aircraft = trackOf(tracks, 111235001U);
  QCOMPARE(aircraft->info.front().kind, ts::AisStationKind::SarAircraft);
  QCOMPARE(aircraft->track.points().front().heightM, 300.0);
}

void TestAis::untimedLog()
{
  // No times: a second a sentence from the start, in order.
  ts::AisReadOptions options;
  options.startTimeS = 1000.0;
  ts::AisReadResult result;
  const std::vector<ts::VesselTrack> tracks =
      readTracks("harbour-untimed.nmea", options, &result);
  QVERIFY(result.untimed);
  QVERIFY(!tracks.empty());
  double first = 1e300;
  double last = 0.0;
  for (const ts::VesselTrack& track : tracks) {
    first = std::min(first, track.track.firstTimeS());
    last = std::max(last, track.track.lastTimeS());
  }
  QCOMPARE(first, 1000.0);
  QVERIFY(last <= 1000.0 + static_cast<double>(result.records));
}

void TestAis::timesInLines()
{
  // A Unix time after the checksum, and one before the sentence.
  ts::NmeaAisParser parser;
  const ts::NmeaAisParser::Result after = parser.parse(
      "!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C,1700000000", 5.0);
  QVERIFY(after.report);
  QCOMPARE(got(after.report).timeS, 1700000000.0);
  const ts::NmeaAisParser::Result before = parser.parse(
      "1700000001.5 !AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C", 5.0);
  QCOMPARE(got(before.report).timeS, 1700000001.5);
  const ts::NmeaAisParser::Result iso = parser.parse(
      "2026-11-20 12:00:00;!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C",
      5.0);
  QCOMPARE(got(iso.report).timeS, fixtureStartS);
  const ts::NmeaAisParser::Result none =
      parser.parse("!AIVDM,1,1,,B,15M67FC000G?ufbE`FepT@3n00Sa,0*5C", 5.0);
  QCOMPARE(got(none.report).timeS, 5.0);
  QVERIFY(!none.timeS);
}

void TestAis::malformedLines()
{
  QByteArray log = readData("harbour-prefixed.nmea");
  QList<QByteArray> lines = log.split('\n');
  lines.insert(2, "garbage");
  lines.insert(
      5,
      "2026-11-20T12:00:00Z !AIVDM,1,1,,A,13P7@hm000Or6NbM6TvkQ2l00000,0*1D");
  // A type 8 (binary broadcast): counted, not a problem.
  lines.insert(8, "!AIVDM,1,1,,A,85Mwp`1Kf3aCnsNvBWLi=wQuNhA5t43N`5nCuI=p<"
                  "IBfVqnMgPGs,0*47");
  log = lines.join('\n');
  ts::AisReadOptions options;
  int64_t reports = 0;
  const ts::AisReadResult result =
      ts::readAisData(log, ts::AisFormat::Unknown, options,
                      [&reports](const ts::VesselReport&) { ++reports; });
  QCOMPARE(result.format, ts::AisFormat::Nmea);
  QCOMPARE(result.problemCount, int64_t{1});
  QCOMPARE(result.problems.front().line, int64_t{3});
  QCOMPARE(result.checksumErrors, int64_t{1});
  QCOMPARE(result.otherMessages.size(), std::size_t{1});
  QCOMPARE(result.otherMessages.begin()->first, 8);
  QVERIFY(reports > 200);
  // Not AIS at all.
  QVERIFY(!ts::readAisData("hello\n", ts::AisFormat::Unknown, options,
                           [](const ts::VesselReport&) {})
               .error.isEmpty());
}

void TestAis::csvColumns()
{
  const ts::AisCsvColumns noaa =
      ts::guessAisCsvColumns(readData("harbour-noaa.csv").left(4096));
  QVERIFY(noaa.isComplete());
  QVERIFY(noaa.header);
  QCOMPARE(noaa.mmsi, 0);
  QCOMPARE(noaa.time, 1);
  QCOMPARE(noaa.latitude, 2);
  QCOMPARE(noaa.longitude, 3);
  QCOMPARE(noaa.speed, 4);
  QCOMPARE(noaa.course, 5);
  QCOMPARE(noaa.heading, 6);
  QCOMPARE(noaa.name, 7);
  QCOMPARE(noaa.imo, 8);
  QCOMPARE(noaa.callsign, 9);
  QCOMPARE(noaa.shipType, 10);
  QCOMPARE(noaa.status, 11);
  QCOMPARE(noaa.length, 12);
  QCOMPARE(noaa.width, 13);
  QCOMPARE(noaa.draught, 14);
  QCOMPARE(noaa.transceiver, 16);
  const ts::AisCsvColumns back = ts::AisCsvColumns::fromJson(noaa.toJson());
  QCOMPARE(back.transceiver, 16);
  QCOMPARE(back.separator, QLatin1Char(','));
  // NOAA's lengths and widths: the antenna in the middle; IMO numbers.
  ts::AisReadOptions options;
  options.csv = noaa;
  const std::vector<ts::VesselTrack> tracks =
      readTracks("harbour-noaa.csv", options);
  const ts::VesselInfo* tanker =
      trackOf(tracks, 235000005U)->infoAt(fixtureStartS + 10.0);
  QCOMPARE(tanker->dimensions.toBow, 125);
  QCOMPARE(tanker->dimensions.toStern, 125);
  QCOMPARE(tanker->dimensions.toPort, 22);
  QCOMPARE(tanker->imo, 9000005U);
  QCOMPARE(tanker->status, ts::AisNavStatus::AtAnchor);
  // Bad rows reported with their lines.
  const QByteArray bad = "MMSI,BaseDateTime,LAT,LON\n"
                         "235000001,2026-11-20T12:00:00,50.8,-1.3\n"
                         "x,2026-11-20T12:00:00,50.8,-1.3\n"
                         "235000001,never,50.8,-1.3\n"
                         "235000001,2026-11-20T12:00:00,95,-1.3\n";
  options.csv = ts::guessAisCsvColumns(bad);
  const ts::AisReadResult result = ts::readAisData(
      bad, ts::AisFormat::Csv, options, [](const ts::VesselReport&) {});
  QCOMPARE(result.reports, int64_t{1});
  QCOMPARE(result.problemCount, int64_t{3});
  QCOMPARE(result.problems.at(0).line, int64_t{3});
  QCOMPARE(result.problems.at(2).line, int64_t{5});
}

void TestAis::staleTimes()
{
  ts::VesselInfo a;
  // Class A under way: ten reports at 2–10 s, at least six minutes.
  QCOMPARE(ts::vesselStaleS(a, 12.0), 360.0);
  QCOMPARE(ts::vesselStaleS(a, 12.0, 0.0), 100.0);
  QCOMPARE(ts::vesselStaleS(a, 20.0, 0.0), 60.0);
  QCOMPARE(ts::vesselStaleS(a, 30.0, 0.0), 20.0);
  // Moored or slow: three minutes a report.
  a.status = ts::AisNavStatus::Moored;
  QCOMPARE(ts::vesselStaleS(a, 12.0), 1800.0);
  a.status = ts::AisNavStatus::UnderWayEngine;
  QCOMPARE(ts::vesselStaleS(a, 1.0), 1800.0);
  // Class B: 30 s moving, 3 minutes slow.
  ts::VesselInfo b;
  b.classB = true;
  QCOMPARE(ts::vesselStaleS(b, 5.0, 0.0), 300.0);
  QCOMPARE(ts::vesselStaleS(b, 1.0, 0.0), 1800.0);
  // Base stations and aids to navigation: an hour.
  ts::VesselInfo aid;
  aid.kind = ts::AisStationKind::AidToNavigation;
  QCOMPARE(ts::vesselStaleS(aid, 0.0), 3600.0);
}

void TestAis::liveBuilder()
{
  ts::VesselTrackBuilder builder;
  for (int32_t i = 0; i < 5; ++i) {
    ts::VesselReport r;
    r.mmsi = 1;
    r.timeS = 100.0 + i;
    r.latDeg = 50.0;
    r.lonDeg = static_cast<double>(i) * 0.001;
    builder.add(r);
  }
  QCOMPARE(builder.buildChanged().size(), std::size_t{1});
  QVERIFY(builder.buildChanged().empty());
  // A static report without a position still makes info.
  ts::VesselReport name;
  name.mmsi = 1;
  name.timeS = 110.0;
  name.name = QStringLiteral("NAMED");
  builder.add(name);
  const std::vector<ts::VesselTrack> changed = builder.buildChanged();
  QCOMPARE(changed.front().lastName(), QStringLiteral("NAMED"));
  QCOMPARE(changed.front().track.points().size(), std::size_t{5});
  builder.forgetBefore(200.0);
  QVERIFY(builder.finish().empty());
}

void TestAis::typeColours()
{
  QCOMPARE(ts::shipTypeGroup(70), ts::ShipTypeGroup::Cargo);
  QCOMPARE(ts::shipTypeGroup(79), ts::ShipTypeGroup::Cargo);
  QCOMPARE(ts::shipTypeGroup(84), ts::ShipTypeGroup::Tanker);
  QCOMPARE(ts::shipTypeGroup(60), ts::ShipTypeGroup::Passenger);
  QCOMPARE(ts::shipTypeGroup(40), ts::ShipTypeGroup::HighSpeed);
  QCOMPARE(ts::shipTypeGroup(52), ts::ShipTypeGroup::TugSpecial);
  QCOMPARE(ts::shipTypeGroup(31), ts::ShipTypeGroup::TugSpecial);
  QCOMPARE(ts::shipTypeGroup(30), ts::ShipTypeGroup::Fishing);
  QCOMPARE(ts::shipTypeGroup(36), ts::ShipTypeGroup::Pleasure);
  QCOMPARE(ts::shipTypeGroup(37), ts::ShipTypeGroup::Pleasure);
  QCOMPARE(ts::shipTypeGroup(0), ts::ShipTypeGroup::Unknown);
  QCOMPARE(ts::shipTypeGroup(99), ts::ShipTypeGroup::Other);
  // Every group of the legend has its own colour.
  std::vector<std::array<std::uint8_t, 4>> seen;
  for (const ts::ShipTypeGroup group : ts::shipTypeLegend()) {
    const std::array<std::uint8_t, 4> c = ts::shipTypeColour(group);
    QVERIFY(std::find(seen.begin(), seen.end(), c) == seen.end());
    seen.push_back(c);
  }
  QCOMPARE(ts::speedColour(0.0), ts::speedColour(-1.0));
  QCOMPARE(ts::speedColour(30.0), ts::speedColour(40.0));
  QVERIFY(ts::speedColour(2.0) != ts::speedColour(8.0));
}

void TestAis::symbols()
{
  ts::TrackSample at;
  at.latDeg = 50.0;
  at.lonDeg = -1.0;
  at.trackDeg = 30.0F;
  at.speedMS = 5.0F;
  ts::VesselMotion motion;
  ts::VesselInfo info;
  // No heading: the course.
  ts::GlobeMarker m = ts::vesselMarker(at, motion, info, 47.0, {}, 8.0);
  QCOMPARE(m.shape, ts::MarkerShape::Ship);
  QCOMPARE(m.headingDeg, 30.0);
  // At sea level (the geoid's 47 m).
  QVERIFY(std::fabs(ts::ecefToGeodetic(m.positionEcef).heightM - 47.0) < 1e-3);
  // A heading wins.
  motion.headingDeg = 35.0F;
  QCOMPARE(ts::vesselMarker(at, motion, info, 0.0, {}, 8.0).headingDeg, 35.0);
  // Moored or at anchor: round.
  info.status = ts::AisNavStatus::Moored;
  QCOMPARE(ts::vesselMarker(at, motion, info, 0.0, {}, 8.0).shape,
           ts::MarkerShape::ShipStopped);
  info.status = ts::AisNavStatus::AtAnchor;
  QCOMPARE(ts::vesselMarker(at, motion, info, 0.0, {}, 8.0).shape,
           ts::MarkerShape::ShipStopped);
  // Neither heading nor course: round too.
  ts::TrackSample still = at;
  still.trackDeg = std::numeric_limits<float>::quiet_NaN();
  info.status = ts::AisNavStatus::UnderWayEngine;
  QCOMPARE(
      ts::vesselMarker(still, ts::VesselMotion(), info, 0.0, {}, 8.0).shape,
      ts::MarkerShape::ShipStopped);
  // Other stations.
  ts::VesselInfo aid;
  aid.kind = ts::AisStationKind::AidToNavigation;
  QCOMPARE(ts::vesselMarker(at, motion, aid, 0.0, {}, 8.0).shape,
           ts::MarkerShape::Diamond);
  ts::VesselInfo base;
  base.kind = ts::AisStationKind::BaseStation;
  QCOMPARE(ts::vesselMarker(at, motion, base, 0.0, {}, 8.0).shape,
           ts::MarkerShape::Square);
  ts::VesselInfo sar;
  sar.kind = ts::AisStationKind::SarAircraft;
  ts::TrackSample high = at;
  high.heightM = 300.0;
  const ts::GlobeMarker aircraft =
      ts::vesselMarker(high, motion, sar, 0.0, {}, 8.0);
  QCOMPARE(aircraft.shape, ts::MarkerShape::Aircraft);
  QVERIFY(std::fabs(ts::ecefToGeodetic(aircraft.positionEcef).heightM - 300.0) <
          1e-3);
}

void TestAis::hulls()
{
  // 150 m to the bow, 50 to the stern, 16 to each side: 200 by 32, the
  // middle 50 m ahead of the antenna.
  ts::TrackSample at;
  at.trackDeg = 45.0F;
  ts::VesselMotion motion;
  motion.headingDeg = 45.0F;
  ts::VesselInfo info;
  info.dimensions = ts::AisDimensions{150, 50, 16, 16};
  const ts::GlobeMarker m = ts::vesselMarker(at, motion, info, 0.0, {}, 8.0);
  QCOMPARE(m.hullLengthM, 200.0);
  QCOMPARE(m.hullWidthM, 32.0);
  QCOMPARE(m.hullForwardM, 50.0);
  QCOMPARE(m.hullStarboardM, 0.0);
  // An antenna 3 m to port of the middle.
  info.dimensions = ts::AisDimensions{10, 10, 1, 7};
  QCOMPARE(ts::vesselMarker(at, motion, info, 0.0, {}, 8.0).hullStarboardM,
           3.0);
  // Unknown dimensions: no hull.
  info.dimensions = ts::AisDimensions();
  QCOMPARE(ts::vesselMarker(at, motion, info, 0.0, {}, 8.0).hullLengthM, 0.0);
}

void TestAis::courseVectors()
{
  // 10 knots east for 6 minutes: a nautical mile east.
  ts::TrackSample at;
  at.latDeg = 0.0;
  at.lonDeg = 0.0;
  at.speedMS = static_cast<float>(10.0 * 1852.0 / 3600.0);
  at.trackDeg = 90.0F;
  const ts::SpaceLine line = ts::courseVector(at, 6.0, 0.0, {});
  QCOMPARE(line.points.size(), std::size_t{2});
  const double metres = ts::length(line.points.at(1).positionEcef -
                                   line.points.at(0).positionEcef);
  // On a sphere of the mean radius: at the equator the ellipsoid is 0.1 %
  // larger.
  QVERIFY(std::fabs(metres - 1852.0) < 3.0);
  QVERIFY(line.points.at(1).lonDeg > 0.0);
  QVERIFY(std::fabs(line.points.at(1).latDeg) < 1e-9);
  // Not moving: nothing.
  at.speedMS = 0.0F;
  QVERIFY(ts::courseVector(at, 6.0, 0.0, {}).points.empty());
}

QTEST_GUILESS_MAIN(TestAis)

#include "tst_ais.moc"
