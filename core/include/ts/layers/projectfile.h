// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Project files: the layer tree and the camera, as JSON.
 */
#ifndef TS_LAYERS_PROJECTFILE_H
#define TS_LAYERS_PROJECTFILE_H

#include <optional>

#include <QByteArray>
#include <QJsonArray>
#include <QString>

#include "ts/camera/camera.h"

namespace ts
{

class LayerTreeModel;

/** The content of a project file. */
struct Project {
  /** Where the camera was. */
  CameraPose camera;
  /** The layer tree (LayerTreeModel::toJson()). */
  QJsonArray layers;
};

/** Format name in every project file. */
inline constexpr const char* projectFormatName = "terrasoar-project";

/**
 * Version of the project file format this build writes. Files of this
 * and every earlier version load; earlier ones are migrated on load.
 */
inline constexpr int projectFormatVersion = 1;

/**
 * Returns @p project as an indented JSON document. The output is
 * deterministic: writing what was read gives the same bytes.
 *
 * @reentrant
 */
QByteArray projectToJson(const Project& project);

/**
 * Reads a project document. Returns nothing and sets @p errorString when
 * the document isn't a project, is of a newer version, or is damaged.
 *
 * @reentrant
 */
std::optional<Project> projectFromJson(const QByteArray& json,
                                       QString* errorString);

/**
 * Writes @p project to @p path (atomically: a failed write leaves the old
 * file). Returns false and sets @p errorString on failure.
 *
 * @reentrant
 */
bool saveProject(const QString& path, const Project& project,
                 QString* errorString);

/**
 * Reads the project at @p path. Returns nothing and sets @p errorString
 * on failure.
 *
 * @reentrant
 */
std::optional<Project> loadProject(const QString& path, QString* errorString);

} // namespace ts

#endif // TS_LAYERS_PROJECTFILE_H
