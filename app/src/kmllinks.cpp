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
#include "kmllinks.h"

#include <cstdint>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>

#include "ts/kml/kmlfeatureprovider.h"
#include "ts/kml/kmlreader.h"
#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

constexpr int32_t pollIntervalMs = 100;
constexpr int32_t msPerS = 1000;

} // namespace

KmlLinks::KmlLinks(std::shared_ptr<NetworkClient> network, QObject* parent)
    : QObject(parent), m_network(std::move(network)), m_poll(new QTimer(this))
{
  m_poll->setInterval(pollIntervalMs);
  connect(m_poll, &QTimer::timeout, this, &KmlLinks::poll);
}

KmlLinks::~KmlLinks()
{
  for (const std::pair<const QString, Link>& link : m_links) {
    if (link.second.ticket) {
      link.second.ticket->cancel();
    }
  }
}

void KmlLinks::add(const QString& nodeId,
                   std::shared_ptr<const KmlDocument> document,
                   const QString& featurePath)
{
  const KmlFeature* feature =
      document ? kmlContainer(*document, featurePath) : nullptr;
  if (feature == nullptr || !feature->link) {
    return;
  }
  Link link;
  link.document = std::move(document);
  link.featurePath = featurePath;
  link.feature = feature;
  const KmlLink& settings = *feature->link;
  if (settings.refreshMode == KmlLink::RefreshMode::OnInterval &&
      settings.refreshIntervalS > 0.0) {
    link.interval = new QTimer(this);
    link.interval->setInterval(
        static_cast<int32_t>(settings.refreshIntervalS * msPerS));
    connect(link.interval, &QTimer::timeout, this, [this, nodeId]() {
      const std::map<QString, Link>::iterator found = m_links.find(nodeId);
      if (found != m_links.end()) {
        load(nodeId, found->second.view);
      }
    });
  }
  if (settings.viewRefreshMode == KmlLink::ViewRefreshMode::OnStop) {
    link.afterStop = new QTimer(this);
    link.afterStop->setSingleShot(true);
    link.afterStop->setInterval(static_cast<int32_t>(
        std::max(settings.viewRefreshTimeS, 0.0) * msPerS));
    connect(link.afterStop, &QTimer::timeout, this, [this, nodeId]() {
      const std::map<QString, Link>::iterator found = m_links.find(nodeId);
      if (found != m_links.end()) {
        load(nodeId, found->second.view);
      }
    });
  }
  remove(nodeId);
  m_links[nodeId] = std::move(link);
}

void KmlLinks::remove(const QString& nodeId)
{
  const std::map<QString, Link>::iterator found = m_links.find(nodeId);
  if (found == m_links.end()) {
    return;
  }
  if (found->second.ticket) {
    found->second.ticket->cancel();
  }
  delete found->second.interval;
  delete found->second.afterStop;
  m_links.erase(found);
}

bool KmlLinks::contains(const QString& nodeId) const
{
  return m_links.count(nodeId) != 0;
}

bool KmlLinks::isLoaded(const QString& nodeId) const
{
  const std::map<QString, Link>::const_iterator found = m_links.find(nodeId);
  return found != m_links.end() && found->second.loaded;
}

QStringList KmlLinks::nodeIds() const
{
  QStringList ids;
  for (const std::pair<const QString, Link>& link : m_links) {
    ids << link.first;
  }
  return ids;
}

const KmlFeature* KmlLinks::link(const QString& nodeId) const
{
  const std::map<QString, Link>::const_iterator found = m_links.find(nodeId);
  return found != m_links.end() ? found->second.feature : nullptr;
}

std::shared_ptr<const KmlDocument>
KmlLinks::document(const QString& nodeId) const
{
  const std::map<QString, Link>::const_iterator found = m_links.find(nodeId);
  return found != m_links.end() ? found->second.document : nullptr;
}

QString KmlLinks::featurePath(const QString& nodeId) const
{
  const std::map<QString, Link>::const_iterator found = m_links.find(nodeId);
  return found != m_links.end() ? found->second.featurePath : QString();
}

