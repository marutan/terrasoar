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
 * The azimuthal projections: Azimuthal Equidistant, Gnomonic and Polar
 * Stereographic.
 */
#ifndef TS_PROJECTION_AZIMUTHAL_H
#define TS_PROJECTION_AZIMUTHAL_H

#include "ts/projection/localexpansion.h"

namespace ts
{

/**
 * An azimuthal projection centred on any point, whose domain is the
 * points within domainLimitDeg() of the centre (along a great circle of
 * the sphere through the geodetic latitudes): what the azimuthal ones
 * share.
 *
 * @threadsafe
 */
class CentredAzimuthalProjection : public LocalExpansionProjection
{
public:
  double domainMarginDeg(double latDeg, double lonDeg) const override;
  bool hasOwnDomain() const override { return true; }
  Geodetic clampToDomain(double latDeg, double lonDeg) const override;
  std::array<float, 8> glslDomainParameters() const override;
  QByteArray glslDomainSource() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;

  /** Returns the centre's latitude, degrees. */
  double centreLatDeg() const { return m_centreLatDeg; }

  /** Returns how far from the centre the domain reaches, degrees. */
  virtual double domainLimitDeg() const = 0;

  /** Returns the great-circle angle from the centre to a point, degrees
   *  (on the sphere through geodetic latitudes). */
  double angleFromCentreDeg(double latDeg, double lonDeg) const;

protected:
  /** Creates the projection centred on @p centreLatDeg, @p centreLonDeg. */
  CentredAzimuthalProjection(double centreLatDeg, double centreLonDeg);

  /** The parameters' default centre (latitude, longitude), degrees. */
  virtual std::array<double, 2> defaultCentreDeg() const = 0;

private:
  double m_centreLatDeg;
};

/**
 * The Azimuthal Equidistant on the WGS 84 ellipsoid, as PROJ's `aeqd`
 * (GeographicLib's, by geodesics): distances and directions from the
 * centre are true. Centred on any point; the North Pole by default (the
 * polar case). Its domain reaches 175° from the centre: the antipode is a
 * circle.
 *
 * @threadsafe
 */
class AzimuthalEquidistantProjection : public CentredAzimuthalProjection
{
public:
  /** Creates the projection centred on @p centreLatDeg, @p centreLonDeg. */
  explicit AzimuthalEquidistantProjection(double centreLatDeg = 90.0,
                                          double centreLonDeg = 0.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  double domainLimitDeg() const override { return 175.0; }
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::array<double, 2> defaultCentreDeg() const override
  {
    return {90.0, 0.0};
  }
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
};

/**
 * The Gnomonic on the sphere of radius a (PROJ's `gnom +R=6378137`): the
 * great circles through the geodetic latitudes and longitudes are
 * straight lines, so routes plan as straight lines. (An ellipsoid has no
 * map on which its geodesics are all straight.) Its domain reaches 80°
 * from the centre: at 90° the map runs to infinity.
 *
 * @threadsafe
 */
class GnomonicProjection : public CentredAzimuthalProjection
{
public:
  /** Creates the projection centred on @p centreLatDeg, @p centreLonDeg. */
  explicit GnomonicProjection(double centreLatDeg = 45.0,
                              double centreLonDeg = 0.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  double domainLimitDeg() const override { return 80.0; }
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::array<double, 2> defaultCentreDeg() const override
  {
    return {45.0, 0.0};
  }
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
  double localExpansionLimitRadAt(double latRad, double dLonRad) const override;
};

/**
 * Polar Stereographic on the WGS 84 ellipsoid, as PROJ's `stere` with a
 * latitude of true scale (variant B): conformal, the pole at the centre,
 * the central meridian straight down from it. North by default as
 * EPSG:3413 (true scale at 70° N, 45° W down), south as EPSG:3031 (71° S,
 * 0° up). Shown over its hemisphere and 30° beyond the equator; the map
 * goes all the way round the pole, with no cut.
 *
 * @threadsafe
 */
class PolarStereographicProjection : public LocalExpansionProjection
{
public:
  /**
   * Creates the north (@p north) or south polar map, true to scale at
   * latitude @p trueScaleLatDeg (of that hemisphere; the pole for scale
   * 1 there), central meridian @p centralMeridianDeg.
   */
  PolarStereographicProjection(bool north, double trueScaleLatDeg,
                               double centralMeridianDeg);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double minimumLatDeg() const override { return m_north ? -30.0 : -90.0; }
  double maximumLatDeg() const override { return m_north ? 90.0 : 30.0; }
  MapPoint project(double latRad, double dLonRad) const override;

  /** Returns true for the north polar map. */
  bool north() const { return m_north; }

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;

private:
  bool m_north;
  double m_trueScaleLatDeg;
  /** ρ = m_rhoPerT · t(±φ), metres. */
  double m_rhoPerT = 0.0;
};

} // namespace ts

#endif // TS_PROJECTION_AZIMUTHAL_H
