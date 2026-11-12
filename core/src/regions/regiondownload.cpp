// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/regions/regiondownload.h"

#include <algorithm>
#include <random>

#include <QCoreApplication>

#include "ts/wms/wmsimageryprovider.h"

namespace ts
{

namespace
{

/** Attempts per tile when the server can't be reached. */
constexpr int unreachableAttempts = 2;

} // namespace

RegionDownload::RegionDownload(std::shared_ptr<WmsImageryProvider> provider,
                               long long region, const RegionGeometry& geometry,
                               int minimumLevel, int maximumLevel,
                               RegionDownloadSettings settings)
    : m_provider(std::move(provider)), m_region(region), m_settings(settings),
      m_tiles(geometry, minimumLevel, maximumLevel, m_provider->dataBounds())
{
  m_progress.totalTiles = countRegionTiles(geometry, minimumLevel, maximumLevel,
                                           m_provider->dataBounds())
                              .total();
}

RegionDownload::~RegionDownload()
{
  cancel();
  for (std::thread& thread : m_threads) {
    thread.join();
  }
}

bool RegionDownload::start(const std::vector<WmsCatalogueEntry>& catalogue,
                           QString* errorString)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (m_progress.state != RegionDownloadProgress::State::Ready) {
    return m_progress.state != RegionDownloadProgress::State::Refused;
  }
  const QUrl url = m_provider->source().url;
  if (bulkDownloadPolicy(url, catalogue) == BulkDownloadPolicy::Forbidden) {
    const WmsCatalogueEntry* entry = findWmsCatalogueEntry(url, catalogue);
    m_progress.state = RegionDownloadProgress::State::Refused;
    m_progress.errorString =
        QCoreApplication::translate(
            "ts::RegionDownload",
            "The terms of use of %1 don't allow downloading regions for "
            "offline use.")
            .arg(entry && !entry->title.isEmpty() ? entry->title : url.host());
    if (errorString) {
      *errorString = m_progress.errorString;
    }
    m_changed.notify_all();
    return false;
  }
  m_progress.state = RegionDownloadProgress::State::Running;
  const int threads = std::max(1, m_settings.threads);
  for (int i = 0; i < threads; ++i) {
    m_threads.emplace_back([this]() { work(); });
  }
  return true;
}

void RegionDownload::pause()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (m_progress.state == RegionDownloadProgress::State::Running) {
    m_progress.state = RegionDownloadProgress::State::Paused;
    m_progress.errorString.clear();
    m_changed.notify_all();
  }
}

void RegionDownload::resume()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (m_progress.state == RegionDownloadProgress::State::Paused) {
    m_progress.state = RegionDownloadProgress::State::Running;
    m_progress.errorString.clear();
    m_unreachableInARow = 0;
    m_changed.notify_all();
  }
}

void RegionDownload::cancel()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_cancelled.store(true);
  if (!m_progress.isOver()) {
    m_progress.state = RegionDownloadProgress::State::Cancelled;
  }
  m_changed.notify_all();
}

RegionDownloadProgress RegionDownload::progress() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_progress;
}

bool RegionDownload::waitUntilSettled(std::chrono::milliseconds timeout) const
{
  std::unique_lock<std::mutex> lock(m_mutex);
  return m_changed.wait_for(lock, timeout, [this]() {
    return m_progress.isOver() ||
           (m_progress.state == RegionDownloadProgress::State::Paused &&
            m_working == 0);
  });
}

bool RegionDownload::takeTile(std::unique_lock<std::mutex>& lock, TileKey& key,
                              int& attempts)
{
  for (;;) {
    if (m_cancelled.load() || m_progress.isOver()) {
      return false;
    }
    if (m_progress.state == RegionDownloadProgress::State::Paused) {
      m_changed.wait(lock);
      continue;
    }
    if (!m_retry.empty()) {
      key = m_retry.front().first;
      attempts = m_retry.front().second;
      m_retry.pop_front();
      return true;
    }
    if (!m_exhausted) {
      const std::optional<TileKey> next = m_tiles.next();
      if (next) {
        key = *next;
        attempts = 0;
        return true;
      }
      m_exhausted = true;
    }
    if (m_working > 0) {
      // Another thread may yet hand a tile back to try again.
      m_changed.wait(lock);
      continue;
    }
    m_progress.state = RegionDownloadProgress::State::Finished;
    m_changed.notify_all();
    return false;
  }
}

