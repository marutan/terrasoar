// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The application's offline regions and their downloads.
 */
#ifndef TS_APP_OFFLINEREGIONMANAGER_H
#define TS_APP_OFFLINEREGIONMANAGER_H

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>
#include <QString>
#include <QTimer>

#include "ts/regions/offlineregionstore.h"
#include "ts/regions/regiondownload.h"
#include "ts/wms/wmscatalogue.h"

namespace ts
{

class ProviderRegistry;
class WmsImageryProvider;

/**
 * Keeps the list of offline regions (an OfflineRegionStore) and runs
 * their downloads (RegionDownload) in the background, whatever window is
 * open. Each region's tiles are fetched through the provider the
 * ProviderRegistry hands out for its source, so a layer showing the same
 * source shares the disk cache with the download.
 *
 * Downloads stop when the program closes; a region that wasn't finished
 * stays in the list (incomplete) and resume() carries on, fetching only
 * what is missing. Deleting a region cancels its download and unpins its
 * tiles, which become ordinary cached tiles.
 *
 * @guithread
 */
class OfflineRegionManager : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(OfflineRegionManager)

public:
  /**
   * Uses the region list at @p storePath and @p registry's providers.
   * @param registry Hands out the regions' providers; must outlive this.
   * @param storePath The region list (OfflineRegionStore).
   * @param parent Usual QObject parent.
   */
  OfflineRegionManager(ProviderRegistry& registry, const QString& storePath,
                       QObject* parent = nullptr);

  /** Cancels the downloads (what was stored stays). */
  ~OfflineRegionManager() override;

  /** Returns the regions, oldest first. */
  const std::vector<OfflineRegion>& regions() const
  {
    return m_store.regions();
  }

  /** Returns the region @p id, or null. */
  const OfflineRegion* region(long long id) const;

  /** Returns why the list couldn't be read, or an empty string. */
  const QString& loadError() const { return m_loadError; }

  /**
   * Replaces the source of the catalogue used for the terms of use
   * (default: currentWmsCatalogue()); for tests.
   */
  void setCatalogue(std::function<std::vector<WmsCatalogueEntry>()> catalogue);

  /** Returns the catalogue (see setCatalogue()). */
  std::vector<WmsCatalogueEntry> catalogue() const;

  /**
   * Adds @p region (its id is assigned) and starts downloading it.
   * Returns the new id, or 0 and @p errorString if its source can't be
   * opened or its terms forbid it (then nothing is added).
   */
  long long addRegion(OfflineRegion region, QString* errorString);

  /**
   * Starts or carries on downloading the region @p id: after a pause, or
   * to finish or refresh it (only missing and stale tiles are fetched).
   * Returns false and @p errorString if it can't.
   */
  bool resume(long long id, QString* errorString);

  /** Pauses the download of the region @p id, if running. */
  void pause(long long id);

  /** Cancels the region's download, unpins its tiles and removes it. */
  void remove(long long id);

  /** Returns the region's download progress, if it has one now. */
  std::optional<RegionDownloadProgress> progress(long long id) const;

  /**
   * Returns a line for the status bar about the downloads running (empty
   * if none).
   */
  QString statusText() const;

  /** Returns the WMS provider for @p source, or null and @p errorString. */
  std::shared_ptr<WmsImageryProvider> providerFor(const QJsonObject& source,
                                                  QString* errorString);

signals:
  /** Regions were added, removed or finished downloading. */
  void regionsChanged();

  /** Download progress changed (polled a few times a second). */
  void progressChanged();

private:
  bool startDownload(OfflineRegion& region, QString* errorString);
  void poll();
  void save();

  ProviderRegistry& m_registry;
  OfflineRegionStore m_store;
  QString m_loadError;
  std::function<std::vector<WmsCatalogueEntry>()> m_catalogue;
  std::map<long long, std::unique_ptr<RegionDownload>> m_downloads;
  QTimer m_poll;
};

} // namespace ts

#endif // TS_APP_OFFLINEREGIONMANAGER_H
