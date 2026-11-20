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
 * When KML features are active: their times on the program's clock,
 * tracks' positions, Regions and their levels of detail, and the
 * addresses NetworkLinks ask for.
 */
#ifndef TS_KML_KMLACTIVITY_H
#define TS_KML_KMLACTIVITY_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QDateTime>
#include <QString>
#include <QUrl>

#include "ts/camera/camera.h"
#include "ts/kml/kmldocument.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

/**
 * Returns true if something with @p time shows at @p clock: a TimeStamp
 * (begin and end the same) from its moment on; a TimeSpan from its begin
 * up to (not including) its end; open ends always.
 */
bool kmlShownAt(const KmlTime& time, const QDateTime& clock);

/**
 * Returns the time of the feature at @p featurePath (child indices from the
 * root) with those of the containers around it: the latest begin and the
 * earliest end; nothing if none of them has a time.
 */
std::optional<KmlTime> kmlEffectiveTime(const KmlDocument& document,
                                        const QString& featurePath);

/**
 * Returns @p outer narrowed by @p inner (either may be none: no time).
 */
std::optional<KmlTime> kmlNarrowTime(const std::optional<KmlTime>& outer,
                                     const std::optional<KmlTime>& inner);

/**
 * Returns the moments at which what the container at @p containerPath
 * shows changes (its placemarks' and overlays' begins and ends, and its
 * containers'; with @p deep, everything inside it), sorted, each once.
 */
std::vector<QDateTime> kmlTimeBoundaries(const KmlDocument& document,
                                         const QString& containerPath,
                                         bool deep);

/**
 * Returns the moment that stands for @p clock among @p boundaries: the
 * last at or before it, or a second before the first. What shows is the
 * same at both, so a layer's source changes only as the clock crosses a
 * boundary. Null if there are no boundaries.
 */
QDateTime kmlTimeKey(const std::vector<QDateTime>& boundaries,
                     const QDateTime& clock);

/**
 * Returns the times the container at @p containerPath has for the time
 * bar (TimeExtent syntax): its boundaries, and each track's span with a
 * step of a hundredth of it; empty for none.
 */
QString kmlTimeExtent(const KmlDocument& document, const QString& containerPath,
                      bool deep);

/**
 * Returns where @p track (a gx:Track, or the first track of a
 * MultiGeometry with one at @p clock) is at @p clock, interpolated between
 * its points; nothing before its first time or after its last.
 */
std::optional<KmlCoordinate> kmlTrackPosition(const KmlGeometry& track,
                                              const QDateTime& clock);

/**
 * Returns how large @p region's box is on the view of @p camera, in pixels
 * (the square root of the area of the screen rectangle it covers); a
 * negative number if none of it is in sight.
 */
double kmlRegionPixels(const KmlRegion& region, const Camera& camera);

/**
 * Returns true if @p region is active on the view of @p camera: some of it
 * in sight, at least minLodPixels and at most maxLodPixels (−1: no limit)
 * across.
 */
bool kmlRegionActive(const KmlRegion& region, const Camera& camera);

/**
 * Returns true if every Region on the way to the feature at @p featurePath
 * (its own and its containers') is active for @p camera.
 */
bool kmlRegionsActive(const KmlDocument& document, const QString& featurePath,
                      const Camera& camera);

/**
 * Returns true if the feature at @p featurePath or a container around it
 * has a Region.
 */
bool kmlHasRegion(const KmlDocument& document, const QString& featurePath);

/** What a NetworkLink's viewFormat can ask about the view. */
struct KmlViewParameters {
  /** The part of the Earth in sight (scaled by viewBoundScale). */
  GeoRect bounds{-180.0, -90.0, 180.0, 90.0};
  /** The camera's pose. */
  CameraPose pose;
  /** Where the eye is. */
  Geodetic eye;
  /** The view's width, pixels. */
  int32_t widthPx = 0;
  /** The view's height, pixels. */
  int32_t heightPx = 0;
  /** Horizontal field of view, degrees. */
  double horizontalFovDeg = 60.0;
  /** Vertical field of view, degrees. */
  double verticalFovDeg = 60.0;
  /** Terrain shown. */
  bool terrain = false;
};

/**
 * Returns the view parameters of @p camera: the bounds from where the view's
 * corners and edges meet the ground, or a box around the target (a globe
 * seen whole: all of it).
 */
KmlViewParameters kmlViewParameters(const Camera& camera, bool terrain);

/**
 * Returns the address @p link asks for from @p document: its href resolved
 * (a local path as a file URL), with its viewFormat (the default
 * `BBOX=[bboxWest],[bboxSouth],[bboxEast],[bboxNorth]` when it refreshes
 * with the view) and httpQuery filled in from @p view and appended to the
 * query of web addresses.
 */
QUrl kmlLinkUrl(const KmlLink& link, const KmlDocument& document,
                const KmlViewParameters& view);

} // namespace ts

#endif // TS_KML_KMLACTIVITY_H