void RegionDownload::work()
{
  using Outcome = RegionTileResult::Outcome;
  std::unique_lock<std::mutex> lock(m_mutex);
  TileKey key;
  int attempts = 0;
  while (takeTile(lock, key, attempts)) {
    ++m_working;
    lock.unlock();
    const RegionTileResult result =
        m_provider->fetchForRegion(key, m_region, m_cancelled);
    lock.lock();
    --m_working;
    RegionDownloadProgress& progress = m_progress;
    switch (result.outcome) {
    case Outcome::Downloaded:
    case Outcome::Revalidated:
      ++progress.downloadedTiles;
      progress.bytes += result.bytes;
      m_unreachableInARow = 0;
      break;
    case Outcome::AlreadyCached:
      ++progress.cachedTiles;
      progress.bytes += result.bytes;
      break;
    case Outcome::NoData:
      ++progress.noDataTiles;
      break;
    case Outcome::Cancelled:
      break;
    case Outcome::Failed:
      progress.errorString = result.errorString;
      if (result.unreachable) {
        ++m_unreachableInARow;
        if (attempts + 1 < unreachableAttempts) {
          m_retry.emplace_back(key, attempts + 1);
        } else {
          ++progress.failedTiles;
        }
        if (m_unreachableInARow >= m_settings.unreachableBeforePause &&
            progress.state == RegionDownloadProgress::State::Running) {
          progress.state = RegionDownloadProgress::State::Paused;
        }
      } else {
        ++progress.failedTiles;
        m_unreachableInARow = 0;
      }
      break;
    }
    // A tile handed back to try again isn't done yet.
    const bool triedAgain = result.outcome == Outcome::Failed &&
                            result.unreachable &&
                            attempts + 1 < unreachableAttempts;
    if (result.outcome != Outcome::Cancelled && !triedAgain) {
      ++progress.doneTiles;
    }
    m_changed.notify_all();
  }
  m_changed.notify_all();
}

long long typicalTileBytes(const WmsImageryProvider& provider)
{
  // Rough sizes of 256-pixel map tiles: JPEG photographs about 20 KB,
  // PNG maps about 25 KB; scaled by area for other tile sizes.
  const WmsSource& source = provider.source();
  const double perPixel =
      source.format.contains(QLatin1String("jpeg"), Qt::CaseInsensitive)
          ? 20000.0 / 65536.0
          : 25000.0 / 65536.0;
  const double pixels =
      static_cast<double>(source.tileSizePx) * source.tileSizePx;
  return static_cast<long long>(perPixel * pixels);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): as in the header.
RegionEstimate
estimateRegionSize(WmsImageryProvider& provider, const RegionGeometry& geometry,
                   int minimumLevel, int maximumLevel, int samples,
                   const std::atomic<bool>& cancelled, std::uint64_t seed)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  RegionEstimate estimate;
  const std::optional<GeoRect> dataBounds = provider.dataBounds();
  const RegionTileCount counts =
      countRegionTiles(geometry, minimumLevel, maximumLevel, dataBounds);
  estimate.tiles = counts.total();
  std::mt19937_64 random(seed);
  const std::vector<TileKey> keys =
      sampleRegionTiles(geometry, counts, dataBounds, samples, random);

  std::mutex mutex;
  std::size_t nextKey = 0;
  long long sampledBytes = 0;
  int sampled = 0;
  std::vector<std::thread> threads;
  const int threadCount = std::min<int>(4, static_cast<int>(keys.size()));
  threads.reserve(static_cast<std::size_t>(threadCount));
  for (int i = 0; i < threadCount; ++i) {
    threads.emplace_back([&]() {
      for (;;) {
        TileKey key;
        {
          const std::lock_guard<std::mutex> lock(mutex);
          if (nextKey >= keys.size() || cancelled.load()) {
            return;
          }
          key = keys[nextKey++];
        }
        const RegionTileResult result =
            provider.fetchForRegion(key, 0, cancelled);
        const bool stored =
            result.outcome == RegionTileResult::Outcome::Downloaded ||
            result.outcome == RegionTileResult::Outcome::Revalidated ||
            result.outcome == RegionTileResult::Outcome::AlreadyCached;
        if (stored) {
          const std::lock_guard<std::mutex> lock(mutex);
          sampledBytes += result.bytes;
          ++sampled;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  estimate.sampledTiles = sampled;
  const double perTile = sampled > 0
                             ? static_cast<double>(sampledBytes) / sampled
                             : static_cast<double>(typicalTileBytes(provider));
  estimate.bytes =
      static_cast<long long>(perTile * static_cast<double>(estimate.tiles));
  return estimate;
}

} // namespace ts
