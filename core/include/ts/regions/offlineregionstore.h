// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The list of offline regions, kept in a file next to the tile caches.
 */
#ifndef TS_REGIONS_OFFLINEREGIONSTORE_H
#define TS_REGIONS_OFFLINEREGIONSTORE_H

#include <optional>
#include <vector>

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include "ts/regions/regiongeometry.h"

namespace ts
{

/** An area of one source kept for offline use. */
struct OfflineRegion {
  /** Unique among the regions of a store, never 0 (see TileDiskCache pins). */
  long long id = 0;
  /** Name for the user. */
  QString name;
  /** The layer source whose tiles it keeps (`{"kind": "wms", …}`). */
  QJsonObject source;
  /** Name of the source for the user (the layer's title). */
  QString sourceName;
  /** The area. */
  RegionGeometry geometry;
  /** Coarsest level kept. */
  int minimumLevel = 0;
  /** Deepest level kept. */
  int maximumLevel = 0;
  /** When it was created (UTC). */
  QDateTime created;
  /** When its download last finished (UTC); null if it never did. */
  QDateTime downloaded;
  /** True once a download went through every tile. */
  bool complete = false;
  /** Tiles in the region (when last counted). */
  long long tiles = 0;
  /** Tiles that failed in the last download. */
  long long failedTiles = 0;
  /** Bytes stored (when last measured). */
  long long bytes = 0;

  /** Returns the object stored in the file. */
  QJsonObject toJson() const;
  /** Reads toJson()'s object, or returns nothing. */
  static std::optional<OfflineRegion> fromJson(const QJsonObject& json);
};

/** Format name of region files. */
inline constexpr const char* offlineRegionsFormatName =
    "terrasoar-offline-regions";

/** Version of the region file format this build writes. */
inline constexpr int offlineRegionsFormatVersion = 1;

/**
 * The offline regions, in a JSON file (normally `regions.json` in the
 * directory of the WMS tile caches):
 *
 *     {"format": "terrasoar-offline-regions", "version": 1, "nextId": 3,
 *      "regions": [{"id": 1, "name": …, "source": {…},
 *                   "geometry": {…}, "minLevel": 0, "maxLevel": 12, …}]}
 *
 * The tiles themselves are in the sources' disk caches, pinned with the
 * region's id. A missing file is an empty list; regions that can't be
 * read are skipped. Saved atomically (QSaveFile).
 *
 * @reentrant
 */
class OfflineRegionStore
{
public:
  /** Uses the file at @p path (not read until load()). */
  explicit OfflineRegionStore(QString path);

  /** Returns the file's path. */
  const QString& path() const { return m_path; }

  /**
   * Reads the file. Returns false and @p errorString (translated) if it
   * exists but isn't a region file or is of a newer version; the list is
   * then empty.
   */
  bool load(QString* errorString);

  /** Writes the file. Returns false and @p errorString on failure. */
  bool save(QString* errorString) const;

  /** Returns the regions, oldest first. */
  const std::vector<OfflineRegion>& regions() const { return m_regions; }

  /** Adds @p region with a new id and returns that id. */
  long long add(OfflineRegion region);

  /** Returns the region @p id, or null. */
  OfflineRegion* find(long long id);

  /** Returns the region @p id, or null. */
  const OfflineRegion* find(long long id) const;

  /** Removes the region @p id. Returns false if there is none. */
  bool remove(long long id);

private:
  QString m_path;
  std::vector<OfflineRegion> m_regions;
  long long m_nextId = 1;
};

} // namespace ts

#endif // TS_REGIONS_OFFLINEREGIONSTORE_H
