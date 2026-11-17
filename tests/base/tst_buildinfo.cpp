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

#include <QtTest>

#include "concurrency.h"
#include "ts/base/buildinfo.h"

/** Unit tests for ts/base: build and version information. */
class TestBuildInfo : public QObject
{
  Q_OBJECT

private slots:
  void versionMatchesBuild()
  {
    const ts::BuildInfo info = ts::buildInfo();
    QCOMPARE(info.version, QStringLiteral(TS_VERSION_STRING));
    QVERIFY(!info.version.isEmpty());
  }

  void qtVersions()
  {
    const ts::BuildInfo info = ts::buildInfo();
    QCOMPARE(info.qtCompileVersion, QStringLiteral(QT_VERSION_STR));
    QCOMPARE(info.qtRuntimeVersion, QString::fromLatin1(qVersion()));
    QVERIFY(info.qtCompileVersion.startsWith(QLatin1String("5.15")) ||
            info.qtCompileVersion.startsWith(QLatin1String("6.")));
  }

  void compilerKnown()
  {
    QVERIFY(!ts::buildInfo().compiler.contains(QLatin1String("unknown")));
  }

  void programNameUntranslated()
  {
    QCOMPARE(ts::programName(), QStringLiteral("Terra Soar"));
  }

  void concurrentCalls()
  {
    const ts::BuildInfo expected = ts::buildInfo();
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&expected](int) {
          for (int i = 0; i < 1000; ++i) {
            const ts::BuildInfo info = ts::buildInfo();
            if (info.version != expected.version ||
                info.compiler != expected.compiler) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }
};

QTEST_GUILESS_MAIN(TestBuildInfo)
#include "tst_buildinfo.moc"
