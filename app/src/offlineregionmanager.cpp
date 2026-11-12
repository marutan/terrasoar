// SPDX-License-Identifier: GPL-2.0-or-later
#include "offlineregionmanager.h"

#include <cmath>
#include <utility>

#include <QDateTime>
#include <QStringList>

#include "ts/network/tilediskcache.h"
#include "ts/providers/providerregistry.h"
#include "ts/wms/wmsimageryprovider.h"
#include "wmscataloguefile.h"

namespace ts
{

namespace
{

/** How often running downloads are looked at, ms. */
constexpr int pollIntervalMs = 250;

} // namespace

OfflineRegionManager::OfflineRegionManager(ProviderRegistry& registry,
                                           const QString& storePath,
                                           QObject* parent)
    : QObject(parent), m_registry(registry), m_store(storePath),
      m_catalogue(&currentWmsCatalogue)
{
  if (!m_store.load(&m_loadError)) {
    qWarning("Offline regions %s: %s", qUtf8Printable(storePath),
             qUtf8Printable(m_loadError));
  }
  m_poll.setInterval(pollIntervalMs);
  connect(&m_poll, &QTimer::timeout, this, &OfflineRegionManager::poll);
}

OfflineRegionManager::~OfflineRegionManager()
{
  // Cancel them all before waiting for any.
  for (const std::pair<const long long, std::unique_ptr<RegionDownload>>&
           download : m_downloads) {
    download.second->cancel();
  }
  m_downloads.clear();
}

const OfflineRegion* OfflineRegionManager::region(long long id) const
{
  return m_store.find(id);
}

void OfflineRegionManager::setCatalogue(
    std::function<std::vector<WmsCatalogueEntry>()> catalogue)
{
  m_catalogue = std::move(catalogue);
}

std::vector<WmsCatalogueEntry> OfflineRegionManager::catalogue() const
{
  return m_catalogue ? m_catalogue() : std::vector<WmsCatalogueEntry>{};
}

std::shared_ptr<WmsImageryProvider>
OfflineRegionManager::providerFor(const QJsonObject& source,
                                  QString* errorString)
{
  QString error;
  std::shared_ptr<WmsImageryProvider> provider =
      std::dynamic_pointer_cast<WmsImageryProvider>(
          m_registry.imagery(source, &error));
  if (!provider && errorString) {
    *errorString = error.isEmpty()
                       ? tr("Only layers from WMS servers can be kept for "
                            "offline use.")
                       : error;
  }
  if (provider && !provider->diskCache() && errorString) {
    *errorString = tr("There is no disk cache to keep tiles in.");
    return nullptr;
  }
  return provider;
}

bool OfflineRegionManager::startDownload(OfflineRegion& region,
                                         QString* errorString)
{
  std::shared_ptr<WmsImageryProvider> provider =
      providerFor(region.source, errorString);
  if (!provider) {
    return false;
  }
  std::unique_ptr<RegionDownload> download = std::make_unique<RegionDownload>(
      provider, region.id, region.geometry, region.minimumLevel,
      region.maximumLevel);
  if (!download->start(catalogue(), errorString)) {
    return false;
  }
  region.tiles = download->progress().totalTiles;
  m_downloads[region.id] = std::move(download);
  if (!m_poll.isActive()) {
    m_poll.start();
  }
  return true;
}

long long OfflineRegionManager::addRegion(OfflineRegion region,
                                          QString* errorString)
{
  if (!region.created.isValid()) {
    region.created = QDateTime::currentDateTimeUtc();
  }
  const long long id = m_store.add(std::move(region));
  OfflineRegion* added = m_store.find(id);
  if (!startDownload(*added, errorString)) {
    m_store.remove(id);
    return 0;
  }
  save();
  emit regionsChanged();
  return id;
}

bool OfflineRegionManager::resume(long long id, QString* errorString)
{
  OfflineRegion* region = m_store.find(id);
  if (!region) {
    return false;
  }
  const std::map<long long, std::unique_ptr<RegionDownload>>::iterator running =
      m_downloads.find(id);
  if (running != m_downloads.end()) {
    running->second->resume();
    emit progressChanged();
    return true;
  }
  if (!startDownload(*region, errorString)) {
    return false;
  }
  emit progressChanged();
  return true;
}

void OfflineRegionManager::pause(long long id)
{
  const std::map<long long, std::unique_ptr<RegionDownload>>::iterator running =
      m_downloads.find(id);
  if (running != m_downloads.end()) {
    running->second->pause();
    emit progressChanged();
  }
}

void OfflineRegionManager::remove(long long id)
{
  OfflineRegion* region = m_store.find(id);
  if (!region) {
    return;
  }
  m_downloads.erase(id); // cancels and waits
  QString error;
  std::shared_ptr<WmsImageryProvider> provider =
      providerFor(region->source, &error);
  if (provider) {
    provider->diskCache()->unpinRegion(id);
  }
  m_store.remove(id);
  save();
  emit regionsChanged();
}

std::optional<RegionDownloadProgress>
OfflineRegionManager::progress(long long id) const
{
  const std::map<long long, std::unique_ptr<RegionDownload>>::const_iterator
      running = m_downloads.find(id);
  if (running == m_downloads.end()) {
    return std::nullopt;
  }
  return running->second->progress();
}

QString OfflineRegionManager::statusText() const
{
  QStringList parts;
  for (const std::pair<const long long, std::unique_ptr<RegionDownload>>&
           download : m_downloads) {
    const RegionDownloadProgress progress = download.second->progress();
    if (progress.state != RegionDownloadProgress::State::Running) {
      continue;
    }
    const OfflineRegion* region = m_store.find(download.first);
    parts.append(
        tr("Downloading “%1”: %2 %")
            .arg(region ? region->name : QString())
            .arg(static_cast<int>(std::floor(progress.fraction() * 100.0))));
  }
  return parts.join(QStringLiteral(" · "));
}

void OfflineRegionManager::poll()
{
  bool finished = false;
  std::map<long long, std::unique_ptr<RegionDownload>>::iterator download =
      m_downloads.begin();
  while (download != m_downloads.end()) {
    const RegionDownloadProgress progress = download->second->progress();
    OfflineRegion* region = m_store.find(download->first);
    if (!progress.isOver() || !region) {
      ++download;
      continue;
    }
    // Finished (or cancelled): record what it holds.
    region->tiles = progress.totalTiles;
    region->failedTiles = progress.failedTiles;
    region->complete =
        progress.state == RegionDownloadProgress::State::Finished;
    if (region->complete) {
      region->downloaded = QDateTime::currentDateTimeUtc();
    }
    TileDiskCache* cache = download->second->provider()->diskCache();
    region->bytes = cache ? cache->regionUsage(region->id).bytes : 0;
    download = m_downloads.erase(download);
    finished = true;
  }
  if (finished) {
    save();
    emit regionsChanged();
  }
  if (m_downloads.empty()) {
    m_poll.stop();
  }
  emit progressChanged();
}

void OfflineRegionManager::save()
{
  QString error;
  if (!m_store.save(&error)) {
    qWarning("Offline regions %s: %s", qUtf8Printable(m_store.path()),
             qUtf8Printable(error));
  }
}

} // namespace ts
