// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "fixturepack.h"

namespace
{

/** Output and exit state of one run of the application. */
struct Run {
  bool finished = false;
  QProcess::ExitStatus status = QProcess::CrashExit;
  int exitCode = -1;
  QByteArray out;
  QByteArray err;
};

/**
 * Runs the built terrasoar with @p arguments and waits up to @p timeoutMs.
 * With @p headless the offscreen platform is used, so no display is needed.
 */
Run runApp(const QStringList& arguments, bool headless = true,
           int timeoutMs = 60000,
           const QProcessEnvironment& extra = QProcessEnvironment())
{
  QProcess process;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(extra);
  if (headless) {
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    // Truly headless: with a display set, the offscreen platform still
    // opens it for OpenGL (GLX), which loads the GL driver.
    env.remove(QStringLiteral("DISPLAY"));
    env.remove(QStringLiteral("WAYLAND_DISPLAY"));
  }
  process.setProcessEnvironment(env);
  process.start(QStringLiteral(TS_APP_PATH), arguments);
  Run run;
  if (!process.waitForStarted(timeoutMs)) {
    run.err = process.errorString().toUtf8();
    return run;
  }
  run.finished = process.waitForFinished(timeoutMs);
  if (!run.finished) {
    process.kill();
    process.waitForFinished();
  }
  run.status = process.exitStatus();
  run.exitCode = process.exitCode();
  run.out = process.readAllStandardOutput();
  run.err = process.readAllStandardError();
  return run;
}

/** Returns stderr lines that look like Qt runtime warnings. */
QStringList qtWarnings(const QByteArray& err)
{
  static const std::array<const char*, 6> markers = {
      "QObject::", "QWidget::", "QPainter::", "QOpenGL", "ASSERT", "Cannot "};
  QStringList found;
  const QList<QByteArray> lines = err.split('\n');
  for (const QByteArray& line : lines) {
    for (const char* marker : markers) {
      if (line.contains(marker)) {
        found << QString::fromUtf8(line);
        break;
      }
    }
  }
  return found;
}

QJsonObject parseJson(const QByteArray& out)
{
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(out, &error);
  return error.error == QJsonParseError::NoError ? document.object()
                                                 : QJsonObject();
}

void verifyCleanExit(const Run& run)
{
  QVERIFY2(run.finished, run.err.constData());
  QCOMPARE(run.status, QProcess::NormalExit);
  QVERIFY2(run.exitCode == 0, run.err.constData());
  const QStringList warnings = qtWarnings(run.err);
  QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
}

} // namespace

/**
 * Build smoke tests: the real executable starts headless, shows and closes
 * its window, and reports measurements.
 */
class TestSmoke : public QObject
{
  Q_OBJECT

private slots:
  void startsAndExitsCleanly()
  {
    verifyCleanExit(runApp({QStringLiteral("--smoke-test")}));
  }

  void startsWithPseudoLocale()
  {
    verifyCleanExit(runApp(
        {QStringLiteral("--smoke-test"), QStringLiteral("--pseudo-locale")}));
  }

  void startsWithOtherLocale()
  {
    verifyCleanExit(
        runApp({QStringLiteral("--smoke-test"), QStringLiteral("--locale"),
                QStringLiteral("de_DE")}));
  }

  void version()
  {
    const Run run = runApp({QStringLiteral("--version")});
    QCOMPARE(run.exitCode, 0);
    QVERIFY2(run.out.contains(TS_VERSION_STRING), run.out.constData());
  }

  void badArgumentIsUsageError()
  {
    const Run run =
        runApp({QStringLiteral("--measure"), QStringLiteral("-"),
                QStringLiteral("--idle-seconds"), QStringLiteral("soon")});
    QVERIFY(run.finished);
    QCOMPARE(run.exitCode, 2);
  }

  void measureReportsStartup()
  {
    const Run run = runApp({QStringLiteral("--measure"), QStringLiteral("-")});
    verifyCleanExit(run);
    const QJsonObject result = parseJson(run.out);
    QVERIFY2(!result.isEmpty(), run.out.constData());
    const double startupMs =
        result.value(QStringLiteral("startup_ms")).toDouble();
    const double residentBytes =
        result.value(QStringLiteral("resident_bytes")).toDouble();
    qInfo("start-up %.1f ms, resident %.1f MB", startupMs,
          residentBytes / (1024.0 * 1024.0));
    QVERIFY(startupMs > 0.0);
    QVERIFY(residentBytes > 0.0);
    QVERIFY(result.value(QStringLiteral("peak_resident_bytes")).toDouble() >=
            residentBytes);
    // The empty shell must leave nearly all of the 256 MB budget free.
    QVERIFY(residentBytes < 128.0 * 1024.0 * 1024.0);
  }

  void idleUsesNoCpu()
  {
    const Run run =
        runApp({QStringLiteral("--measure"), QStringLiteral("-"),
                QStringLiteral("--idle-seconds"), QStringLiteral("2")});
    verifyCleanExit(run);
    const QJsonObject result = parseJson(run.out);
    const double percent =
        result.value(QStringLiteral("idle_cpu_percent")).toDouble(-1.0);
    qInfo("idle CPU %.2f%%", percent);
    QVERIFY(percent >= 0.0);
    QVERIFY2(percent < 5.0, run.out.constData());
  }

