// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Downloads and checks the files of a data pack in the background.
 */
#ifndef TS_PACKS_PACKINSTALL_H
#define TS_PACKS_PACKINSTALL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <QString>

#include "ts/packs/packindex.h"
#include "ts/packs/packmanifest.h"

namespace ts
{

class NetworkClient;
struct HttpResponse;

/** Behaviour of a PackInstall. */
struct PackInstallSettings {
  /**
   * Bytes asked for per request. Must stay below the network client's
   * response limit (NetworkSettings::maximumResponseBytes).
   */
  qint64 chunkBytes = qint64{8} * 1024 * 1024;
  /** Allows http to loopback addresses (tests), see isAllowedPackUrl(). */
  bool allowLocalHttp = false;
};

/** Where a PackInstall is, see PackInstall::progress(). */
struct PackInstallProgress {
  /** The state of an install. */
  enum class State {
    Ready,     ///< Created, not started.
    Running,   ///< Downloading or checking.
    Paused,    ///< Paused by the user, or by a network failure (errorString).
    Verified,  ///< Every file downloaded and checked: ready to activate.
    Failed,    ///< A file is damaged or can't be had (errorString).
    Cancelled, ///< Cancelled.
  };
  /** The state. */
  State state = State::Ready;
  /** Bytes of the pack (0 until its manifest is read). */
  qint64 totalBytes = 0;
  /** Bytes of it on disk so far. */
  qint64 doneBytes = 0;
  /** Why it paused itself or failed (translated). */
  QString errorString;

  /** Returns doneBytes / totalBytes (0 while the total is unknown). */
  double fraction() const
  {
    return totalBytes <= 0 ? 0.0
                           : static_cast<double>(doneBytes) /
                                 static_cast<double>(totalBytes);
  }

  /** Returns true for Verified, Failed and Cancelled. */
  bool isOver() const
  {
    return state == State::Verified || state == State::Failed ||
           state == State::Cancelled;
  }
};

/**
 * Downloads the pack an index entry describes into its staging directory
 * (PackLibrary::prepareStaging()) and checks every file.
 *
 * The manifest comes first and must match the index's SHA-256. Then each
 * file the manifest lists is fetched in chunks with Range requests,
 * appended to `<path>.part`, checked against the manifest's SHA-256 and
 * renamed into place. Files already in place, and the bytes of part files,
 * are kept: a new PackInstall on the same directory continues where the
 * last one stopped. A damaged file is deleted and the install fails; a
 * network failure (after the client's own retries) pauses it.
 *
 * Runs on one thread of its own. pause() and cancel() drop the request on
 * the wire; neither deletes anything (PackLibrary::discardStaging() does,
 * after a cancel).
 *
 * See docs/design/packs.md.
 *
 * @threadsafe
 */
class PackInstall
{
public:
  /** Prepares to download @p entry into @p stagingDirectory. */
  PackInstall(std::shared_ptr<NetworkClient> network, PackIndexEntry entry,
              QString stagingDirectory, PackInstallSettings settings = {});

  /** Stops, like pause() (the files stay), and waits for the thread. */
  ~PackInstall();

  PackInstall(const PackInstall&) = delete;
  PackInstall& operator=(const PackInstall&) = delete;
  PackInstall(PackInstall&&) = delete;
  PackInstall& operator=(PackInstall&&) = delete;

  /** Starts. Does nothing if already started. */
  void start();

  /** Stops; the files stay for a later install to continue. Final. */
  void pause();

  /** Stops. Final; the caller deletes the staging directory. */
  void cancel();

  /** Returns where it is. */
  PackInstallProgress progress() const;

  /** Returns the index entry. */
  const PackIndexEntry& entry() const { return m_entry; }

  /** Returns the manifest, once read. */
  std::optional<PackManifest> manifest() const;

  /**
   * Waits until it is over or paused, at most @p timeout. Returns true if
   * it is (for tests).
   */
  bool waitUntilSettled(std::chrono::milliseconds timeout) const;

private:
  /** What one answer to a Range request gives. */
  enum class Chunk {
    Append,   ///< The next bytes of the file.
    Restart,  ///< The whole file: replaces what there is.
    Complete, ///< Nothing more (416): check what there is.
    Stop,     ///< Stopped, or settled with an error.
  };

  void work();
  bool fetchManifest();
  /** Downloads the manifest to @p path and into @p json; checks it. */
  bool downloadManifest(const QString& path, QByteArray* json);
  bool fetchFile(const PackFile& file);
  /** Takes @p response to the request for @p file from byte @p have. */
  Chunk takeChunk(const PackFile& file, const HttpResponse& response,
                  qint64 have, QByteArray* body);
  void stop(PackInstallProgress::State state);
  void settle(PackInstallProgress::State state, const QString& error);
  void addDone(qint64 bytes);

  const std::shared_ptr<NetworkClient> m_network;
  const PackIndexEntry m_entry;
  const QString m_directory;
  const PackInstallSettings m_settings;
  std::atomic<bool> m_stop{false};
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_changed;
  PackInstallProgress m_progress;
  std::optional<PackManifest> m_manifest;
  std::thread m_thread;
};

} // namespace ts

#endif // TS_PACKS_PACKINSTALL_H
