// SPDX-License-Identifier: GPL-2.0-or-later
#include <QGuiApplication>
#include <QtTest>

#include "ts/gl/glprobe.h"

namespace
{

bool envFlag(const char* name)
{
  return qEnvironmentVariableIntValue(name) != 0;
}

} // namespace

/**
 * Tests for ts/gl: OpenGL 3.3 core context creation.
 *
 * Set TS_REQUIRE_GL=1 to make a missing context a failure instead of a
 * skip (CI does), and TS_EXPECT_SOFTWARE_GL=1 to require a software
 * rasteriser such as Mesa llvmpipe.
 */
class TestGl : public QObject
{
  Q_OBJECT

private slots:
  void softwareRendererNames_data()
  {
    QTest::addColumn<QString>("renderer");
    QTest::addColumn<bool>("software");
    QTest::newRow("llvmpipe")
        << QStringLiteral("llvmpipe (LLVM 17.0.6, 256 bits)") << true;
    QTest::newRow("softpipe") << QStringLiteral("softpipe") << true;
    QTest::newRow("warp") << QStringLiteral("Microsoft Basic Render Driver")
                          << true;
    QTest::newRow("gdi") << QStringLiteral("GDI Generic") << true;
    QTest::newRow("swiftshader")
        << QStringLiteral("Google SwiftShader") << true;
    QTest::newRow("intel") << QStringLiteral("Intel(R) HD Graphics 4400")
                           << false;
    QTest::newRow("mesa-intel")
        << QStringLiteral("Mesa Intel(R) HD Graphics 4400 (HSW GT2)") << false;
    QTest::newRow("empty") << QString() << false;
  }

  void softwareRendererNames()
  {
    QFETCH(QString, renderer);
    QFETCH(bool, software);
    QCOMPARE(ts::isSoftwareRenderer(renderer), software);
  }

  void createsOpenGl33CoreContext()
  {
    const ts::GlProbeResult probe = ts::probeOpenGl(3, 3);
    qInfo("platform %s: %s / %s / %s",
          qPrintable(QGuiApplication::platformName()), qPrintable(probe.vendor),
          qPrintable(probe.renderer), qPrintable(probe.version));
    if (!probe.ok && !envFlag("TS_REQUIRE_GL")) {
      QSKIP(qPrintable(QStringLiteral("No OpenGL 3.3 here (%1); set "
                                      "TS_REQUIRE_GL=1 to fail instead.")
                           .arg(probe.errorString)));
    }
    QVERIFY2(probe.ok, qPrintable(probe.errorString));
    QVERIFY(probe.majorVersion > 3 ||
            (probe.majorVersion == 3 && probe.minorVersion >= 3));
    QVERIFY(probe.coreProfile);
    QVERIFY(!probe.renderer.isEmpty());
    if (envFlag("TS_EXPECT_SOFTWARE_GL")) {
      QVERIFY2(probe.softwareRenderer, qPrintable(probe.renderer));
    }
  }

  void impossibleVersionFails()
  {
    const ts::GlProbeResult probe = ts::probeOpenGl(9, 9);
    QVERIFY(!probe.ok);
    QVERIFY(!probe.errorString.isEmpty());
  }
};

QTEST_MAIN(TestGl)
#include "tst_gl.moc"
