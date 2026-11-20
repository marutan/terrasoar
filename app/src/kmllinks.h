// SPDX-License-Identifier: GPL-2.0-or-later
/*
  Terra Soar - A virtual globe

  Copyright (C) 2026 Peter Howkins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
/**
 * @file
 * KML NetworkLinks: fetching what they link to, and again as their refresh
 * modes say.
 */
#ifndef TS_APP_KMLLINKS_H
#define TS_APP_KMLLINKS_H

#include <map>
#include <memory>

#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

#include "ts/kml/kmlactivity.h"
#include "ts/kml/kmldocument.h"

class QTimer;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * The NetworkLinks of the layer tree, by node id. Each is loaded when
 * load() is called (the main window does when it is added and its Region,
 * if any, is active): a local file at once, a web address in the
 * background. What arrives is written to a file of its own (web documents
 * in a temporary folder for the session, their relative addresses kept
 * pointing at the web) and loaded() gives its path. Afterwards each link
 * is fetched again every refreshInterval (onInterval), viewRefreshTime
 * after the view stops moving (viewRefreshMode onStop), or when asked
 * (Refresh).
 *
 * @guithread
 */
class KmlLinks : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(KmlLinks)

public:
  /** Fetches web links through @p network. */
  KmlLinks(std::shared_ptr<NetworkClient> network, QObject* parent);

  /** Cancels fetches still running. */
  ~KmlLinks() override;

  /**
   * Watches the NetworkLink at @p featurePath of @p document, shown at the
   * layer tree's node @p nodeId. Nothing is fetched until load().
   */
  void add(const QString& nodeId, std::shared_ptr<const KmlDocument> document,
           const QString& featurePath);

  /** Stops watching @p nodeId (its node is gone). */
  void remove(const QString& nodeId);

  /** Returns true if @p nodeId is a NetworkLink's node. */
  bool contains(const QString& nodeId) const;

  /** Returns true if @p nodeId has been loaded at least once. */
  bool isLoaded(const QString& nodeId) const;

  /** Returns the ids of the links' nodes. */
  QStringList nodeIds() const;

  /** Returns the link at @p nodeId (its document and path), or null. */
  const KmlFeature* link(const QString& nodeId) const;

  /** Returns the document holding the link at @p nodeId, or null. */
  std::shared_ptr<const KmlDocument> document(const QString& nodeId) const;

  /** Returns the feature path of the link at @p nodeId. */
  QString featurePath(const QString& nodeId) const;

  /** Returns the address @p nodeId last asked for. */
  QUrl lastUrl(const QString& nodeId) const;

  /** Fetches @p nodeId now, for @p view. */
  void load(const QString& nodeId, const KmlViewParameters& view);

  /**
   * The view has stopped at @p view: links that refresh then do, after
   * their viewRefreshTime.
   */
  void viewStopped(const KmlViewParameters& view);

  /** Returns how many fetches are running. */
  int32_t pending() const;

signals:
  /**
   * What @p nodeId links to is in the file at @p path; @p flyToView: fly to
   * its view.
   */
  void loaded(const QString& nodeId, const QString& path, bool flyToView);

  /** @p nodeId couldn't be fetched: @p error says why. */
  void failed(const QString& nodeId, const QString& error);

private:
  struct Link {
    std::shared_ptr<const KmlDocument> document;
    QString featurePath;
    const KmlFeature* feature = nullptr;
    std::shared_ptr<NetworkTicket> ticket;
    QUrl url;
    QTimer* interval = nullptr;
    QTimer* afterStop = nullptr;
    KmlViewParameters view;
    int32_t generation = 0;
    bool loaded = false;
  };

  void poll();
  void deliver(const QString& nodeId, Link& link, const QByteArray& body);

  std::shared_ptr<NetworkClient> m_network;
  QTemporaryDir m_folder;
  QTimer* m_poll = nullptr;
  std::map<QString, Link> m_links;
};

} // namespace ts

#endif // TS_APP_KMLLINKS_H
