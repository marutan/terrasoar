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
 * The base of projections that aren't cylindrical: their shaders use a
 * local expansion for small tiles and the plain formula for large ones.
 */
#ifndef TS_PROJECTION_LOCALEXPANSION_H
#define TS_PROJECTION_LOCALEXPANSION_H

#include <array>
#include <mutex>
#include <optional>

#include "ts/projection/projection.h"

namespace ts
{

/**
 * A projection whose x and y depend on both the latitude and the
 * longitude (Transverse Mercator, conics, azimuthals…), drawn on the GPU
 * in one of two ways, chosen per tile:
 *
 * - **Small tiles** (reaching less than localExpansionLimitRad() from
 *   their reference point): the second-order Taylor expansion of the
 *   projection about the reference,
 *
 *       F(ref + d) − F(ref) ≈ J d + ½ dᵀ H d,
 *
 *   with J and H worked out in double precision on the CPU (by central
 *   differences of project()). Its error, a third derivative times d³,
 *   is far below a pixel at such sizes, and nothing large is subtracted
 *   in float.
 * - **Large tiles**: the projection's own formula in float, glslForward(),
 *   at the vertex and at the reference, subtracted. Float loses about a
 *   metre at Earth scale, far below a pixel of a tile that size.
 *
 * Subclasses give project() (the formulas in double), a starting point
 * for the inverse (refined here by Newton's method on project(), so
 * inverse() is exact for this forward()), the GLSL forward and its
 * parameters. Everything else (bounds, scale factors, the shader
 * function) is done here. The longitude domain is limited by
 * maximumLonOffsetDeg(); the map doesn't repeat.
 *
 * @threadsafe
 */
class LocalExpansionProjection : public Projection
{
public:
  /** Half-span below which a tile uses the Taylor expansion, radians. */
  static constexpr double localExpansionLimitRad() { return 0.01; }

  double minimumLatDeg() const override { return -90.0; }
  double maximumLatDeg() const override { return 90.0; }
  MapPoint forward(double latDeg, double lonDeg) const override;
  std::optional<Geodetic> inverse(MapPoint point) const override;
  std::optional<Geodetic> nearestInDomain(MapPoint point) const override;
  MapRect bounds() const override;
  MapScale scaleAt(double latDeg) const override;
  MapScale scaleAtPoint(double latDeg, double lonDeg) const override;
  bool repeats() const override { return false; }
  QByteArray glslSource() const override;
  ProjectionShaderReference shaderReference(double latDeg,
                                            double lonDeg) const override;
  ProjectionShaderReference
  shaderReferenceFor(double latDeg, double lonDeg,
                     double halfSpanRad) const override;

  /**
   * Returns the map position of latitude @p latRad and longitude
   * @p dLonRad east of the central meridian, both radians, without any
   * limits: it must be smooth a little beyond the domain (the derivatives
   * are taken there).
   */
  virtual MapPoint project(double latRad, double dLonRad) const = 0;

protected:
  /** Creates the projection centred on @p centralMeridianDeg. */
  explicit LocalExpansionProjection(double centralMeridianDeg);

  /**
   * Returns a first guess of the point at @p point (latitude, longitude
   * east of the central meridian, radians), or nothing far off the map.
   * inverse() refines it.
   */
  virtual std::optional<std::array<double, 2>>
  approximateInverse(MapPoint point) const = 0;

  /**
   * Returns the GLSL source of
   *
   *     vec2 <glslFunction()>Forward(vec2 lonLatRad, vec4 p0, vec4 p1)
   *
   * the map position in float of a longitude east of the central meridian
   * and a latitude, radians, with p0 and p1 from glslParameters().
   */
  virtual QByteArray glslForwardSource() const = 0;

  /** Returns p0 and p1 of the GLSL forward. */
  virtual std::array<std::array<float, 4>, 2> glslParameters() const = 0;

  /**
   * Returns the half-span below which a tile about latitude @p latRad,
   * @p dLonRad east of the central meridian, uses the Taylor expansion
   * (radians): localExpansionLimitRad() by default. A projection that
   * isn't smooth somewhere (a cone's apex) gives less near there, so that
   * tiles near it use the formula.
   */
  virtual double localExpansionLimitRadAt(double latRad, double dLonRad) const;

  /**
   * Returns p0 and p1 of the GLSL forward for a large tile about latitude
   * @p latRad, @p dLonRad east of the central meridian (radians). The
   * default is glslParameters(); a projection whose forward depends on
   * the place (a datum shift computed approximately in the shader, made
   * exact at the reference) gives its corrections here.
   */
  virtual std::array<std::array<float, 4>, 2>
  glslParametersAt(double latRad, double dLonRad) const;

private:
  /** The expansion's coefficients about a point (radians). */
  struct Expansion {
    /** ∂x/∂λ, ∂x/∂φ, ∂y/∂λ, ∂y/∂φ, metres per radian. */
    std::array<double, 4> jacobian{};
    /** ∂²x/∂λ², ∂²x/∂λ∂φ, ∂²x/∂φ², metres per radian². */
    std::array<double, 3> hessianX{};
    /** The same for y. */
    std::array<double, 3> hessianY{};
  };

  Expansion expansionAt(double latRad, double dLonRad) const;

  /** inverse() without the domain: latitude and longitude east of the
   *  central meridian (radians) whose project() is @p point, if found. */
  std::optional<std::array<double, 2>> solve(MapPoint point) const;

  mutable std::once_flag m_boundsOnce;
  mutable MapRect m_bounds;
};

} // namespace ts

#endif // TS_PROJECTION_LOCALEXPANSION_H
