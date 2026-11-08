// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/layers/projectfile.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace ts
{

namespace
{

std::optional<Project> failProject(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return std::nullopt;
}

QJsonObject cameraToJson(const CameraPose& pose)
{
  QJsonObject json;
  json.insert(QStringLiteral("targetLatDeg"), pose.targetLatDeg);
  json.insert(QStringLiteral("targetLonDeg"), pose.targetLonDeg);
  json.insert(QStringLiteral("rangeM"), pose.rangeM);
  json.insert(QStringLiteral("headingDeg"), pose.headingDeg);
  json.insert(QStringLiteral("tiltDeg"), pose.tiltDeg);
  return json;
}

CameraPose cameraFromJson(const QJsonObject& json)
{
  const CameraPose defaults;
  CameraPose pose;
  pose.targetLatDeg = json.value(QStringLiteral("targetLatDeg"))
                          .toDouble(defaults.targetLatDeg);
  pose.targetLonDeg = json.value(QStringLiteral("targetLonDeg"))
                          .toDouble(defaults.targetLonDeg);
  pose.rangeM = json.value(QStringLiteral("rangeM")).toDouble(defaults.rangeM);
  pose.headingDeg =
      json.value(QStringLiteral("headingDeg")).toDouble(defaults.headingDeg);
  pose.tiltDeg =
      json.value(QStringLiteral("tiltDeg")).toDouble(defaults.tiltDeg);
  return pose;
}

/**
 * Brings a document of an earlier version up to the current one. Each
 * format change adds a step here and a fixture of the old version to
 * tests/data/projects, which the tests keep loading.
 */
QJsonObject migrate(QJsonObject document, int /*fromVersion*/)
{
  // Version 1 is the first; nothing to migrate yet.
  return document;
}

} // namespace

QByteArray projectToJson(const Project& project)
{
  QJsonObject document;
  document.insert(QStringLiteral("format"), QLatin1String(projectFormatName));
  document.insert(QStringLiteral("version"), projectFormatVersion);
  document.insert(QStringLiteral("camera"), cameraToJson(project.camera));
  document.insert(QStringLiteral("layers"), project.layers);
  return QJsonDocument(document).toJson(QJsonDocument::Indented);
}

std::optional<Project> projectFromJson(const QByteArray& json,
                                       QString* errorString)
{
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return failProject(errorString,
                       QCoreApplication::translate(
                           "ts::ProjectFile", "The file is not valid JSON: %1")
                           .arg(parseError.errorString()));
  }
  QJsonObject object = document.object();
  if (object.value(QStringLiteral("format")).toString() !=
      QLatin1String(projectFormatName)) {
    return failProject(
        errorString, QCoreApplication::translate("ts::ProjectFile",
                                                 "The file is not a project."));
  }
  const int version = object.value(QStringLiteral("version")).toInt(0);
  if (version < 1) {
    return failProject(errorString,
                       QCoreApplication::translate(
                           "ts::ProjectFile", "The project has no version."));
  }
  if (version > projectFormatVersion) {
    return failProject(
        errorString,
        QCoreApplication::translate(
            "ts::ProjectFile",
            "The project was saved by a newer version of the program "
            "(format %1; this one reads up to %2).")
            .arg(version)
            .arg(projectFormatVersion));
  }
  object = migrate(object, version);
  Project project;
  project.camera =
      cameraFromJson(object.value(QStringLiteral("camera")).toObject());
  project.layers = object.value(QStringLiteral("layers")).toArray();
  return project;
}

bool saveProject(const QString& path, const Project& project,
                 QString* errorString)
{
  QSaveFile file(path);
  if (file.open(QIODevice::WriteOnly)) {
    const QByteArray json = projectToJson(project);
    if (file.write(json) == json.size() && file.commit()) {
      return true;
    }
  }
  if (errorString) {
    *errorString =
        QCoreApplication::translate("ts::ProjectFile", "Could not write %1: %2")
            .arg(path, file.errorString());
  }
  return false;
}

std::optional<Project> loadProject(const QString& path, QString* errorString)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return failProject(
        errorString,
        QCoreApplication::translate("ts::ProjectFile", "Could not read %1: %2")
            .arg(path, file.errorString()));
  }
  return projectFromJson(file.readAll(), errorString);
}

} // namespace ts
