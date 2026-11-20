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
 * What the application shows of KML features: camera poses for their
 * views and the HTML of their balloons.
 */
#ifndef TS_KML_KMLPRESENTATION_H
#define TS_KML_KMLPRESENTATION_H

#include <optional>

#include <QString>

#include "ts/camera/camera.h"
#include "ts/kml/kmldocument.h"

namespace ts
{

/**
 * Returns the camera pose for @p view: a LookAt's point, range, heading
 * and tilt as they are; a Camera's position turned into the point it
 * looks at on the ground (straight down when not tilted) at the matching
 * range. Tilts are kept below 85°.
 */
CameraPose kmlViewPose(const KmlView& view);

/**
 * Returns the pose to fly to for @p feature: its own view, else one
 * framing its geometry (or a ground overlay's box); nothing if it has
 * neither.
 */
std::optional<CameraPose> kmlFeaturePose(const KmlFeature& feature);

/**
 * Returns the HTML of @p placemark's balloon with @p style's
 * BalloonStyle: its text with $[name], $[description], $[Snippet],
 * $[id] and each extended data $[field] (or $[field/displayName])
 * filled in; without one, the name as a heading, the description, and
 * the extended data as a table. Text values are escaped; the
 * description stays HTML, as KML has it.
 */
QString kmlBalloonHtml(const KmlFeature& placemark, const KmlStyle& style);

} // namespace ts

#endif // TS_KML_KMLPRESENTATION_H
