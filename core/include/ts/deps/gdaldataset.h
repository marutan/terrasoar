// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Owning handle for a GDAL dataset.
 */
#ifndef TS_DEPS_GDALDATASET_H
#define TS_DEPS_GDALDATASET_H

#include <QString>

namespace ts
{

/**
 * Owns one GDAL dataset handle (raster or vector), closing it on
 * destruction.
 *
 * GDAL dataset handles must not be shared between threads, so each thread
 * opens its own GdalDataset for the same file.
 *
 * @reentrant
 */
class GdalDataset
{
public:
  /** Creates a handle with no open dataset. */
  GdalDataset() = default;

  /**
   * Opens @p path read-only.
   *
   * @param path File name or GDAL connection string, UTF-8 encoded on disk.
   * @see isOpen(), errorString()
   */
  explicit GdalDataset(const QString& path);

  /** Closes the dataset if one is open. */
  ~GdalDataset();

  GdalDataset(const GdalDataset&) = delete;
  GdalDataset& operator=(const GdalDataset&) = delete;

  /** Takes over the dataset of @p other, which is left closed. */
  GdalDataset(GdalDataset&& other) noexcept;

  /** Closes the current dataset and takes over that of @p other. */
  GdalDataset& operator=(GdalDataset&& other) noexcept;

  /** Returns true if a dataset is open. */
  bool isOpen() const { return m_handle != nullptr; }

  /** Returns the GDAL error message from the last failed open. */
  QString errorString() const { return m_error; }

  /** Returns the short name of the driver that opened it, e.g. "GeoJSON". */
  QString driverShortName() const;

  /** Returns the number of vector layers (0 for a pure raster). */
  int layerCount() const;

  /** Returns the number of raster bands (0 for a pure vector dataset). */
  int rasterBandCount() const;

  /**
   * Returns the number of features in vector layer @p layerIndex, or -1 if
   * the layer does not exist.
   */
  long long featureCount(int layerIndex) const;

  /** Returns the raw GDALDatasetH, or nullptr. Ownership stays here. */
  void* handle() const { return m_handle; }

private:
  void close();

  void* m_handle = nullptr;
  QString m_error;
};

} // namespace ts

#endif // TS_DEPS_GDALDATASET_H
