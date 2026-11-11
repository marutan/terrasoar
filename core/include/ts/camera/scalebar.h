// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The scale bar: a round ground distance and its length on screen.
 */
#ifndef TS_CAMERA_SCALEBAR_H
#define TS_CAMERA_SCALEBAR_H

#include <optional>

namespace ts
{

class Camera;

/** A scale bar for a view. */
struct ScaleBar {
  /** The distance the bar stands for: 1, 2 or 5 times a power of ten, m. */
  double lengthM = 0.0;
  /** The bar's length on screen, logical pixels. */
  double widthPx = 0.0;
  /** The latitude the bar was measured at (the centre of the view). */
  double latDeg = 0.0;
  /**
   * True if the scale at the top or bottom of the view differs from the
   * bar's by more than a tenth: on a Mercator map of a large area, or a
   * globe seen whole. The bar is then only right near its latitude.
   */
  bool variesAcrossView = false;
};

/**
 * Returns the scale bar for @p camera, at most @p maximumWidthPx long, or
 * nothing if the centre of the view is not on the globe or map.
 *
 * The bar is measured where it applies: the geodesic distance between two
 * points of the surface a few pixels either side of the centre of the
 * view. So it is true ground distance at the centre's latitude in every
 * view, whatever the projection stretches elsewhere.
 *
 * @threadsafe
 */
std::optional<ScaleBar> scaleBarFor(const Camera& camera,
                                    double maximumWidthPx);

} // namespace ts

#endif // TS_CAMERA_SCALEBAR_H
