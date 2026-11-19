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
 * The transverse projections: Transverse Mercator, UTM and Cassini.
 */
#ifndef TS_PROJECTION_TRANSVERSEMERCATOR_H
#define TS_PROJECTION_TRANSVERSEMERCATOR_H

#include <cstdint>
#include <memory>

#include "ts/projection/localexpansion.h"

namespace ts
{

struct KruegerSeries;

/**
 * Transverse Mercator (Gauss–Krüger) on the WGS 84 ellipsoid, by Krüger's
 * series to n⁶ (Karney 2011), as PROJ's `tmerc`: conformal, true to scale
 * (times the scale factor k0) along the central meridian. Shown within
 * maximumLonOffsetDeg() of the central meridian, where the series stays
 * within a millimetre of the exact projection.
 *
 * @threadsafe
 */
class TransverseMercatorProjection : public LocalExpansionProjection
{
public:
  /**
   * Creates the projection with central meridian @p centralMeridianDeg
   * and scale factor @p scaleFactor.
   */
  explicit TransverseMercatorProjection(double centralMeridianDeg = 0.0,
                                        double scaleFactor = 1.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double maximumLonOffsetDeg() const override { return 40.0; }
  MapPoint project(double latRad, double dLonRad) const override;

  /** Returns the scale factor on the central meridian, k0. */
  double scaleFactor() const { return m_scaleFactor; }

protected:
  /**
   * Creates the projection on the ellipsoid with semi-major axis
   * @p semiMajorAxisM and semi-minor axis @p semiMinorAxisM (metres)
   * instead of WGS 84's. project() then takes latitudes and longitudes on
   * that ellipsoid.
   */
  TransverseMercatorProjection(double centralMeridianDeg, double scaleFactor,
                               double semiMajorAxisM, double semiMinorAxisM);

  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;

  /**
   * Returns GLSL statements put at the start of the forward function,
   * which may change its `lonLatRad` (longitude east of the central
   * meridian, latitude; radians) before the projection: a datum shift.
   * p0.x is the scale; p0.yzw and p1 are free for them. None by default.
   */
  virtual QByteArray glslForwardPrologue() const;

private:
  double m_scaleFactor;
  std::shared_ptr<const KruegerSeries> m_series;
};

/**
 * Universal Transverse Mercator: Transverse Mercator with k0 = 0.9996 on
 * the central meridian of a 6° zone (1 to 60, from 180° W). The map
 * shows well beyond the zone; positions are those of the zone's grid
 * without its false easting and northing.
 *
 * @threadsafe
 */
class UtmProjection : public TransverseMercatorProjection
{
public:
  /** Creates the projection of zone @p zone (1–60). */
  explicit UtmProjection(int32_t zone = 30);

  QString id() const override;
  QString displayName() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;

  /** Returns the zone. */
  int32_t zone() const { return m_zone; }

  /** Returns the central meridian of @p zone, degrees. */
  static double zoneCentralMeridianDeg(int32_t zone);

private:
  int32_t m_zone;
};

/**
 * Cassini–Soldner on the WGS 84 ellipsoid, as PROJ's `cass` (Snyder's
 * series): the transverse equidistant cylindrical, true to scale along
 * the central meridian and along lines at right angles to it. The basis
 * of many older national and cadastral surveys. Shown within
 * maximumLonOffsetDeg() of the central meridian.
 *
 * @threadsafe
 */
class CassiniProjection : public LocalExpansionProjection
{
public:
  /** Creates the projection with central meridian @p centralMeridianDeg. */
  explicit CassiniProjection(double centralMeridianDeg = 0.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double maximumLonOffsetDeg() const override { return 30.0; }
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
};

} // namespace ts

#endif // TS_PROJECTION_TRANSVERSEMERCATOR_H