QUrl KmlLinks::lastUrl(const QString& nodeId) const
{
  const std::map<QString, Link>::const_iterator found = m_links.find(nodeId);
  return found != m_links.end() ? found->second.url : QUrl();
}

int32_t KmlLinks::pending() const
{
  int32_t count = 0;
  for (const std::pair<const QString, Link>& link : m_links) {
    count += link.second.ticket ? 1 : 0;
  }
  return count;
}

void KmlLinks::load(const QString& nodeId, const KmlViewParameters& view)
{
  const std::map<QString, Link>::iterator found = m_links.find(nodeId);
  if (found == m_links.end()) {
    return;
  }
  Link& link = found->second;
  link.view = view;
  if (link.ticket) {
    return; // already on its way
  }
  link.url = kmlLinkUrl(*link.feature->link, *link.document, view);
  if (link.url.isLocalFile()) {
    const QString where = link.url.toLocalFile();
    if (QFileInfo::exists(where)) {
      // Told from the event loop, not from inside whoever asked.
      link.loaded = true;
      const bool fly = link.feature->flyToView;
      QTimer::singleShot(0, this, [this, nodeId, where, fly]() {
        if (contains(nodeId)) {
          emit loaded(nodeId, where, fly);
        }
      });
    } else {
      // Inside a KMZ: copied out.
      const QByteArray body = readKmlLocalFile(where);
      if (body.isEmpty()) {
        emit failed(nodeId, QCoreApplication::translate("ts::KmlLinks",
                                                        "\"%1\" can't be read.")
                                .arg(link.feature->link->href));
      } else {
        deliver(nodeId, link, body);
      }
    }
  } else if (m_network) {
    link.ticket = m_network->start(HttpRequest{link.url, {}});
    m_poll->start();
  }
  if (link.interval != nullptr && !link.interval->isActive()) {
    link.interval->start();
  }
}

void KmlLinks::viewStopped(const KmlViewParameters& view)
{
  for (std::pair<const QString, Link>& link : m_links) {
    if (link.second.afterStop != nullptr && link.second.loaded) {
      link.second.view = view;
      link.second.afterStop->start();
    }
  }
}

void KmlLinks::deliver(const QString& nodeId, Link& link,
                       const QByteArray& body)
{
  // Each copy a file of its own, so a document read from the last stays
  // as it was.
  const bool kmz = body.startsWith("PK");
  const QByteArray hash =
      QCryptographicHash::hash(nodeId.toUtf8(), QCryptographicHash::Sha1)
          .toHex()
          .left(16);
  const QString path = m_folder.filePath(
      QStringLiteral("%1-%2.%3")
          .arg(QString::fromLatin1(hash))
          .arg(++link.generation)
          .arg(kmz ? QStringLiteral("kmz") : QStringLiteral("kml")));
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() ||
      !file.commit()) {
    emit failed(nodeId, file.errorString());
    return;
  }
  if (!link.url.isLocalFile()) {
    setKmlWebBase(path, link.url.toString());
  }
  link.loaded = true;
  emit loaded(nodeId, path, link.feature->flyToView);
}

void KmlLinks::poll()
{
  std::vector<QString> finished;
  for (std::pair<const QString, Link>& link : m_links) {
    if (link.second.ticket && link.second.ticket->isFinished()) {
      finished.push_back(link.first);
    }
  }
  for (const QString& nodeId : finished) {
    const std::map<QString, Link>::iterator found = m_links.find(nodeId);
    if (found == m_links.end()) {
      continue;
    }
    Link& link = found->second;
    const HttpResponse response = link.ticket->response();
    link.ticket.reset();
    if (response.isSuccess()) {
      deliver(nodeId, link, response.body);
    } else {
      emit failed(nodeId, QCoreApplication::translate(
                              "ts::KmlLinks", "%1 couldn't be fetched (%2).")
                              .arg(link.url.toDisplayString(),
                                   QString::number(response.status)));
    }
  }
  if (pending() == 0) {
    m_poll->stop();
  }
}

} // namespace ts
