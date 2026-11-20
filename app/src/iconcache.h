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
 * The icons KML placemarks use: from KMZ archives, local files or the web,
 * kept in memory and on disk.
 */
#ifndef TS_APP_ICONCACHE_H
#define TS_APP_ICONCACHE_H

#include <map>
#include <memory>

#include <QImage>
#include <QObject>
#include <QString>

class QTimer;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * Icons by address: local files and KMZ members are read at once; web
 * icons are fetched through @p network in the background (iconsChanged()
 * when one arrives) and kept in the cache directory, so they show again
 * offline. Until an icon is there, and for one that can't be had, the
 * default pushpin stands in.
 *
 * @guithread
 */
class IconCache : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(IconCache)

public:
  /** Fetches web icons through @p network. */
  IconCache(std::shared_ptr<NetworkClient> network, QObject* parent);

  /** Cancels the fetches still running. */
  ~IconCache() override;

  /** Returns the icon at @p where (the pushpin for empty or missing). */
  QImage icon(const QString& where);

  /** Returns the default icon: a yellow pushpin, 32 pixels. */
  static const QImage& pushpin();

  /** Returns how many web icons are still being fetched. */
  int32_t pending() const { return static_cast<int32_t>(m_tickets.size()); }

signals:
  /** A web icon arrived. */
  void iconsChanged();

private:
  void poll();
  static QString diskPath(const QString& url);

  std::shared_ptr<NetworkClient> m_network;
  QTimer* m_poll = nullptr;
  std::map<QString, QImage> m_icons;
  std::map<QString, std::shared_ptr<NetworkTicket>> m_tickets;
};

} // namespace ts

#endif // TS_APP_ICONCACHE_H
