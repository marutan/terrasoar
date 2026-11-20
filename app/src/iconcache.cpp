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
#include "iconcache.h"

#include <cstdint>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include "ts/kml/kmlreader.h"
#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

constexpr int32_t pollIntervalMs = 100;

} // namespace

IconCache::IconCache(std::shared_ptr<NetworkClient> network, QObject* parent)
    : QObject(parent), m_network(std::move(network)), m_poll(new QTimer(this))
{
  m_poll->setInterval(pollIntervalMs);
  connect(m_poll, &QTimer::timeout, this, &IconCache::poll);
}

IconCache::~IconCache()
{
  for (const std::pair<const QString, std::shared_ptr<NetworkTicket>>& ticket :
       m_tickets) {
    ticket.second->cancel();
  }
}

const QImage& IconCache::pushpin()
{
  static const QImage pin = []() {
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The needle, then the head, tip at the bottom centre.
    painter.setPen(QPen(QColor(90, 90, 90), 2.0));
    painter.drawLine(QPointF(16.0, 31.0), QPointF(16.0, 14.0));
    painter.setPen(QPen(QColor(120, 90, 0), 1.5));
    painter.setBrush(QColor(255, 214, 0));
    painter.drawEllipse(QPointF(16.0, 10.0), 8.0, 8.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 250, 200));
    painter.drawEllipse(QPointF(13.5, 7.5), 2.5, 2.5);
    return image;
  }();
  return pin;
}

QString IconCache::diskPath(const QString& url)
{
  const QByteArray hash =
      QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("kml-icons/") + QString::fromLatin1(hash));
}

QImage IconCache::icon(const QString& where)
{
  if (where.isEmpty()) {
    return pushpin();
  }
  const std::map<QString, QImage>::const_iterator known = m_icons.find(where);
  if (known != m_icons.end()) {
    return known->second.isNull() ? pushpin() : known->second;
  }
  const bool web = where.startsWith(QLatin1String("http://")) ||
                   where.startsWith(QLatin1String("https://"));
  QImage image;
  if (!web) {
    image = QImage::fromData(readKmlLocalFile(where));
    m_icons[where] = image;
    return image.isNull() ? pushpin() : image;
  }
  // From an earlier fetch, else ask for it.
  QFile saved(diskPath(where));
  if (saved.open(QIODevice::ReadOnly)) {
    image = QImage::fromData(saved.readAll());
    if (!image.isNull()) {
      m_icons[where] = image;
      return image;
    }
  }
  if (m_network && m_tickets.count(where) == 0) {
    m_tickets[where] = m_network->start(HttpRequest{QUrl(where), {}});
    m_poll->start();
  }
  return pushpin();
}

void IconCache::poll()
{
  bool arrived = false;
  std::vector<QString> done;
  for (const std::pair<const QString, std::shared_ptr<NetworkTicket>>& ticket :
       m_tickets) {
    if (!ticket.second->isFinished()) {
      continue;
    }
    done.push_back(ticket.first);
    const HttpResponse response = ticket.second->response();
    const QImage image =
        response.isSuccess() ? QImage::fromData(response.body) : QImage();
    // A failed icon stays the pushpin (null) until the program restarts.
    m_icons[ticket.first] = image;
    if (!image.isNull()) {
      QDir().mkpath(QFileInfo(diskPath(ticket.first)).absolutePath());
      QSaveFile file(diskPath(ticket.first));
      if (file.open(QIODevice::WriteOnly)) {
        file.write(response.body);
        file.commit();
      }
      arrived = true;
    }
  }
  for (const QString& key : done) {
    m_tickets.erase(key);
  }
  if (m_tickets.empty()) {
    m_poll->stop();
  }
  if (arrived) {
    emit iconsChanged();
  }
}

} // namespace ts
