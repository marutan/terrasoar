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
 * The Ordnance Survey National Grid of Great Britain (EPSG:27700): the
 * OSGB36 datum, the projection and grid references.
 */
#ifndef TS_PROJECTION_NATIONALGRID_H
#define TS_PROJECTION_NATIONALGRID_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <QString>

#include "ts/projection/transversemercator.h"

namespace ts
{

class HorizontalShiftGrid;

/**
 * Converts between WGS 84 and the OSGB36 datum of the National Grid, as
 * PROJ does from EPSG:4326 to EPSG:4277:
 *
 * - with the OSTN15 grid (the Ordnance Survey's definitive transformation,
 *   in its NTv2 form, about 0.1 m): WGS 84 is taken as ETRS89 and the
 *   grid's offsets are removed;
 * - without it, or outside it: the Helmert transform "OSGB36 to WGS 84
 *   (6)" (EPSG:1314, about 2 m in Great Britain, metres more away from
 *   it), at zero ellipsoidal height.
 *
 * Over the grid's outermost 0.1°, all at sea, the two are blended
 * smoothly, so positions don't jump at its edge.
 *
 * @threadsafe (immutable)
 */
class Osgb36Datum
{
public:
  /** Converts with @p ostn15, or with the Helmert transform if null. */
  explicit Osgb36Datum(std::shared_ptr<const HorizontalShiftGrid> ostn15);

  /** Returns true if it has the OSTN15 grid. */
  bool hasGrid() const { return m_grid != nullptr; }

  /** Returns the OSTN15 grid's file, or empty. */
  QString gridPath() const;

  /**
   * Returns the OSGB36 latitude and longitude (radians) of WGS 84 latitude
   * @p latRad and longitude @p lonRad.
   */
  std::array<double, 2> fromWgs84(double latRad, double lonRad) const;

  /** The inverse of fromWgs84(), to a micrometre. */
  std::array<double, 2> toWgs84(double latRad, double lonRad) const;

  /** fromWgs84() by the Helmert transform alone. */
  static std::array<double, 2> helmertFromWgs84(double latRad, double lonRad);

  /** Degrees over which the grid's edge is blended into the Helmert. */
  static constexpr double blendDeg() { return 0.1; }

private:
  std::shared_ptr<const HorizontalShiftGrid> m_grid;
};

/**
 * The Ordnance Survey National Grid (EPSG:27700): Transverse Mercator on
 * the Airy 1830 ellipsoid, origin 49° N 2° W, scale factor 0.9996012717,
 * false origin 400 km west and 100 km north of it, in the OSGB36 datum
 * (Osgb36Datum). Map positions are the grid's eastings and northings.
 * Shown within maximumLonOffsetDeg() of 2° W and 85° of the equator, as
 * Mercator: nearer the poles the shader's approximate datum shift, which
 * divides by cos φ, drifts.
 *
 * @threadsafe
 */
class NationalGridProjection : public TransverseMercatorProjection
{
public:
  /** Creates the projection, with the OSTN15 grid @p ostn15 if not null. */
  explicit NationalGridProjection(
      std::shared_ptr<const HorizontalShiftGrid> ostn15 = nullptr);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  QString datumShiftGridId() const override;
  QString gridReference(double latDeg, double lonDeg) const override;
  double minimumLatDeg() const override { return -85.0; }
  double maximumLatDeg() const override { return 85.0; }
  MapPoint project(double latRad, double dLonRad) const override;

  /** Returns the datum conversion. */
  const Osgb36Datum& datum() const { return m_datum; }

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardPrologue() const override;
  std::array<std::array<float, 4>, 2>
  glslParametersAt(double latRad, double dLonRad) const override;

private:
  /** Grid eastings and northings of OSGB36 latitude and longitude. */
  MapPoint gridOf(double osgbLatRad, double osgbLonRad) const;

  Osgb36Datum m_datum;
  /** The northing of the true origin (49° N on 2° W) before the false
   *  northing: subtracted from the series' y. */
  double m_originNorthingM;
};

/**
 * Returns the grid reference of easting @p eastingM and northing
 * @p northingM with @p digits digits (2 to 10, even: 10 gives metres),
 * e.g. "TQ 30080 80729", truncated as the Ordnance Survey does; empty
 * outside the lettered squares (0 to 700 km east, 0 to 1300 km north).
 */
QString formatGridReference(double eastingM, double northingM,
                            int32_t digits = 10);

/**
 * Returns the easting and northing of the south-west corner of the square
 * grid reference @p text names ("TQ 30080 80729", "TQ3008080729",
 * "SU 387 147", "NN"; letters in either case), or nothing if it isn't
 * one.
 */
std::optional<MapPoint> parseGridReference(const QString& text);

} // namespace ts

#endif // TS_PROJECTION_NATIONALGRID_H
