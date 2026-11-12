// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The country outlines of the base pack, for choosing an offline region
 * by country.
 */
#ifndef TS_REGIONS_COUNTRYAREAS_H
#define TS_REGIONS_COUNTRYAREAS_H

#include <optional>
#include <vector>

#include <QString>

#include "ts/regions/regiongeometry.h"

namespace ts
{

class PackManifest;

/** One country of a CountryAreas file. */
struct CountryArea {
  /** Natural Earth's ADM0_A3 code, e.g. "GBR"; unique. */
  QString id;
  /** English name, e.g. "United Kingdom". */
  QString name;
  /** ISO 3166-1 alpha-2 code, or empty if it has none. */
  QString isoA2;
  /** ISO 3166-1 alpha-3 code, or empty if it has none. */
  QString isoA3;
};

/**
 * Reads the countries GeoPackage the packager writes (the base pack's
 * `countries` dataset, kind `areas`): one row per country with a
 * MULTIPOLYGON that covers the whole country, widened by a few
 * kilometres. See docs/design/offline-regions.md.
 *
 * The list is read when constructed; outlines are read on demand.
 *
 * @reentrant
 */
class CountryAreas
{
public:
  /** Reads the list of countries in the GeoPackage at @p path. */
  explicit CountryAreas(QString path);

  /**
   * Returns the countries file of the pack described by @p manifest in
   * @p directory (its first dataset of kind `areas`), or an empty string.
   */
  static QString fileInPack(const PackManifest& manifest,
                            const QString& directory);

  /** Returns true if the list was read. */
  bool isValid() const { return m_error.isEmpty(); }

  /** Returns why the file can't be used (English), if not valid. */
  const QString& errorString() const { return m_error; }

  /** Returns the countries, sorted by name. */
  const std::vector<CountryArea>& countries() const { return m_countries; }

  /** Returns the country with id @p id, or nothing. */
  std::optional<CountryArea> country(const QString& id) const;

  /**
   * Returns the outline of the country with id @p id, or nothing and
   * @p errorString (translated).
   */
  std::optional<RegionGeometry> area(const QString& id,
                                     QString* errorString) const;

private:
  QString m_path;
  QString m_error;
  std::vector<CountryArea> m_countries;
};

} // namespace ts

#endif // TS_REGIONS_COUNTRYAREAS_H
