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
#include <cstdint>
#include <optional>

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "concurrency.h"
#include "ts/layers/layertreemodel.h"
#include "ts/time/layertime.h"
#include "ts/time/programclock.h"
#include "ts/time/timeextent.h"
#include "ts/wms/wmssource.h"
#include "ts/wmts/wmtssource.h"

namespace
{

QDateTime utc(const char* text)
{
  return ts::parseIsoTime(QLatin1String(text), QDateTime())
      .value_or(QDateTime());
}

/** A WMS source with a time dimension following the clock. */
QJsonObject timedWmsSource(const QString& extent)
{
  ts::WmsSource wms;
  wms.url = QUrl(QStringLiteral("https://example.org/wms"));
  wms.layers = QStringList{QStringLiteral("clouds")};
  wms.styles = QStringList{QString()};
  QJsonObject source = wms.toJson();
  ts::LayerTime time;
  time.extent = extent;
  time.defaultValue = QStringLiteral("2026-11-20");
  source.insert(QStringLiteral("time"), time.toJson());
  return source;
}

} // namespace

/**
 * Unit tests for ts/time: ISO 8601 times and durations, time extents
 * (lists and intervals), layer time settings, the source drawn at a time,
 * and the program clock.
 */
class TestTime : public QObject
{
  Q_OBJECT

private slots:
  void periods_data()
  {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<int32_t>("months");
    QTest::addColumn<int32_t>("days");
    QTest::addColumn<qint64>("seconds");
    QTest::newRow("day") << "P1D" << true << 0 << 1 << qint64{0};
    QTest::newRow("hours") << "PT6H" << true << 0 << 0 << qint64{21600};
    QTest::newRow("month") << "P1M" << true << 1 << 0 << qint64{0};
    QTest::newRow("year") << "P1Y" << true << 12 << 0 << qint64{0};
    QTest::newRow("weeks") << "P2W" << true << 0 << 14 << qint64{0};
    QTest::newRow("all") << "P1Y2M10DT2H30M5S" << true << 14 << 10
                         << qint64{9005};
    QTest::newRow("minutes") << "pt10m" << true << 0 << 0 << qint64{600};
    QTest::newRow("empty") << "P" << false << 0 << 0 << qint64{0};
    QTest::newRow("empty time") << "P1DT" << false << 0 << 0 << qint64{0};
    QTest::newRow("zero") << "P0D" << false << 0 << 0 << qint64{0};
    QTest::newRow("no P") << "1D" << false << 0 << 0 << qint64{0};
    QTest::newRow("fraction") << "P0.5D" << false << 0 << 0 << qint64{0};
  }

  void periods()
  {
    QFETCH(QString, text);
    QFETCH(bool, valid);
    QFETCH(int32_t, months);
    QFETCH(int32_t, days);
    QFETCH(qint64, seconds);
    const std::optional<ts::TimePeriod> period = ts::TimePeriod::parse(text);
    QCOMPARE(period.has_value(), valid);
    if (period) {
      QCOMPARE(period->years * 12 + period->months, months);
      QCOMPARE(period->days, days);
      QCOMPARE(static_cast<qint64>(period->seconds), seconds);
    }
  }

  void isoTimes_data()
  {
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    QTest::newRow("year") << "2004" << "2004-01-01T00:00:00.000Z";
    QTest::newRow("month") << "2004-07" << "2004-07-01T00:00:00.000Z";
    QTest::newRow("day") << "2004-07-15" << "2004-07-15T00:00:00.000Z";
    QTest::newRow("minute")
        << "2004-07-15T12:30Z" << "2004-07-15T12:30:00.000Z";
    QTest::newRow("second")
        << "2004-07-15T12:30:15Z" << "2004-07-15T12:30:15.000Z";
    QTest::newRow("milli") << "2004-07-15T12:30:15.250Z"
                           << "2004-07-15T12:30:15.250Z";
    QTest::newRow("no zone is UTC")
        << "2004-07-15T12:30:15" << "2004-07-15T12:30:15.000Z";
    QTest::newRow("offset")
        << "2004-07-15T12:30:00+02:00" << "2004-07-15T10:30:00.000Z";
    QTest::newRow("negative offset")
        << "2004-07-15T12:30:00-0530" << "2004-07-15T18:00:00.000Z";
    QTest::newRow("current") << "current" << "2026-11-20T09:00:00.000Z";
    QTest::newRow("bad month") << "2004-13-01" << "";
    QTest::newRow("words") << "yesterday" << "";
  }

