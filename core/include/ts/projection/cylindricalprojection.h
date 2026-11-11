// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The cylindrical projections: Mercator, Web Mercator and Miller.
 */
#ifndef TS_PROJECTION_CYLINDRICALPROJECTION_H
#define TS_PROJECTION_CYLINDRICALPROJECTION_H

#include "ts/projection/projection.h"

namespace ts
{

/**
 * The family the three projections of the 2D views belong to: normal
 * cylindrical projections with
 *
 *     x = a (λ − λ0)
 *     y = a (c atanh(sin(s φ)) − e atanh(e sin(s φ)))
 *
 * for longitude λ, latitude φ and the WGS 84 semi-major axis a. Meridians
 * are evenly spaced vertical lines and parallels horizontal lines, so the
 * map repeats every 2πa in x. Subclasses choose s, c and e and the
 * latitude limit.
 *
 * @threadsafe
 */
class CylindricalProjection : public Projection
{
public:
  /** The constants of the family. */
  struct Parameters {
    /** Factor s on the latitude. */
    double latitudeFactor = 1.0;
    /** Factor c on the first term. */
    double scale = 1.0;
    /** Eccentricity e (0 for the spherical formulas). */
    double eccentricity = 0.0;
    /** The map is cut at ± this latitude, degrees. */
    double maximumLatDeg = 90.0;
  };

  /** Returns the constants. */
  const Parameters& parameters() const { return m_parameters; }

  double minimumLatDeg() const override;
  double maximumLatDeg() const override;
  MapPoint forward(double latDeg, double lonDeg) const override;
  std::optional<Geodetic> inverse(const MapPoint& point) const override;
  MapRect bounds() const override;
  MapScale scaleAt(double latDeg) const override;
  bool isCylindrical() const override { return true; }
  ProjectionShaderReference shaderReference(double latDeg,
                                            double lonDeg) const override;
  QByteArray glslSource() const override;

  /** Returns the northing of @p latDeg (clamped to the limits), metres. */
  double northingM(double latDeg) const;

  /**
   * Returns the latitude with northing @p yM, degrees; beyond the limits
   * for a northing off the map.
   */
  double latitudeDeg(double yM) const;

  /**
   * Returns the width of the world, 2πa metres: the map repeats with this
   * period in x.
   */
  static double worldWidthM();

protected:
  /** Creates the projection with @p parameters. */
  CylindricalProjection(const Parameters& parameters,
                        double centralMeridianDeg);

private:
  Parameters m_parameters;
};

/**
 * The ellipsoidal Mercator projection of WGS 84 (as EPSG:3395): conformal;
 * straight lines are lines of constant bearing. Cut at about ±85.08°,
 * where the map is square.
 *
 * @threadsafe
 */
class MercatorProjection : public CylindricalProjection
{
public:
  /** Creates the projection centred on @p centralMeridianDeg. */
  explicit MercatorProjection(double centralMeridianDeg = 0.0);
  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
};

/**
 * Web Mercator (EPSG:3857): the spherical Mercator formulas applied to
 * WGS 84 latitudes, as web maps use. Not quite conformal. Cut at about
 * ±85.05°, where the map is square.
 *
 * @threadsafe
 */
class WebMercatorProjection : public CylindricalProjection
{
public:
  /** Creates the projection centred on @p centralMeridianDeg. */
  explicit WebMercatorProjection(double centralMeridianDeg = 0.0);
  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
};

/**
 * The Miller cylindrical projection (spherical formulas, radius a): a
 * compromise that shows the poles, neither conformal nor equal-area.
 *
 * @threadsafe
 */
class MillerProjection : public CylindricalProjection
{
public:
  /** Creates the projection centred on @p centralMeridianDeg. */
  explicit MillerProjection(double centralMeridianDeg = 0.0);
  QString id() const override;
  QString displayName() const override;
  QString projString() const override;
  QByteArray glslFunction() const override;
};

} // namespace ts

#endif // TS_PROJECTION_CYLINDRICALPROJECTION_H
