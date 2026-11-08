// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/geo/geoformat.h"

#include <cmath>

#include <QCoreApplication>

#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

/** Decimal places of degrees: 1e-5° is about a metre. */
constexpr int degreeDecimals = 5;

QString degrees(double valueDeg, const QLocale& locale)
{
  return locale.toString(std::abs(valueDeg), 'f', degreeDecimals);
}

} // namespace

QString formatLatitude(double latDeg, const QLocale& locale)
{
  // Rounded to zero shows no hemisphere sign flip ("0.00000° N").
  const bool south =
      latDeg < 0.0 && std::abs(latDeg) >= 0.5 * std::pow(10.0, -degreeDecimals);
  return south ? QCoreApplication::translate("ts::GeoFormat", "%1° S")
                     .arg(degrees(latDeg, locale))
               : QCoreApplication::translate("ts::GeoFormat", "%1° N")
                     .arg(degrees(latDeg, locale));
}

QString formatLongitude(double lonDeg, const QLocale& locale)
{
  double lon = normaliseLonDeg(lonDeg);
  // −180 and 180 are the same meridian; show it as 180° E.
  if (lon <= -180.0 + 0.5 * std::pow(10.0, -degreeDecimals)) {
    lon = 180.0;
  }
  const bool west =
      lon < 0.0 && std::abs(lon) >= 0.5 * std::pow(10.0, -degreeDecimals);
  return west ? QCoreApplication::translate("ts::GeoFormat", "%1° W")
                    .arg(degrees(lon, locale))
              : QCoreApplication::translate("ts::GeoFormat", "%1° E")
                    .arg(degrees(lon, locale));
}

QString formatDistance(double distanceM, const QLocale& locale)
{
  const double absolute = std::abs(distanceM);
  if (absolute < 10.0) {
    return QCoreApplication::translate("ts::GeoFormat", "%1 m")
        .arg(locale.toString(distanceM, 'f', 1));
  }
  if (absolute < 1000.0) {
    return QCoreApplication::translate("ts::GeoFormat", "%1 m")
        .arg(locale.toString(distanceM, 'f', 0));
  }
  if (absolute < 100000.0) {
    return QCoreApplication::translate("ts::GeoFormat", "%1 km")
        .arg(locale.toString(distanceM / 1000.0, 'f', 1));
  }
  return QCoreApplication::translate("ts::GeoFormat", "%1 km")
      .arg(locale.toString(distanceM / 1000.0, 'f', 0));
}

} // namespace ts
