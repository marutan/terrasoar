// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Elevation from a raster DEM file read with GDAL (GeoTIFF, COG, …).
 */
#ifndef TS_ELEVATION_GDALELEVATIONPROVIDER_H
#define TS_ELEVATION_GDALELEVATIONPROVIDER_H

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <QString>

#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

class GeoidModel;

/**
 * What a DEM file's heights are measured from, as found in its
 * coordinate reference system.
 */
enum class DemVerticalDatum {
  /** The file doesn't say. */
  Unknown,
  /** Heights above the WGS 84 ellipsoid (e.g. EPSG:4979). */
  Ellipsoid,
  /** Heights above EGM2008 (e.g. EPSG:3855). */
  Egm2008,
  /** Heights above EGM96 (e.g. EPSG:5773). */
  Egm96,
  /**
   * Heights above Ordnance Datum Newlyn (EPSG:5701, Great Britain), which
   * the OSGM15 geoid pack converts.
   */
  Odn,
};

/**
 * Returns the vertical datum declared by the coordinate reference system
 * of the raster at @p path, Unknown if it declares none GDAL can name.
 */
DemVerticalDatum detectDemVerticalDatum(const QString& path);

/**
 * Returns the dataset id of @p datum ("egm2008", "egm96", "osgm15",
 * "ellipsoid"; empty for Unknown).
 */
QString demVerticalDatumId(DemVerticalDatum datum);

/**
 * Heights from the first band of a raster DEM any GDAL driver reads, in
 * any coordinate reference system GDAL can turn latitude and longitude
 * into (geographic or projected). Each tile's samples are the raster
 * reprojected and averaged to the sample spacing (from the file's
 * overviews where it has them), read through windows aligned to a grid
 * that is the same for every tile, so neighbouring tiles share their edge
 * values. Samples outside the raster or on its no-data value are NaN
 * ("no data", for CompositeElevationProvider); a tile without any data is
 * nothing. Heights are in metres, after the band's scale and offset.
 *
 * Without overviews, tiles more than 64 source pixels to a sample are not
 * read (they would read most of the file for a few samples); `gdaladdo`
 * adds overviews.
 *
 * @threadsafe
 */
class GdalElevationProvider : public ElevationProvider
{
public:
  /**
   * Opens the raster at @p path. @p datumGeoid is the geoid the heights
   * are measured from (null for heights above the ellipsoid);
   * @p seaLevelGeoid, the global one, gives HeightTile::seaLevelM where
   * @p datumGeoid is null, and stands in where a regional @p datumGeoid
   * has no value (may be null too: sea level is then the ellipsoid). See
   * applyVerticalDatum().
   * @p datumId names the datum for id(). Check isValid().
   */
  // The file, then its datum's name.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  GdalElevationProvider(QString path, QString datumId,
                        std::shared_ptr<const GeoidModel> datumGeoid,
                        std::shared_ptr<const GeoidModel> seaLevelGeoid);
  ~GdalElevationProvider() override;

  GdalElevationProvider(const GdalElevationProvider&) = delete;
  GdalElevationProvider& operator=(const GdalElevationProvider&) = delete;
  GdalElevationProvider(GdalElevationProvider&&) = delete;
  GdalElevationProvider& operator=(GdalElevationProvider&&) = delete;

  /** Returns true if the file can be read. */
  bool isValid() const { return m_error.isEmpty(); }

  /** Returns why the provider isn't valid (translated), or empty. */
  const QString& errorString() const { return m_error; }

  /** The raster's extent, degrees (approximate for projected rasters). */
  const GeoRect& bounds() const { return m_bounds; }

  /** Samples along a tile's side. */
  static constexpr int samples = 65;

  QString id() const override;
  QString name() const override;
  /** The level whose sample spacing matches the raster's pixels. */
  int maximumLevel() const override { return m_maximumLevel; }

  std::optional<HeightTile>
  loadHeights(TileKey key, const std::atomic<bool>& cancelled) override;

private:
  struct Reader;
  /** A reader from the pool, or a newly opened one. */
  std::unique_ptr<Reader> takeReader();
  /** Puts @p reader back in the pool. */
  void giveBack(std::unique_ptr<Reader> reader);
  /** The raster's extent in degrees, if it can be had. */
  static std::optional<GeoRect> extentOf(const Reader& reader);
  /** Loads @p key with @p reader (loadHeights() without the pool). */
  std::optional<HeightTile> read(const Reader& reader, TileKey key,
                                 const std::atomic<bool>& cancelled) const;

  QString m_path;
  QString m_datumId;
  std::shared_ptr<const GeoidModel> m_datumGeoid;
  std::shared_ptr<const GeoidModel> m_seaLevelGeoid;
  QString m_error;
  GeoRect m_bounds;
  int m_maximumLevel = 0;
  /** True if the band has overviews. */
  bool m_hasOverviews = false;
  std::mutex m_mutex;
  /** Open datasets not in use; GDAL datasets are used by one thread at a time.
   */
  std::vector<std::unique_ptr<Reader>> m_readers;
};

} // namespace ts

#endif // TS_ELEVATION_GDALELEVATIONPROVIDER_H
