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

//
// terrasoar-packager: builds a data pack (the base pack) from downloaded
// sources. A build-time tool, not part of the application; see
// docs/tools/packager.md.

#include <cstdint>
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
#include "vpf/vpfcoverage.h"

namespace
{

void print(const QString& line)
{
  QTextStream out(stdout);
  out << line << '\n';
  out.flush();
}

namespace vpf = ts::packager::vpf;

/** Prints each feature class of @p coverage with what was read. */
void printClasses(vpf::Coverage& coverage)
{
  QString error;
  for (const vpf::FeatureClass& featureClass : coverage.featureClasses()) {
    std::vector<vpf::Feature> features;
    vpf::ReadStats stats;
    if (!coverage.readFeatures(featureClass, &features, &stats, &error)) {
      print(QStringLiteral("      %1: %2").arg(featureClass.name, error));
      continue;
    }
    int64_t vertices = 0;
    for (const vpf::Feature& feature : features) {
      for (const std::vector<vpf::Coordinate>& part : feature.parts) {
        vertices += static_cast<int64_t>(part.size());
      }
    }
    print(
        QStringLiteral("      %1 (%2 via %3): %4 features, %5 vertices, "
                       "%6 skipped")
            .arg(featureClass.name, featureClass.table, featureClass.primitive)
            .arg(stats.features)
            .arg(vertices)
            .arg(stats.skipped));
    for (const QString& problem : stats.problems) {
      print(QStringLiteral("        ") + problem);
    }
  }
}

/** Prints library @p info's coverages (only @p only, if not empty). */
void printLibrary(const vpf::Database& database, const vpf::LibraryInfo& info,
                  const QString& only)
{
  QString error;
  const std::optional<vpf::Library> library =
      vpf::Library::open(database, info.name, &error);
  if (!library) {
    print(QStringLiteral("  library %1: %2").arg(info.name, error));
    return;
  }
  print(QStringLiteral("  library %1 (%2 tiles), %3 %4 %5 %6")
            .arg(info.name)
            .arg(library->tilePaths.size())
            .arg(info.bounds.xmin)
            .arg(info.bounds.ymin)
            .arg(info.bounds.xmax)
            .arg(info.bounds.ymax));
  for (const vpf::CoverageInfo& coverageInfo : library->coverages) {
    if (!only.isEmpty() && coverageInfo.name != only) {
      continue;
    }
    std::optional<vpf::Coverage> coverage =
        vpf::Coverage::open(*library, coverageInfo.name, &error);
    if (!coverage) {
      print(
          QStringLiteral("    coverage %1: %2").arg(coverageInfo.name, error));
      continue;
    }
    print(QStringLiteral("    coverage %1 (%2, level %3)")
              .arg(coverageInfo.name, coverageInfo.description)
              .arg(coverageInfo.level));
    printClasses(*coverage);
  }
}

/**
 * Prints the libraries, coverages and feature classes of the VPF database
 * at @p path (only coverage @p only, if not empty), with each class's
 * features read and skipped.
 */
// The database, then the coverage in it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int32_t printVpfInfo(const QString& path, const QString& only)
{
  QString error;
  const std::optional<vpf::Database> database =
      vpf::Database::open(path, &error);
  if (!database) {
    print(QStringLiteral("error: ") + error);
    return 1;
  }
  print(QStringLiteral("Database %1: %2")
            .arg(database->name, database->description));
  for (const vpf::LibraryInfo& info : database->libraries) {
    printLibrary(*database, info, only);
  }
  return 0;
}

} // namespace

int32_t main(int32_t argc, char* argv[])
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
  const QCommandLineOption vpfInfoOption(
      QStringLiteral("vpf-info"),
      QStringLiteral("List the libraries, coverages and feature classes of "
                     "a VPF database (a directory, or /vsizip/<zip>/<dir>)."),
      QStringLiteral("database"));
  const QCommandLineOption coverageOption(
      QStringLiteral("coverage"),
      QStringLiteral("With --vpf-info, only this coverage."),
      QStringLiteral("name"));
  parser.addOption(vpfInfoOption);
  parser.addOption(coverageOption);
  parser.process(application);
  if (parser.isSet(vpfInfoOption)) {
    return printVpfInfo(parser.value(vpfInfoOption),
                        parser.value(coverageOption));
  }
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
