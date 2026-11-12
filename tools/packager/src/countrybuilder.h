// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Builds the base pack's country outlines, for choosing an offline region
 * by country.
 */
#ifndef TS_PACKAGER_COUNTRYBUILDER_H
#define TS_PACKAGER_COUNTRYBUILDER_H

#include <functional>
#include <vector>

#include <QString>

namespace ts::packager
{

/** What to build. */
struct CountryOptions {
  /**
   * The Natural Earth admin-0 countries file (GeoJSON or anything else
   * GDAL reads, with the fields ADM0_A3, ADMIN, ISO_A2_EH and ISO_A3_EH).
   */
  QString sourcePath;
  /** The GeoPackage to create. */
  QString outputPath;
  /**
   * How far each outline is widened, degrees, so that the region covers
   * the coast and every island. Must exceed simplifyDeg.
   */
  double bufferDeg = 0.05;
  /** The widened outline is simplified within this, degrees. */
  double simplifyDeg = 0.02;
  /** ADM0_A3 codes left out (Antarctica: no polygon can go round a pole). */
  std::vector<QString> exclude;
  /** Called with progress messages (English); may be empty. */
  std::function<void(const QString&)> progress;
};

/** What was built. */
struct CountryStats {
  /** Countries written. */
  int countries = 0;
  /** Countries left out (CountryOptions::exclude). */
  int excluded = 0;
  /** Vertices read. */
  long long sourceVertices = 0;
  /** Rings written (outer rings and holes). */
  long long rings = 0;
  /** Vertices written. */
  long long vertices = 0;
};

/**
 * Writes one row per country to the table `countries` of a new
 * GeoPackage: `id` (ADM0_A3), `name` (ADMIN), `iso_a2`, `iso_a3` (empty
 * where Natural Earth has none) and a MULTIPOLYGON. Each outline is the
 * country's polygons widened by bufferDeg and simplified within
 * simplifyDeg, so it covers the whole country; a country across the
 * antimeridian has its western parts moved by 360° to keep it in one
 * piece (longitudes then go past 180°).
 *
 * Fails, with @p errorString, if a source can't be read or written, or if
 * an outline is invalid or doesn't cover its country.
 */
bool buildCountries(const CountryOptions& options, CountryStats* stats,
                    QString* errorString);

} // namespace ts::packager

#endif // TS_PACKAGER_COUNTRYBUILDER_H
