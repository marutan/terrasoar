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

#include "ts/camera/scalebar.h"

#include <cmath>

#include "ts/camera/camera.h"
#include "ts/geo/geodesic.h"

namespace ts
{

namespace
{

/** Half the baseline the scale is measured over, pixels. */
constexpr double halfBaselinePx = 8.0;

/** Ground metres per pixel along the row @p yPx at the view's middle. */
std::optional<double> metresPerPixel(const Camera& camera, double yPx)
{
  const double xPx = 0.5 * camera.viewportWidthPx();
  const std::optional<Geodetic> left =
      camera.pickGeodetic(xPx - halfBaselinePx, yPx);
  const std::optional<Geodetic> right =
      camera.pickGeodetic(xPx + halfBaselinePx, yPx);
  if (!left || !right) {
    return std::nullopt;
  }
  const double distanceM =
      geodesicInverse(left->latDeg, left->lonDeg, right->latDeg, right->lonDeg)
          .distanceM;
  return distanceM / (2.0 * halfBaselinePx);
}

} // namespace

std::optional<ScaleBar> scaleBarFor(const Camera& camera, double maximumWidthPx)
{
  const double centreYPx = 0.5 * camera.viewportHeightPx();
  const std::optional<double> scale = metresPerPixel(camera, centreYPx);
  const std::optional<Geodetic> centre =
      camera.pickGeodetic(0.5 * camera.viewportWidthPx(), centreYPx);
  if (!scale || !centre || !(*scale > 0.0) || !(maximumWidthPx > 0.0)) {
    return std::nullopt;
  }
  // The largest of 1, 2, 5 × 10ⁿ that fits.
  const double longestM = *scale * maximumWidthPx;
  const double decade = std::pow(10.0, std::floor(std::log10(longestM)));
  double lengthM = decade;
  for (const double step : {2.0, 5.0, 10.0}) {
    if (step * decade <= longestM * (1.0 + 1e-12)) {
      lengthM = step * decade;
    }
  }
  ScaleBar bar;
  bar.lengthM = lengthM;
  bar.widthPx = lengthM / *scale;
  bar.latDeg = centre->latDeg;
  for (const double yPx : {0.5, camera.viewportHeightPx() - 0.5}) {
    const std::optional<double> edge = metresPerPixel(camera, yPx);
    if (!edge || std::abs(*edge / *scale - 1.0) > 0.1) {
      bar.variesAcrossView = true;
    }
  }
  return bar;
}

} // namespace ts
