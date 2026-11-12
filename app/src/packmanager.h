// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The application's extra data packs: the online index, downloads,
 * installs and removals.
 */
#ifndef TS_APP_PACKMANAGER_H
#define TS_APP_PACKMANAGER_H

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "ts/packs/packindex.h"
#include "ts/packs/packinstall.h"
#include "ts/packs/packlibrary.h"

namespace ts
{

class NetworkTicket;
class ProviderRegistry;

/**
 * Keeps the extra packs (a PackLibrary), tells the ProviderRegistry which
 * are installed, reads the online index on request and runs installs
 * (PackInstall) in the background, whatever window is open.
 *
 * A finished install is activated at once: the registry forgets the old
 * version, layersChanged() lets the window refresh its layers (so its
 * files are closed), the new version is swapped in and the registry
 * told. Installs stop when the program closes; their downloads stay in
 * the staging area and resume() carries on.
 *
 * See docs/design/packs.md.
 *
 * @guithread
 */
class PackManager : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(PackManager)

public:
  /** Where the index stands. */
  enum class IndexState {
    NotLoaded, ///< Not asked for yet.
    Loading,   ///< On its way.
    Loaded,    ///< Read (see index()).
    Failed,    ///< Could not be read (see indexError()).
  };

  /**
   * Uses the packs in @p root, tidies them (PackLibrary::tidy()) and
   * tells @p registry which are installed.
   * @param registry Gets the installed packs, and lends its network
   *        client; must outlive this.
   * @param root The packs directory.
   * @param parent Usual QObject parent.
   */
  PackManager(ProviderRegistry& registry, const QString& root,
              QObject* parent = nullptr);

  /** Stops the installs (their downloads stay, to resume later). */
  ~PackManager() override;

  /** Sets the index to read (default defaultPackIndexUrl()); for tests. */
  void setIndexUrl(const QUrl& url) { m_indexUrl = url; }

  /** Allows http to loopback addresses; for tests. */
  void setAllowLocalHttp(bool allow) { m_allowLocalHttp = allow; }

  /** Starts reading the index (again). */
  void refreshIndex();

  /** Returns where the index stands. */
  IndexState indexState() const { return m_indexState; }

  /** Returns why the index couldn't be read (translated). */
  const QString& indexError() const { return m_indexError; }

  /** Returns the index, once loaded. */
  const std::optional<PackIndex>& index() const { return m_index; }

  /** Returns the installed packs, sorted by id. */
  const std::vector<InstalledPack>& installed() const { return m_installed; }

  /** Returns the installed pack @p id, or null. */
  const InstalledPack* installedPack(const QString& id) const;

  /** Returns the download of pack @p id in the staging area, if any. */
  std::optional<PendingPack> pending(const QString& id) const;

  /**
   * Returns every pack id known: in the index, installed or being
   * downloaded; in the index's order, then the others sorted.
   */
  std::vector<QString> packIds() const;

  /**
   * Installs or updates pack @p id from the index, or carries on with its
   * download. Returns false and @p errorString (translated) if it can't
   * start.
   */
  bool install(const QString& id, QString* errorString);

  /** Pauses the install of pack @p id; its download stays. */
  void pause(const QString& id);

  /** Cancels the install of pack @p id and deletes its download. */
  void cancel(const QString& id);

  /** Removes the installed pack @p id (and cancels its download). */
  void remove(const QString& id);

  /** Returns the install's progress, while there is one. */
  std::optional<PackInstallProgress> progress(const QString& id) const;

  /** Returns a line for the status bar about installs running, or "". */
  QString statusText() const;

signals:
  /** Packs were installed, removed, or the index or downloads changed. */
  void packsChanged();

  /** Install progress changed (polled a few times a second). */
  void progressChanged();

  /**
   * The registry's packs changed: refresh the layers now (connected
   * directly, so files of a pack about to be replaced get closed).
   */
  void layersChanged();

private:
  void poll();
  void pollIndex();
  void activate(const QString& id);
  void reload();

  ProviderRegistry& m_registry;
  PackLibrary m_library;
  QUrl m_indexUrl;
  bool m_allowLocalHttp = false;
  IndexState m_indexState = IndexState::NotLoaded;
  QString m_indexError;
  std::optional<PackIndex> m_index;
  std::shared_ptr<NetworkTicket> m_indexTicket;
  std::vector<InstalledPack> m_installed;
  std::vector<PendingPack> m_pending;
  std::map<QString, std::unique_ptr<PackInstall>> m_installs;
  /** Each install's state at the last poll. */
  std::map<QString, PackInstallProgress::State> m_states;
  QTimer m_poll;
};

} // namespace ts

#endif // TS_APP_PACKMANAGER_H
