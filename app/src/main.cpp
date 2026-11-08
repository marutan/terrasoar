// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Program entry point: translators, command line and the main window.
 */
#include <cstdio>
#include <cstring>
#include <memory>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include "mainwindow.h"
#include "startupmeasurement.h"
#include "ts/base/buildinfo.h"
#include "ts/gl/glprobe.h"
#include "ts/i18n/pseudotranslator.h"
#include "ts/i18n/translationloader.h"

namespace
{

/** Exit codes, documented in docs/testing.md. */
enum ExitCode {
  ExitOk = 0,
  ExitFailure = 1,
  ExitUsage = 2,
};

/**
 * Looks for @p name in argv before QApplication exists. Some settings
 * (software OpenGL, the locale) must be known before the application object
 * or the translated --help text is created.
 */
const char* findArgument(int argc, char** argv, const char* name,
                         bool wantsValue)
{
  const std::size_t length = std::strlen(name);
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == nullptr || std::strncmp(argv[i], name, length) != 0) {
      continue;
    }
    const char rest = argv[i][length];
    if (rest == '\0') {
      if (!wantsValue) {
        return argv[i];
      }
      return i + 1 < argc ? argv[i + 1] : nullptr;
    }
    if (rest == '=' && wantsValue) {
      return argv[i] + length + 1;
    }
  }
  return nullptr;
}

bool writeJson(const QJsonObject& object, const QString& target)
{
  const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Indented);
  if (target == QLatin1String("-")) {
    const std::size_t size = static_cast<std::size_t>(json.size());
    return std::fwrite(json.constData(), 1, size, stdout) == size &&
           std::fflush(stdout) == 0;
  }
  QFile file(target);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  return file.write(json) == json.size();
}

int runGlCheck()
{
  const ts::GlProbeResult probe = ts::probeOpenGl(3, 3);
  QJsonObject object;
  object.insert(QStringLiteral("ok"), probe.ok);
  object.insert(QStringLiteral("major"), probe.majorVersion);
  object.insert(QStringLiteral("minor"), probe.minorVersion);
  object.insert(QStringLiteral("core_profile"), probe.coreProfile);
  object.insert(QStringLiteral("vendor"), probe.vendor);
  object.insert(QStringLiteral("renderer"), probe.renderer);
  object.insert(QStringLiteral("version"), probe.version);
  object.insert(QStringLiteral("software"), probe.softwareRenderer);
  object.insert(QStringLiteral("platform"), QGuiApplication::platformName());
  if (!probe.ok) {
    object.insert(QStringLiteral("error"), probe.errorString);
  }
  writeJson(object, QStringLiteral("-"));
  return probe.ok ? ExitOk : ExitFailure;
}

} // namespace

int main(int argc, char* argv[])
{
  QElapsedTimer sinceMain;
  sinceMain.start();

  if (findArgument(argc, argv, "--software-gl", false) != nullptr) {
    QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
  }
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Terra Soar"));
  QCoreApplication::setApplicationName(QStringLiteral("terrasoar"));
  QCoreApplication::setApplicationVersion(ts::buildInfo().version);

  // Translators go in before any user-visible text is created.
  const char* localeArgument = findArgument(argc, argv, "--locale", true);
  const QLocale locale = localeArgument
                             ? QLocale(QString::fromLocal8Bit(localeArgument))
                             : QLocale::system();
  QTranslator translator;
  if (!ts::loadTranslation(translator, locale).isEmpty()) {
    QCoreApplication::installTranslator(&translator);
  }
  std::unique_ptr<ts::PseudoTranslator> pseudoTranslator;
  if (findArgument(argc, argv, "--pseudo-locale", false) != nullptr) {
    pseudoTranslator = std::make_unique<ts::PseudoTranslator>();
    QCoreApplication::installTranslator(pseudoTranslator.get());
  }
  QGuiApplication::setApplicationDisplayName(ts::programName());

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QCoreApplication::translate("main", "A 3D globe and 2D map viewer."));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption localeOption(
      QStringLiteral("locale"),
      QCoreApplication::translate(
          "main", "Use the translation for <locale> instead of the system "
                  "language."),
      QCoreApplication::translate("main", "locale"));
  const QCommandLineOption pseudoOption(
      QStringLiteral("pseudo-locale"),
      QCoreApplication::translate(
          "main", "Show every text in a generated test language."));
  const QCommandLineOption softwareGlOption(
      QStringLiteral("software-gl"),
      QCoreApplication::translate("main", "Use software OpenGL rendering."));
  const QCommandLineOption glCheckOption(
      QStringLiteral("gl-check"),
      QCoreApplication::translate(
          "main", "Check that an OpenGL 3.3 context can be created, print "
                  "the result as JSON and exit."));
  const QCommandLineOption smokeOption(
      QStringLiteral("smoke-test"),
      QCoreApplication::translate(
          "main", "Open the main window, close it again and exit."));
  const QCommandLineOption measureOption(
      QStringLiteral("measure"),
      QCoreApplication::translate(
          "main", "Measure start-up time and memory, write them as JSON to "
                  "<file> (- for standard output) and exit."),
      QCoreApplication::translate("main", "file"));
  const QCommandLineOption idleOption(
      QStringLiteral("idle-seconds"),
      QCoreApplication::translate(
          "main", "With --measure: stay idle for <seconds> after start-up "
                  "and report the CPU used meanwhile."),
      QCoreApplication::translate("main", "seconds"), QStringLiteral("0"));
  parser.addOption(localeOption);
  parser.addOption(pseudoOption);
  parser.addOption(softwareGlOption);
  parser.addOption(glCheckOption);
  parser.addOption(smokeOption);
  parser.addOption(measureOption);
  parser.addOption(idleOption);
  parser.process(app);

  if (parser.isSet(glCheckOption)) {
    return runGlCheck();
  }

  bool idleOk = false;
  const int idleS = parser.value(idleOption).toInt(&idleOk);
  if (!idleOk || idleS < 0) {
    // Nothing useful to do if stderr itself fails.
    static_cast<void>(std::fputs(
        qPrintable(QCoreApplication::translate(
            "main", "--idle-seconds needs a whole number of seconds.\n")),
        stderr));
    return ExitUsage;
  }

  ts::MainWindow window;

  std::unique_ptr<ts::StartupMeasurement> measurement;
  if (parser.isSet(measureOption)) {
    const QString target = parser.value(measureOption);
    measurement =
        std::make_unique<ts::StartupMeasurement>(sinceMain, &window, idleS);
    QObject::connect(measurement.get(), &ts::StartupMeasurement::finished,
                     &window, [&window, &measurement, target]() {
                       if (!writeJson(measurement->result(), target)) {
                         QCoreApplication::exit(ExitFailure);
                         return;
                       }
                       window.close();
                     });
    measurement->start();
  }

  window.show();

  if (parser.isSet(smokeOption) && !measurement) {
    // Close through the normal path (closeEvent, lastWindowClosed) once the
    // event loop is running.
    QTimer::singleShot(0, &window, &QWidget::close);
  }

  return QApplication::exec();
}
