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
 * Rotated Mercator: the oblique Mercator (Hotine) about a chosen centre
 * line.
 */
#ifndef TS_PROJECTION_ROTATEDMERCATOR_H
#define TS_PROJECTION_ROTATEDMERCATOR_H

#include <array>
#include <optional>

#include "ts/projection/localexpansion.h"

namespace ts
{

/**
 * The oblique Mercator on the WGS 84 ellipsoid (Hotine's, as PROJ's
 * `omerc` with its default rectification and u offset): Mercator about a
 * rotated pole, so that a chosen centre line, the great circle through
 * the centre at the given azimuth, is a straight line true to scale
 * (times the scale factor). The map is turned so that north is up at the
 * centre, which is at x = y = 0. For long, narrow areas that don't run
 * north–south.
 *
 * Its domain is its own (domainMarginDeg()): within 85° of the centre
 * line in the rotated frame (Mercator's limit, short of the rotated poles
 * where the map runs to infinity), 150° along it either way from the
 * centre (short of the rotated antimeridian, where the map is cut), and,
 * as Hotine's B is a little over 1, within π / B of the meridian λ0
 * (beyond it the map would cover itself again: a sliver about 0.6° wide
 * on the far side of the Earth is left out).
 *
 * @threadsafe
 */
class RotatedMercatorProjection : public LocalExpansionProjection
{
public:
  /**
   * Creates the projection centred on latitude @p centreLatDeg, longitude
   * @p centreLonDeg, the centre line leaving it at azimuth @p azimuthDeg
   * (clockwise from north), with scale factor @p scaleFactor on it.
   */
  explicit RotatedMercatorProjection(double centreLatDeg = 45.0,
                                     double centreLonDeg = 0.0,
                                     double azimuthDeg = 45.0,
                                     double scaleFactor = 1.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  double domainMarginDeg(double latDeg, double lonDeg) const override;
  bool hasOwnDomain() const override { return true; }
  Geodetic clampToDomain(double latDeg, double lonDeg) const override;
  std::array<float, 8> glslDomainParameters() const override;
  QByteArray glslDomainSource() const override;
  MapRect bounds() const override;
  MapPoint project(double latRad, double dLonRad) const override;

  /** Returns the centre's latitude, degrees. */
  double centreLatDeg() const { return m_centreLatDeg; }

  /** Returns the centre line's azimuth at the centre, degrees. */
  double azimuthDeg() const { return m_azimuthDeg; }

  /** Returns the scale factor on the centre line. */
  double scaleFactor() const { return m_scaleFactor; }

  /**
   * Returns the latitude and longitude, in the rotated frame, of latitude
   * @p latDeg and longitude @p lonDeg: {across the centre line, along it
   * from the centre}, degrees (on the conformal sphere Hotine's
   * projection works on). The centre line is latitude 0, the centre
   * {0, 0}.
   */
  std::array<double, 2> rotatedDeg(double latDeg, double lonDeg) const;

  /**
   * Returns the map offset of a whole turn along the centre line. Where
   * the line passes PROJ's cut (half a turn from where it crosses the
   * aposphere's equator, not from the centre), PROJ's positions are this
   * much from these, which run on across it.
   */
  MapPoint alongTurnM() const;

  /** How far from the centre line the domain reaches, degrees. */
  static constexpr double acrossLimitDeg() { return 85.0; }

  /** How far along the centre line the domain reaches, degrees. */
  static constexpr double alongLimitDeg() { return 150.0; }

  /**
   * Returns the parameters (centre, azimuth; scale factor 1) of the
   * centre line through latitude @p lat1Deg, longitude @p lon1Deg and
   * @p lat2Deg, @p lon2Deg: centred halfway along the geodesic between
   * them, at its azimuth there. Nothing for coincident or antipodal
   * points.
   */
  static std::optional<ProjectionParameterValues>
  throughPoints(double lat1Deg, double lon1Deg, double lat2Deg, double lon2Deg);

protected:
  std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const override;
  QByteArray glslForwardSource() const override;
  std::array<std::array<float, 4>, 2> glslParameters() const override;

private:
  /** u and v before the rectification (PROJ's names), and the rotated
   *  frame's U (the sine of the rotated latitude, negated) and angle
   *  along the centre line. */
  struct Native {
    double u = 0.0;
    double v = 0.0;
    double sinAcross = 0.0;
    double alongRad = 0.0;
    /** The longitude from λ0, radians, wrapped to ±π. */
    double lamRad = 0.0;
  };

  Native native(double latRad, double dLonRad) const;

  double m_centreLatDeg;
  double m_azimuthDeg;
  double m_scaleFactor;
  /** Hotine's constants, as PROJ's omerc names them. */
  double m_a = 0.0;
  double m_b = 0.0;
  double m_e = 0.0;
  double m_arb = 0.0;
  double m_singam = 0.0;
  double m_cosgam = 0.0;
  double m_sinrot = 0.0;
  double m_cosrot = 0.0;
  double m_u0 = 0.0;
  /** The centre's angle along the line from where it crosses the
   *  aposphere's equator (PROJ's u0 / (A / B)), and its sine and cosine. */
  double m_centreAlongRad = 0.0;
  double m_sinCentre = 0.0;
  double m_cosCentre = 0.0;
  /** v at the geographic north and south poles. */
  double m_vPoleN = 0.0;
  double m_vPoleS = 0.0;
  /** PROJ's lam0 (where the aposphere's longitude is 0) minus the
   *  centre's longitude, radians. */
  double m_lam0OffsetRad = 0.0;
};

} // namespace ts

#endif // TS_PROJECTION_ROTATEDMERCATOR_H
