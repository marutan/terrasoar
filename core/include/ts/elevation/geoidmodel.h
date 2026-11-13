// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * A geoid model: the height of the geoid (mean sea level) above the WGS 84
 * ellipsoid, from a PROJ grid.
 */
#ifndef TS_ELEVATION_GEOIDMODEL_H
#define TS_ELEVATION_GEOIDMODEL_H

#include <memory>
#include <vector>

#include <QString>

#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * The geoid undulation N from a grid in PROJ's GeoTIFF format (such as
 * the base pack's EGM2008 and EGM96 grids), interpolated bilinearly by
 * PROJ's `vgridshift`. A height H above the geoid is h = H + N above the
 * ellipsoid. See docs/design/vertical-datums.md.
 *
 * @threadsafe
 */
class GeoidModel
{
public:
  /**
   * Opens the grid @p gridFile in @p directory (found through PROJ's
   * search path, so the directory may contain spaces); check isValid().
   */
  GeoidModel(const QString& directory, const QString& gridFile);
  ~GeoidModel();

  GeoidModel(const GeoidModel&) = delete;
  GeoidModel& operator=(const GeoidModel&) = delete;
  GeoidModel(GeoidModel&&) = delete;
  GeoidModel& operator=(GeoidModel&&) = delete;

  /** Returns true if the grid was found and can be read. */
  bool isValid() const;

  /** Returns why the model isn't valid (English), or an empty string. */
  QString errorString() const;

  /** Returns the grid's file name. */
  const QString& gridFile() const { return m_gridFile; }

  /**
   * Returns the undulation N at (@p latDeg, @p lonDeg), metres (positive
   * where the geoid is above the ellipsoid), or NaN if it can't be had.
   */
  double undulationM(double latDeg, double lonDeg) const;

  /** Returns undulationM() for each of @p points, in one call. */
  std::vector<double> undulationsM(const std::vector<GeoPoint>& points) const;

private:
  struct Private;
  std::unique_ptr<Private> d;
  QString m_gridFile;
};

/**
 * Turns @p tile's heights, sampled at @p points (one per sample), into
 * heights above the ellipsoid: adds the undulation of @p datum, the geoid
 * they are measured from (null for heights already above the ellipsoid),
 * and sets HeightTile::seaLevelM from @p datum, or @p seaLevel where
 * @p datum is null (both null: no sea level). NaN heights stay NaN.
 */
void applyVerticalDatum(HeightTile& tile, const std::vector<GeoPoint>& points,
                        const GeoidModel* datum, const GeoidModel* seaLevel);

} // namespace ts

#endif // TS_ELEVATION_GEOIDMODEL_H