  void isoTimes()
  {
    QFETCH(QString, text);
    QFETCH(QString, expected);
    const std::optional<QDateTime> time =
        ts::parseIsoTime(text, utc("2026-11-20T09:00:00Z"));
    QCOMPARE(time.has_value(), !expected.isEmpty());
    if (time) {
      QCOMPARE(time->toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) +
                   QLatin1Char('Z'),
               expected);
      QCOMPARE(time->offsetFromUtc(), 0);
    }
  }

  void lists()
  {
    // Listed instants, out of order and with spaces; written back as given.
    const ts::TimeExtent extent = ts::TimeExtent::parse(
        QStringLiteral("2004-03-01, 2004-01-01,2004-02-01"));
    QCOMPARE(extent.count(), int64_t{3});
    QCOMPARE(extent.first().value_or(QDateTime()), utc("2004-01-01"));
    QCOMPARE(extent.last().value_or(QDateTime()), utc("2004-03-01"));
    QCOMPARE(extent.format(
                 extent.atOrBefore(utc("2004-02-15")).value_or(QDateTime())),
             QStringLiteral("2004-02-01"));
    // Before the first: the first; after the last: the last.
    QCOMPARE(extent.atOrBefore(utc("1999-01-01")).value_or(QDateTime()),
             utc("2004-01-01"));
    QCOMPARE(extent.atOrBefore(utc("2030-01-01")).value_or(QDateTime()),
             utc("2004-03-01"));
    QCOMPARE(extent.after(utc("2004-01-01")).value_or(QDateTime()),
             utc("2004-02-01"));
    QVERIFY(!extent.after(utc("2004-03-01")).has_value());
    QCOMPARE(extent.before(utc("2004-02-01")).value_or(QDateTime()),
             utc("2004-01-01"));
    QVERIFY(!extent.before(utc("2004-01-01")).has_value());
    QVERIFY(std::abs(extent.shortestStepSeconds().value_or(0.0) -
                     29 * 86400.0) < 1.0);
    QVERIFY(ts::TimeExtent::parse(QString()).isEmpty());
    QVERIFY(!ts::TimeExtent().atOrBefore(utc("2004-01-01")).has_value());
    // Unreadable parts are left out.
    QCOMPARE(
        ts::TimeExtent::parse(QStringLiteral("soon,2004-01-01,x/y")).count(),
        int64_t{1});
  }

  void dailyInterval()
  {
    // GIBS-like daily imagery since 2000.
    const ts::TimeExtent extent =
        ts::TimeExtent::parse(QStringLiteral("2000-02-24/2026-11-20/P1D"));
    QCOMPARE(extent.count(),
             QDate(2000, 2, 24).daysTo(QDate(2026, 11, 20)) + 1);
    QCOMPARE(
        extent.format(
            extent.atOrBefore(utc("2010-06-15T18:00Z")).value_or(QDateTime())),
        QStringLiteral("2010-06-15"));
    QCOMPARE(extent.atOrBefore(utc("2030-01-01")).value_or(QDateTime()),
             utc("2026-11-20"));
    QCOMPARE(extent.after(utc("2010-06-15T18:00Z")).value_or(QDateTime()),
             utc("2010-06-16"));
    QCOMPARE(extent.before(utc("2010-06-15")).value_or(QDateTime()),
             utc("2010-06-14"));
    QCOMPARE(extent.shortestStepSeconds().value_or(0.0), 86400.0);
    // Several intervals with a gap.
    const ts::TimeExtent gaps = ts::TimeExtent::parse(
        QStringLiteral("2020-01-01/2020-01-03/P1D,2020-02-01/2020-02-02/P1D"));
    QCOMPARE(gaps.count(), int64_t{5});
    QCOMPARE(gaps.atOrBefore(utc("2020-01-20")).value_or(QDateTime()),
             utc("2020-01-03"));
    QCOMPARE(gaps.after(utc("2020-01-03")).value_or(QDateTime()),
             utc("2020-02-01"));
  }

  void monthlyAndHourly()
  {
    const ts::TimeExtent months =
        ts::TimeExtent::parse(QStringLiteral("2004-01-01/2004-12-01/P1M"));
    QCOMPARE(months.count(), int64_t{12});
    QCOMPARE(months.format(
                 months.atOrBefore(utc("2004-05-17")).value_or(QDateTime())),
             QStringLiteral("2004-05-01"));
    QVERIFY(ts::LayerTime::isMonthly(months));
    QVERIFY(ts::LayerTime::isMonthly(ts::TimeExtent::parse(QStringLiteral(
        "2004-01,2004-02,2004-03,2004-04,2004-05,2004-06,2004-07,2004-08,"
        "2004-09,2004-10,2004-11,2004-12"))));
    QVERIFY(!ts::LayerTime::isMonthly(
        ts::TimeExtent::parse(QStringLiteral("2004-01-01/2005-01-01/P1M"))));
    // Six-hourly forecasts, written to the second.
    const ts::TimeExtent hours = ts::TimeExtent::parse(
        QStringLiteral("2026-11-20T00:00:00Z/2026-11-21T00:00:00Z/PT6H"));
    QCOMPARE(hours.count(), int64_t{5});
    QCOMPARE(
        hours.format(
            hours.atOrBefore(utc("2026-11-20T13:10Z")).value_or(QDateTime())),
        QStringLiteral("2026-11-20T12:00:00Z"));
    // Continuous: any time in it.
    const ts::TimeExtent continuous =
        ts::TimeExtent::parse(QStringLiteral("2000-01-01/2001-01-01"));
    QCOMPARE(
        continuous.atOrBefore(utc("2000-06-01T06:00Z")).value_or(QDateTime()),
        utc("2000-06-01T06:00Z"));
    QCOMPARE(continuous.format(utc("2000-06-01T06:00Z")),
             QStringLiteral("2000-06-01T06:00:00Z"));
    // Up to "current".
    const ts::TimeExtent current = ts::TimeExtent::parse(
        QStringLiteral("2020/current/P1Y"), utc("2026-11-20T09:00Z"));
    QCOMPARE(current.count(), int64_t{7});
    QCOMPARE(current.format(current.last().value_or(QDateTime())),
             QStringLiteral("2026"));
  }

  void layerTimes()
  {
    ts::LayerTime time;
    time.extent = QStringLiteral("2004-01-01/2004-12-01/P1M");
    time.defaultValue = QStringLiteral("2004-07-01");
    QVERIFY(time.followsClock());
    QCOMPARE(time.valueAt(utc("2004-03-10")), QStringLiteral("2004-03-01"));
    QCOMPARE(time.valueAt(utc("2026-03-10")), QStringLiteral("2004-12-01"));
    time.mode = ts::LayerTime::Mode::MonthOfYear;
    QCOMPARE(time.valueAt(utc("2026-03-10")), QStringLiteral("2004-03-01"));
    QCOMPARE(time.valueAt(utc("1990-11-30T23:00Z")),
             QStringLiteral("2004-11-01"));
    time.mode = ts::LayerTime::Mode::Fixed;
    time.value = QStringLiteral("2004-08-01");
    QVERIFY(!time.followsClock());
    QCOMPARE(time.valueAt(utc("2026-03-10")), QStringLiteral("2004-08-01"));
    time.value.clear();
    QCOMPARE(time.valueAt(utc("2026-03-10")), QStringLiteral("2004-07-01"));

    // JSON.
    time.mode = ts::LayerTime::Mode::MonthOfYear;
    QJsonObject source;
    source.insert(QStringLiteral("time"), time.toJson());
    const std::optional<ts::LayerTime> back = ts::LayerTime::fromSource(source);
    if (!back) {
      QFAIL("back is missing");
    }
    QCOMPARE(back->mode, ts::LayerTime::Mode::MonthOfYear);
    QCOMPARE(back->extent, time.extent);
    QCOMPARE(back->defaultValue, time.defaultValue);
    QVERIFY(!ts::LayerTime::fromSource(QJsonObject{}).has_value());
    QVERIFY(!ts::LayerTime::fromSource(
        QJsonObject{{QStringLiteral("time"), QJsonObject{}}}));
  }

  void requestForEachTime_data()
  {
    QTest::addColumn<QString>("clock");
    QTest::addColumn<QString>("expected");
    QTest::newRow("in range") << "2026-11-03T15:00Z" << "2026-11-03";
    QTest::newRow("midnight") << "2026-11-04T00:00Z" << "2026-11-04";
    QTest::newRow("before") << "2025-01-01T00:00Z" << "2026-11-01";
    QTest::newRow("after") << "2027-01-01T00:00Z" << "2026-11-20";
  }

  void requestForEachTime()
  {
    // The clock's time picks TIME in GetMap, and each time is a source of
    // its own (its own cache).
    QFETCH(QString, clock);
    QFETCH(QString, expected);
    const QJsonObject source =
        timedWmsSource(QStringLiteral("2026-11-01/2026-11-20/P1D"));
    const QJsonObject drawn = ts::sourceAtTime(source, utc(qPrintable(clock)));
    QString error;
    const std::optional<ts::WmsSource> wms =
        ts::WmsSource::fromJson(drawn, &error);
    if (!wms) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(wms->dimensions.size(), std::size_t{1});
    QCOMPARE(wms->dimensions.front().second, expected);
    const QUrlQuery query(ts::getMapUrl(*wms, ts::TileKey{2, 1, 1}));
    QCOMPARE(query.queryItemValue(QStringLiteral("TIME")), expected);
    QVERIFY(wms->id().endsWith(QStringLiteral("|time=") + expected));
    const std::optional<ts::WmsSource> plain =
        ts::WmsSource::fromJson(source, &error);
    if (!plain) {
      QFAIL("plain is missing");
    }
    QVERIFY(plain->id() != wms->id());
    // Without dimensions the id is as before Phase 12 (existing caches).
    QVERIFY(!plain->id().contains(QLatin1String("time")));
  }

  void wmtsTimeFollows()
  {
    // A WMTS source's Time dimension is replaced, not added.
    QJsonObject source;
    source.insert(QStringLiteral("kind"), QStringLiteral("wmts"));
    // Appended, not braced: QJsonArray{QJsonArray{…}} copies the inner
    // array in Qt 6.8.
    QJsonArray dimensions;
    dimensions.append(
        QJsonArray{QStringLiteral("Time"), QStringLiteral("2026-01-01")});
    source.insert(QStringLiteral("dimensions"), dimensions);
    ts::LayerTime time;
    time.dimension = QStringLiteral("Time");
    time.extent = QStringLiteral("2026-01-01,2026-02-01");
    source.insert(QStringLiteral("time"), time.toJson());
    const QJsonObject drawn = ts::sourceAtTime(source, utc("2026-02-10"));
    const QJsonArray drawnDimensions =
        drawn.value(QStringLiteral("dimensions")).toArray();
    QCOMPARE(drawnDimensions.size(), 1);
    QCOMPARE(drawnDimensions.at(0).toArray().at(1).toString(),
             QStringLiteral("2026-02-01"));
    // A source without time is left alone.
    QJsonObject plain{{QStringLiteral("kind"), QStringLiteral("solid")}};
    QCOMPARE(ts::sourceAtTime(plain, utc("2026-02-10")), plain);
  }

  void stackAtTime()
  {
    ts::LayerStack stack;
    ts::ImageryStackEntry entry;
    entry.definition.source =
        timedWmsSource(QStringLiteral("2026-11-01/2026-11-20/P1D"));
    stack.imagery.push_back(entry);
    const ts::LayerStack drawn =
        ts::stackAtTime(stack, utc("2026-11-05T10:00Z"));
    QCOMPARE(drawn.imagery.front()
                 .definition.source.value(QStringLiteral("dimensions"))
                 .toArray()
                 .at(0)
                 .toArray()
                 .at(1)
                 .toString(),
             QStringLiteral("2026-11-05"));
  }

  void programClock()
  {
    ts::ProgramClock clock;
    QVERIFY(std::abs(clock.time().secsTo(QDateTime::currentDateTimeUtc())) <
            60);
    QSignalSpy changed(&clock, &ts::ProgramClock::timeChanged);
    clock.setTime(utc("2004-07-01T00:00Z"));
    QCOMPARE(changed.count(), 1);
    clock.setTime(utc("2004-07-01T00:00Z"));
    QCOMPARE(changed.count(), 1);
    clock.setTime(QDateTime());
    QCOMPARE(changed.count(), 1);
    QCOMPARE(clock.time(), utc("2004-07-01T00:00Z"));
    QCOMPARE(clock.time().offsetFromUtc(), 0);
  }

  void concurrentOwnInstances()
  {
    // TimeExtent and LayerTime are reentrant.
    const int32_t failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [](int32_t thread) {
          const ts::TimeExtent extent = ts::TimeExtent::parse(
              QStringLiteral("2000-01-01/2026-12-31/P1D"));
          for (int32_t i = 0; i < 200; ++i) {
            const QDateTime t =
                utc("2001-01-01").addDays(int64_t{thread} * 300 + i);
            if (extent.atOrBefore(t.addSecs(3600)).value_or(QDateTime()) != t) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkAtOrBefore()
  {
    const ts::TimeExtent extent =
        ts::TimeExtent::parse(QStringLiteral("2000-02-24/2026-11-20/P1D"));
    const QDateTime t = utc("2015-03-03T12:00Z");
    QBENCHMARK
    {
      QVERIFY(extent.atOrBefore(t).has_value());
    }
  }
};

QTEST_GUILESS_MAIN(TestTime)
#include "tst_time.moc"
