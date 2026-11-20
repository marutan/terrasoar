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
 * The window listing what the WMS layers have at a clicked point
 * (GetFeatureInfo).
 */
#ifndef TS_APP_FEATUREINFOWINDOW_H
#define TS_APP_FEATUREINFOWINDOW_H

#include <cstdint>
#include <memory>
#include <vector>

#include <QDialog>
#include <QString>
#include <QUrl>

#include "ts/regions/regiongeometry.h"

class QEvent;
class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/** One layer to ask at the clicked point. */
struct FeatureInfoQuery {
  /** The layer's name in the Layers panel. */
  QString layerName;
  /** The GetFeatureInfo request (getFeatureInfoUrl()). */
  QUrl url;
  /** The INFO_FORMAT asked for (used when the answer doesn't say). */
  QString format;
};

/**
 * Shows the features the WMS layers have where the user clicked: one
 * entry per layer, topmost first, with its features and their attributes
 * (or the server's text), "Nothing here", or what went wrong. A new click
 * replaces the last one's requests and results. The requests go through
 * @p network without blocking the GUI; a timer polls them while they run.
 * Nothing in an answer is fetched or run: HTML answers are shown as
 * their text.
 *
 * See the user guide's section on it (docs/user/adding-wms-layers.md).
 *
 * @guithread
 */
class FeatureInfoWindow : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(FeatureInfoWindow)

public:
  /** The user guide's section on this window, opened by F1. */
  static QString helpTarget();

  /** Creates the window; requests go through @p network. */
  FeatureInfoWindow(std::shared_ptr<NetworkClient> network, QWidget* parent);

  /** Cancels the requests still running. */
  ~FeatureInfoWindow() override;

  /** Asks each of @p queries about @p point, replacing earlier results. */
  void query(LonLat point, const std::vector<FeatureInfoQuery>& queries);

  /** Returns how many answers are still awaited. */
  int32_t pendingCount() const;

  /**
   * Returns the results as text, one line per row ("layer", "  feature",
   * "    name: value"), for tests.
   */
  QString resultsText() const;

signals:
  /** Every answer of the last query has arrived (or failed). */
  void finished();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  struct Pending {
    std::shared_ptr<NetworkTicket> ticket;
    QTreeWidgetItem* item = nullptr;
    QString format;
  };

  void retranslateUi();
  void poll();
  void answered(const Pending& pending);

  std::shared_ptr<NetworkClient> m_network;
  QLabel* m_placeLabel = nullptr;
  QTreeWidget* m_tree = nullptr;
  QTimer* m_pollTimer = nullptr;
  std::vector<Pending> m_pending;
  LonLat m_point;
  bool m_hasPoint = false;
};

} // namespace ts

#endif // TS_APP_FEATUREINFOWINDOW_H