  /**
   * The application with its globe view on the real platform: opens,
   * draws and closes without warnings. Skipped without OpenGL 3.3 unless
   * TS_REQUIRE_GL=1.
   */
  void startsWithGlobe()
  {
    const Run glCheck = runApp({QStringLiteral("--gl-check")}, false);
    if (glCheck.exitCode != 0 &&
        qEnvironmentVariableIntValue("TS_REQUIRE_GL") == 0) {
      QSKIP("No OpenGL 3.3 here; set TS_REQUIRE_GL=1 to fail instead.");
    }
    const Run run = runApp({QStringLiteral("--smoke-test")}, false);
    QVERIFY2(run.finished, run.err.constData());
    QCOMPARE(run.status, QProcess::NormalExit);
    QCOMPARE(run.exitCode, 0);
    const QStringList warnings = qtWarnings(run.err);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    QVERIFY2(!run.err.contains("Globe view:"), run.err.constData());
  }

  /**
   * Offline start: the application, on the real platform with the fixture
   * base pack and every proxy pointing at a closed port (so any network
   * access would fail), starts with the base layers on the globe: the
   * graticule and shoreline over Blue Marble, their sources credited, and
   * tiles drawn. Terra Soar has no network code yet (WMS comes in Phase 4);
   * this keeps it working offline once it has.
   */
  void offlineBaseLayers()
  {
    const Run glCheck = runApp({QStringLiteral("--gl-check")}, false);
    if (glCheck.exitCode != 0 &&
        qEnvironmentVariableIntValue("TS_REQUIRE_GL") == 0) {
      QSKIP("No OpenGL 3.3 here; set TS_REQUIRE_GL=1 to fail instead.");
    }
    const QTemporaryDir pack;
    ts::test::writeFixtureBasePack(pack.path());
    QProcessEnvironment offline;
    offline.insert(QStringLiteral("TS_BASEPACK_DIR"), pack.path());
    for (const char* name : {"http_proxy", "https_proxy", "HTTP_PROXY",
                             "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
      offline.insert(QLatin1String(name),
                     QStringLiteral("http://127.0.0.1:9")); // discard port
    }
    offline.insert(QStringLiteral("no_proxy"), QString());
    const Run run =
        runApp({QStringLiteral("--measure"), QStringLiteral("-"),
                QStringLiteral("--idle-seconds"), QStringLiteral("2")},
               false, 60000, offline);
    verifyCleanExit(run);
    const QJsonObject state =
        parseJson(run.out).value(QStringLiteral("state")).toObject();
    qInfo("%s",
          QJsonDocument(state).toJson(QJsonDocument::Compact).constData());
    QCOMPARE(state.value(QStringLiteral("base_pack")).toString(),
             QStringLiteral("ok"));
    QCOMPARE(
        state.value(QStringLiteral("layers")).toArray(),
        (QJsonArray{QStringLiteral("Graticule"), QStringLiteral("Shoreline"),
                    QStringLiteral("Blue Marble")}));
    QCOMPARE(state.value(QStringLiteral("attributions")).toArray().size(), 2);
    QCOMPARE(state.value(QStringLiteral("imagery_layers")).toInt(), 1);
    QCOMPARE(state.value(QStringLiteral("vector_layers")).toInt(), 2);
    QVERIFY(state.value(QStringLiteral("imagery_tiles_drawn")).toInt() > 0);
    QVERIFY(state.value(QStringLiteral("vector_tiles_drawn")).toInt() > 0);
  }

  /**
   * Software-rendering smoke test: on the real platform (not offscreen),
   * the app creates an OpenGL 3.3 core context. CI runs this on Mesa
   * llvmpipe (Linux, under Xvfb) and on Qt's software OpenGL (Windows).
   */
  void createsOpenGl33Context()
  {
    QStringList arguments{QStringLiteral("--gl-check")};
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIntValue("TS_EXPECT_SOFTWARE_GL") != 0) {
      arguments << QStringLiteral("--software-gl");
    }
#endif
    const Run run = runApp(arguments, false);
    const QJsonObject result = parseJson(run.out);
    qInfo("%s", run.out.trimmed().constData());
    if (run.exitCode != 0 &&
        qEnvironmentVariableIntValue("TS_REQUIRE_GL") == 0) {
      QSKIP("No OpenGL 3.3 here; set TS_REQUIRE_GL=1 to fail instead.");
    }
    QVERIFY2(run.finished && run.exitCode == 0, run.err.constData());
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    QVERIFY(result.value(QStringLiteral("core_profile")).toBool());
    if (qEnvironmentVariableIntValue("TS_EXPECT_SOFTWARE_GL") != 0) {
      QVERIFY2(result.value(QStringLiteral("software")).toBool(),
               run.out.constData());
    }
  }
};

QTEST_GUILESS_MAIN(TestSmoke)
#include "tst_smoke.moc"
