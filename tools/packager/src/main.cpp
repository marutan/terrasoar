// SPDX-License-Identifier: GPL-2.0-or-later
//
// terrasoar-packager: builds a data pack (the base pack) from downloaded
// sources. A build-time tool, not part of the application; see
// docs/tools/packager.md.

#include <optional>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QString>
#include <QTextStream>

#include "packbuilder.h"

namespace
{

void print(const QString& line)
{
  QTextStream out(stdout);
  out << line << '\n';
  out.flush();
}

} // namespace

int main(int argc, char* argv[])
{
  // QCoreApplication finds Qt's image plugins (JPEG).
  const QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("terrasoar-packager"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Builds a Terra Soar data pack from downloaded sources."));
  parser.addHelpOption();
  const QCommandLineOption configOption(
      QStringLiteral("config"), QStringLiteral("Pack configuration (JSON)."),
      QStringLiteral("file"));
  const QCommandLineOption sourcesOption(
      QStringLiteral("sources"),
      QStringLiteral("Directory with the downloaded sources."),
      QStringLiteral("directory"));
  const QCommandLineOption outputOption(
      QStringLiteral("output"),
      QStringLiteral("Directory to create the pack in (new or empty)."),
      QStringLiteral("directory"));
  const QCommandLineOption reportOption(
      QStringLiteral("report"),
      QStringLiteral("Write a JSON report of the build to this file."),
      QStringLiteral("file"));
  const QCommandLineOption noVerifyOption(
      QStringLiteral("no-verify-sources"),
      QStringLiteral("Skip the sources' SHA-256 check (development only)."));
  parser.addOption(configOption);
  parser.addOption(sourcesOption);
  parser.addOption(outputOption);
  parser.addOption(reportOption);
  parser.addOption(noVerifyOption);
  parser.process(application);
  if (!parser.isSet(configOption) || !parser.isSet(sourcesOption) ||
      !parser.isSet(outputOption)) {
    print(QStringLiteral("--config, --sources and --output are required."));
    return 2;
  }

  QString error;
  const std::optional<ts::packager::PackConfig> config =
      ts::packager::PackConfig::read(parser.value(configOption), &error);
  if (!config) {
    print(QStringLiteral("error: ") + error);
    return 1;
  }
  ts::packager::BuildOptions options;
  options.sourcesDirectory = parser.value(sourcesOption);
  options.configDirectory =
      QFileInfo(parser.value(configOption)).absolutePath();
  options.outputDirectory = parser.value(outputOption);
  options.verifySources = !parser.isSet(noVerifyOption);
  QElapsedTimer timer;
  timer.start();
  options.progress = [&timer](const QString& message) {
    print(QStringLiteral("[%1 s] %2")
              .arg(static_cast<double>(timer.elapsed()) / 1000.0, 6, 'f', 1)
              .arg(message));
  };
  ts::packager::BuildReport report;
  const bool ok = ts::packager::buildPack(*config, options, &report, &error);
  const QByteArray json =
      QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented);
  if (parser.isSet(reportOption)) {
    QFile file(parser.value(reportOption));
    if (file.open(QIODevice::WriteOnly)) {
      file.write(json);
    }
  }
  print(QString::fromUtf8(json));
  if (!ok) {
    print(QStringLiteral("error: ") + error);
    return 1;
  }
  print(QStringLiteral("Pack written to %1 (%2 bytes).")
            .arg(options.outputDirectory)
            .arg(report.totalBytes));
  return 0;
}
