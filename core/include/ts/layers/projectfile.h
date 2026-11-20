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

/**
 * @file
 * Project files: the layer tree and the camera, as JSON.
 */
#ifndef TS_LAYERS_PROJECTFILE_H
#define TS_LAYERS_PROJECTFILE_H

#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QString>

#include "ts/camera/camera.h"
#include "ts/projection/projection.h"

namespace ts
{

class LayerTreeModel;

/** How the program clock ran when a project was saved (`view.clock`). */
struct ClockState {
  /** It followed real time: opened, it runs in real time from then. */
  bool realTime = false;
  /** It was running (at rate from time). */
  bool running = false;
  /** Its rate: seconds of the clock per second. */
  double rate = 1.0;

  /** Equality. */
  bool operator==(ClockState other) const
  {
    return realTime == other.realTime && running == other.running &&
           rate == other.rate;
  }
};

/** The content of a project file. */
struct Project {
  /** Where the camera was. */
  CameraPose camera;
  /** True if the view was the flat map, false for the globe. */
  bool mapView = false;
  /** Identifier of the flat map's projection (Projection::id()). */
  QString projection = QStringLiteral("mercator");
  /**
   * The projection's parameters (Projection::parameterValues()); empty
   * for none. An optional member of `view`.
   */
  ProjectionParameterValues projectionParameters;
  /**
   * The program clock's time (UTC), which layers with a time dimension
   * follow; invalid for none (the current time when opened). An optional
   * member of `view`, written as ISO 8601.
   */
  QDateTime time;
  /**
   * How the clock ran (an optional member of `view`, `{"realTime": false,
   * "running": true, "rate": 60}`); nothing in projects saved before it
   * ran, which open paused at time.
   */
  std::optional<ClockState> clock;
  /** The layer tree (LayerTreeModel::toJson()). */
  QJsonArray layers;
  /**
   * The KML documents in the layer tree (key `kml`), each an object with
   * `node` (the id of its entry in layers), and `path` (its file) or
   * `text` (the KML of one never saved); `editable` if it is being
   * edited. Empty, and left out, if there are none.
   */
  QJsonArray kml;
};

/** Format name in every project file. */
inline constexpr const char* projectFormatName = "terrasoar-project";

/**
 * Version of the project file format this build writes. Files of this
 * and every earlier version load; earlier ones are migrated on load.
 */
inline constexpr int32_t projectFormatVersion = 2;

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
