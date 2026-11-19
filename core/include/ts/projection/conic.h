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
 * The conic projections: Lambert Conformal Conic and the American
 * Polyconic.
 */
#ifndef TS_PROJECTION_CONIC_H
#define TS_PROJECTION_CONIC_H

#include "ts/projection/localexpansion.h"

namespace ts
{

/**
 * Lambert Conformal Conic on the WGS 84 ellipsoid, as PROJ's `lcc`: with
 * two standard parallels (secant: true to scale along both) or one (equal
 * parallels: tangent there). Conformal. The standard projection of
 * aeronautical charts and many national and continental grids; by
 * default EPSG:3034's (Europe: 35° and 65° N, origin 52° N 10° E).
 *
 * The map is a fan about the cone's apex (a pole). Its domain is its
 * own: within 170° of the central meridian, so the fan's cut opposite it
 * is never drawn, and from the apex's pole to 60° beyond the nearer
 * standard parallel (the far pole is at infinity).
 *
 * @threadsafe
 */
class LambertConformalConicProjection : public LocalExpansionProjection
{
public:
  /**
   * Creates the projection with standard parallels @p standardParallel1Deg
   * and @p standardParallel2Deg (equal for one), origin latitude
   * @p originLatDeg (where y = 0 on the central meridian) and central
   * meridian @p centralMeridianDeg. Parallels symmetric about the equator
   * (the cylindrical limit) are moved apart to a sum of 1°.
   */
  explicit LambertConformalConicProjection(double standardParallel1Deg = 35.0,
                                           double standardParallel2Deg = 65.0,
                                           double originLatDeg = 52.0,
                                           double centralMeridianDeg = 10.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double minimumLatDeg() const override { return m_minimumLatDeg; }
  double maximumLatDeg() const override { return m_maximumLatDeg; }
  double domainMarginDeg(double latDeg, double lonDeg) const override;
  bool hasOwnDomain() const override { return true; }
  Geodetic clampToDomain(double latDeg, double lonDeg) const override;
  std::array<float, 8> glslDomainParameters() const override;
  QByteArray glslDomainSource() const override;
  MapPoint project(double latRad, double dLonRad) const override;

  /** Returns the cone constant n (sin of the parallel for one). */
  double coneConstant() const { return m_n; }

  /** How far from the central meridian the domain reaches, degrees. */
  static constexpr double lonLimitDeg() { return 170.0; }

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;
  double localExpansionLimitRadAt(double latRad, double dLonRad) const override;

private:
  double m_parallel1Deg;
  double m_parallel2Deg;
  double m_originLatDeg;
  double m_minimumLatDeg = -90.0;
  double m_maximumLatDeg = 90.0;
  /** PROJ's n, c and rho0, metres for c and rho0. */
  double m_n = 0.0;
  double m_c = 0.0;
  double m_rho0 = 0.0;
};

/**
 * The American Polyconic on the WGS 84 ellipsoid, as PROJ's `poly`: each
 * parallel is the arc of its own tangent cone, true to scale, as is the
 * central meridian; neither conformal nor equal-area. Used for the older
 * US topographic series. Shown within maximumLonOffsetDeg() of the
 * central meridian (near ±90° its meridians fold over each other at high
 * latitudes).
 *
 * @threadsafe
 */
class PolyconicProjection : public LocalExpansionProjection
{
public:
  /**
   * Creates the projection with central meridian @p centralMeridianDeg and
   * origin latitude @p originLatDeg (y = 0 there on the meridian).
   */
  explicit PolyconicProjection(double centralMeridianDeg = 0.0,
                               double originLatDeg = 0.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double maximumLonOffsetDeg() const override { return 75.0; }
  MapPoint project(double latRad, double dLonRad) const override;

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;

private:
  double m_originLatDeg;
  /** The meridian arc to the origin latitude, metres. */
  double m_originArcM;
};

} // namespace ts

#endif // TS_PROJECTION_CONIC_H
