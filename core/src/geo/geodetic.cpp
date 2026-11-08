// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/geo/geodetic.h"

#include <cmath>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"

namespace ts
{

namespace
{

constexpr double a = wgs84::semiMajorAxisM;
constexpr double b = wgs84::semiMinorAxisM;
constexpr double e2 = wgs84::eccentricitySquared;
constexpr double e4 = e2 * e2;

} // namespace

double normaliseLonDeg(double lonDeg)
{
  double result = std::fmod(lonDeg + 180.0, 360.0);
  if (result < 0.0) {
    result += 360.0;
  }
  return result - 180.0;
}

Vec3 geodeticToEcef(const Geodetic& position)
{
  const double lat = degToRad(position.latDeg);
  const double lon = degToRad(position.lonDeg);
  const double sinLat = std::sin(lat);
  const double cosLat = std::cos(lat);
  // Prime-vertical radius of curvature.
  const double n = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
  const double horizontal = (n + position.heightM) * cosLat;
  return Vec3{horizontal * std::cos(lon), horizontal * std::sin(lon),
              (n * (1.0 - e2) + position.heightM) * sinLat};
}

Geodetic ecefToGeodetic(const Vec3& positionEcef)
{
  const double x = positionEcef.x;
  const double y = positionEcef.y;
  const double z = positionEcef.z;
  const double horizontal = std::hypot(x, y);

  // On the polar axis the general formula below is 0/0.
  if (horizontal < 1e-9) {
    return Geodetic{z >= 0.0 ? 90.0 : -90.0, 0.0, std::abs(z) - b};
  }

  // H. Vermeille, "An analytical method to transform geocentric into
  // geodetic coordinates", J. Geodesy 85 (2011), section 2.
  const double p = (x * x + y * y) / (a * a);
  const double q = (1.0 - e2) / (a * a) * z * z;
  const double r = (p + q - e4) / 6.0;
  const double s = e4 * p * q / (4.0 * r * r * r);
  const double t = std::cbrt(1.0 + s + std::sqrt(s * (2.0 + s)));
  const double u = r * (1.0 + t + 1.0 / t);
  const double v = std::sqrt(u * u + e4 * q);
  const double w = e2 * (u + v - q) / (2.0 * v);
  const double k = std::sqrt(u + v + w * w) - w;
  const double d = k * horizontal / (k + e2);
  const double dz = std::hypot(d, z);

  const double latRad = 2.0 * std::atan2(z, d + dz);
  const double lonRad = std::atan2(y, x);
  const double heightM = (k + e2 - 1.0) / k * dz;
  return Geodetic{radToDeg(latRad), radToDeg(lonRad), heightM};
}

Vec3 surfaceNormalEcef(double latDeg, double lonDeg)
{
  const double lat = degToRad(latDeg);
  const double lon = degToRad(lonDeg);
  const double cosLat = std::cos(lat);
  return Vec3{cosLat * std::cos(lon), cosLat * std::sin(lon), std::sin(lat)};
}

Vec3 ellipsoidNormalEcef(const Vec3& pointEcef)
{
  return normalized(Vec3{pointEcef.x / (a * a), pointEcef.y / (a * a),
                         pointEcef.z / (b * b)});
}

LocalFrame localFrame(double latDeg, double lonDeg)
{
  const double lat = degToRad(latDeg);
  const double lon = degToRad(lonDeg);
  const double sinLat = std::sin(lat);
  const double cosLat = std::cos(lat);
  const double sinLon = std::sin(lon);
  const double cosLon = std::cos(lon);
  return LocalFrame{Vec3{-sinLon, cosLon, 0.0},
                    Vec3{-sinLat * cosLon, -sinLat * sinLon, cosLat},
                    Vec3{cosLat * cosLon, cosLat * sinLon, sinLat}};
}

} // namespace ts
