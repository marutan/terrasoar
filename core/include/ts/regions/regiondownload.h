// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Downloads the tiles of an offline region in the background.
 */
#ifndef TS_REGIONS_REGIONDOWNLOAD_H
#define TS_REGIONS_REGIONDOWNLOAD_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <QString>

#include "ts/regions/regiongeometry.h"
#include "ts/wms/wmscatalogue.h"

namespace ts
{

class WmsImageryProvider;

/** Behaviour of a RegionDownload. */
struct RegionDownloadSettings {
  /**
   * Download threads, so this many requests are in flight. Kept below
   * the network client's limit so the view's own tiles still get through,
   * and polite to the server.
   */
  int threads = 2;
  /**
   * After this many tiles in a row fail because the server can't be
   * reached, the download pauses itself (it can be resumed).
   */
  int unreachableBeforePause = 6;
};

/** Where a RegionDownload is, see RegionDownload::progress(). */
struct RegionDownloadProgress {
  /** The state of a download. */
  enum class State {
    Ready,     ///< Created, not started.
    Running,   ///< Downloading.
    Paused,    ///< Paused by the user, or by itself (see errorString).
    Finished,  ///< Every tile done (some may have failed).
    Cancelled, ///< Cancelled.
    Refused,   ///< Not started: the source's terms forbid it.
  };
  /** The state. */
  State state = State::Ready;
  /** Tiles in the region. */
  long long totalTiles = 0;
  /** Tiles done so far (whatever became of them). */
  long long doneTiles = 0;
  /** Tiles fetched or revalidated. */
  long long downloadedTiles = 0;
  /** Tiles already stored and fresh. */
  long long cachedTiles = 0;
  /** Tiles the source has no data for. */
  long long noDataTiles = 0;
  /** Tiles that failed. */
  long long failedTiles = 0;
  /** Bytes of the region's tiles stored so far. */
  long long bytes = 0;
  /** The last failure (translated); why it paused itself, or refused. */
  QString errorString;

  /** Returns doneTiles / totalTiles (1 for an empty region). */
  double fraction() const
  {
    return totalTiles <= 0 ? 1.0
                           : static_cast<double>(doneTiles) /
                                 static_cast<double>(totalTiles);
  }

  /** Returns true for Finished, Cancelled and Refused. */
  bool isOver() const
  {
    return state == State::Finished || state == State::Cancelled ||
           state == State::Refused;
  }
};

/**
 * Downloads the tiles of an offline region into a WMS source's disk
 * cache and pins them there (WmsImageryProvider::fetchForRegion()).
 *
 * Tiles go level by level from the coarsest (RegionTileIterator) on a few
 * threads of its own, through the provider's network client. Tiles
 * already stored and fresh are only pinned, so starting a download again
 * (to resume after the program was closed, or to refresh a region) only
 * fetches what is missing or stale. pause() lets the requests on the wire
 * finish and holds the rest; when the server can't be reached for a while
 * the download pauses itself, and the tiles that failed that way are
 * tried again on resume(). cancel() drops the requests on the wire; what
 * was stored stays pinned until the caller unpins the region
 * (TileDiskCache::unpinRegion()).
 *
 * A source whose catalogue entry forbids bulk downloads is refused
 * (bulkDownloadPolicy()).
 *
 * See docs/design/offline-regions.md.
 *
 * @threadsafe
 */
class RegionDownload
{
public:
  /**
   * Prepares to download the tiles of @p geometry at levels
   * @p minimumLevel to @p maximumLevel of @p provider's source for the
   * region @p region (an id, not 0). Counts the tiles at once.
   */
  RegionDownload(std::shared_ptr<WmsImageryProvider> provider, long long region,
                 const RegionGeometry& geometry, int minimumLevel,
                 int maximumLevel, RegionDownloadSettings settings = {});

  /** Cancels the download and waits for its threads. */
  ~RegionDownload();

  RegionDownload(const RegionDownload&) = delete;
  RegionDownload& operator=(const RegionDownload&) = delete;
  RegionDownload(RegionDownload&&) = delete;
  RegionDownload& operator=(RegionDownload&&) = delete;

  /**
   * Starts downloading, unless the source's entry in @p catalogue forbids
   * bulk downloads: then the state becomes Refused and it returns false
   * with @p errorString (translated). Does nothing if already started.
   */
  bool start(const std::vector<WmsCatalogueEntry>& catalogue,
             QString* errorString);

  /** Holds the tiles not yet requested; those on the wire finish. */
  void pause();

  /** Carries on after pause() or a pause of its own. */
  void resume();

  /** Stops: requests on the wire are dropped. Final. */
  void cancel();

  /** Returns where it is. */
  RegionDownloadProgress progress() const;

  /** Returns the region id. */
  long long region() const { return m_region; }

  /** Returns the provider. */
  const std::shared_ptr<WmsImageryProvider>& provider() const
  {
    return m_provider;
  }

  /**
   * Waits until the download is over or paused, at most @p timeout.
   * Returns true if it is (for tests).
   */
  bool waitUntilSettled(std::chrono::milliseconds timeout) const;

private:
  void work();
  bool takeTile(std::unique_lock<std::mutex>& lock, TileKey& key,
                int& attempts);

  const std::shared_ptr<WmsImageryProvider> m_provider;
  const long long m_region;
  const RegionDownloadSettings m_settings;
  RegionTileIterator m_tiles;
  std::atomic<bool> m_cancelled{false};
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_changed;
  RegionDownloadProgress m_progress;
  /** Tiles to try again (unreachable server), with attempts so far. */
  std::deque<std::pair<TileKey, int>> m_retry;
  int m_unreachableInARow = 0;
  int m_working = 0;
  bool m_exhausted = false;
  std::vector<std::thread> m_threads;
};

/** How big a region will be, see estimateRegionSize(). */
struct RegionEstimate {
  /** Tiles in the region. */
  long long tiles = 0;
  /** Bytes on disk, estimated. */
  long long bytes = 0;
  /** Sample tiles the estimate is based on (0: a guess from the format). */
  int sampledTiles = 0;
};

/**
 * Returns a guess of the size of one tile of @p provider's source, from
 * its image format and tile size alone (before any samples).
 *
 * @reentrant
 */
long long typicalTileBytes(const WmsImageryProvider& provider);

/**
 * Estimates the size of a region: counts its tiles exactly, then fetches
 * up to @p samples of them at random (sampleRegionTiles(), seeded with
 * @p seed) on a few threads, and multiplies their mean size by the count.
 * The samples stay in the disk cache (not pinned), so they aren't fetched
 * again by the download. Without any sample (no network, or cancelled
 * through @p cancelled) the size is typicalTileBytes() per tile. Blocks;
 * call it off the GUI thread.
 *
 * @threadsafe
 */
// The levels come in the same order as everywhere (countRegionTiles()).
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
RegionEstimate
estimateRegionSize(WmsImageryProvider& provider, const RegionGeometry& geometry,
                   int minimumLevel, int maximumLevel, int samples,
                   const std::atomic<bool>& cancelled, std::uint64_t seed = 1);
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace ts

#endif // TS_REGIONS_REGIONDOWNLOAD_H
