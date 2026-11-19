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
 * Normal cylindrical projections with their own northing function:
 * Equidistant Cylindrical, Gall Stereographic and Gall–Peters.
 */
#ifndef TS_PROJECTION_NORMALCYLINDRICAL_H
#define TS_PROJECTION_NORMALCYLINDRICAL_H

#include "ts/projection/projection.h"

namespace ts
{

/**
 * A normal cylindrical projection:
 *
 *     x = a k (λ − λ0)
 *     y = a f(φ)
 *
 * for the WGS 84 semi-major axis a, a constant k and a northing function
 * f of the latitude alone, which subclasses give (with its inverse and
 * derivative). Meridians are evenly spaced vertical lines and parallels
 * horizontal lines, so the map repeats every 2πak in x. Scale factors
 * are on the WGS 84 ellipsoid whatever f assumes.
 *
 * Unlike CylindricalProjection (one family of formulas, chosen by
 * constants), each subclass brings its own f and its own GLSL, written
 * so that the northing difference from a tile's centre is computed
 * without subtracting nearly equal numbers.
 *
 * @threadsafe
 */
class NormalCylindricalProjection : public Projection
{
public:
  double minimumLatDeg() const override;
  double maximumLatDeg() const override;
  MapPoint forward(double latDeg, double lonDeg) const override;
  std::optional<Geodetic> inverse(MapPoint point) const override;
  MapRect bounds() const override;
  MapScale scaleAt(double latDeg) const override;
  bool isCylindrical() const override { return true; }

  /** Returns k, the scale of x on the longitude. */
  double xScale() const { return m_xScale; }

  /** Returns the northing of @p latDeg (clamped to the limits), metres. */
  double northingM(double latDeg) const;

protected:
  /**
   * Creates the projection with x scale @p xScale, cut at ±
   * @p maximumLatDeg.
   */
  NormalCylindricalProjection(double xScale, double maximumLatDeg);

  /** Returns f(φ) for @p latRad. */
  virtual double unitNorthing(double latRad) const = 0;
  /** Returns the latitude (radians) whose f is @p unitY. */
  virtual double unitLatitude(double unitY) const = 0;
  /** Returns df/dφ at @p latRad. */
  virtual double unitNorthingDerivative(double latRad) const = 0;

  /** Returns the clamped latitude span r0.zw shaders use (radians). */
  std::array<float, 2> latitudeSpan(double latRad) const;

private:
  double m_xScale;
  double m_maximumLatDeg;
};

/**
 * Equidistant Cylindrical (PROJ `eqc`): x = a cos φ1 λ, y = a φ, true to
 * scale along every meridian and along the standard parallels ±φ1. Plate
 * Carrée is the case φ1 = 0. Spherical formulas with radius a, as PROJ's.
 *
 * @threadsafe
 */
class EquidistantCylindricalProjection : public NormalCylindricalProjection
{
public:
  /** Creates the projection with standard parallel @p standardParallelDeg. */
  explicit EquidistantCylindricalProjection(double standardParallelDeg = 0.0);

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  std::vector<ProjectionParameter> parameters() const override;
  ProjectionParameterValues parameterValues() const override;
  QByteArray glslFunction() const override;
  QByteArray glslSource() const override;
  ProjectionShaderReference shaderReference(double latDeg,
                                            double lonDeg) const override;

protected:
  double unitNorthing(double latRad) const override;
  double unitLatitude(double unitY) const override;
  double unitNorthingDerivative(double latRad) const override;

private:
  double m_standardParallelDeg;
};

/**
 * Gall Stereographic (PROJ `gall`): x = a λ / √2,
 * y = a (1 + √2/2) tan(φ/2); a cylinder secant at 45° N and S, projected
 * stereographically, a compromise for world maps. Spherical formulas with
 * radius a, as PROJ's.
 *
 * @threadsafe
 */
class GallStereographicProjection : public NormalCylindricalProjection
{
public:
  /** Creates the projection. */
  GallStereographicProjection();

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  QByteArray glslSource() const override;
  ProjectionShaderReference shaderReference(double latDeg,
                                            double lonDeg) const override;

protected:
  double unitNorthing(double latRad) const override;
  double unitLatitude(double unitY) const override;
  double unitNorthingDerivative(double latRad) const override;
};

/**
 * Gall–Peters, also called Gall Orthographic (PROJ
 * `cea +lat_ts=45`): the cylindrical equal-area projection on the WGS 84
 * ellipsoid with standard parallels at 45°:
 *
 *     x = a k0 λ,  y = a q(φ) / (2 k0)
 *     k0 = cos φ1 / √(1 − e² sin² φ1)
 *     q(φ) = (1 − e²) (sin φ / (1 − e² sin² φ) + atanh(e sin φ) / e)
 *
 * Areas are true everywhere; shapes are stretched north–south near the
 * equator and squashed near the poles.
 *
 * @threadsafe
 */
class GallPetersProjection : public NormalCylindricalProjection
{
public:
  /** Creates the projection. */
  GallPetersProjection();

  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
  QByteArray glslSource() const override;
  ProjectionShaderReference shaderReference(double latDeg,
                                            double lonDeg) const override;

protected:
  double unitNorthing(double latRad) const override;
  double unitLatitude(double unitY) const override;
  double unitNorthingDerivative(double latRad) const override;
};

} // namespace ts

#endif // TS_PROJECTION_NORMALCYLINDRICAL_H
