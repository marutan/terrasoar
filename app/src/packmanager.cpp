// SPDX-License-Identifier: GPL-2.0-or-later
#include "packmanager.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QStringList>

#include "ts/network/networkclient.h"
#include "ts/providers/providerregistry.h"

namespace ts
{

namespace
{

/** How often installs and the index are looked at, ms. */
constexpr int pollIntervalMs = 250;

} // namespace

PackManager::PackManager(ProviderRegistry& registry, const QString& root,
                         QObject* parent)
    : QObject(parent), m_registry(registry), m_library(root),
      m_indexUrl(defaultPackIndexUrl())
{
  for (const QString& problem : m_library.tidy()) {
    qWarning("Packs: %s", qUtf8Printable(problem));
  }
  reload();
  m_poll.setInterval(pollIntervalMs);
  connect(&m_poll, &QTimer::timeout, this, &PackManager::poll);
}

PackManager::~PackManager()
{
  if (m_indexTicket) {
    m_indexTicket->cancel();
  }
  // Stop them all before waiting for any; the downloads stay.
  for (const std::pair<const QString, std::unique_ptr<PackInstall>>& install :
       m_installs) {
    install.second->pause();
  }
  m_installs.clear();
}

void PackManager::reload()
{
  m_installed = m_library.installed();
  m_pending = m_library.pending();
  for (const InstalledPack& pack : m_installed) {
    if (pack.damaged) {
      qWarning("Pack %s is damaged: %s", qUtf8Printable(pack.manifest.id),
               qUtf8Printable(pack.problem));
    }
  }
  m_registry.setInstalledPacks(m_installed);
  emit layersChanged();
  emit packsChanged();
}

const InstalledPack* PackManager::installedPack(const QString& id) const
{
  for (const InstalledPack& pack : m_installed) {
    if (pack.manifest.id == id) {
      return &pack;
    }
  }
  return nullptr;
}

std::optional<PendingPack> PackManager::pending(const QString& id) const
{
  for (const PendingPack& pack : m_pending) {
    if (pack.entry.id == id) {
      return pack;
    }
  }
  return std::nullopt;
}

std::vector<QString> PackManager::packIds() const
{
  std::vector<QString> ids;
  if (m_index) {
    ids.reserve(m_index->packs.size());
    for (const PackIndexEntry& entry : m_index->packs) {
      ids.push_back(entry.id);
    }
  }
  std::vector<QString> others;
  others.reserve(m_installed.size() + m_pending.size());
  for (const InstalledPack& pack : m_installed) {
    others.push_back(pack.manifest.id);
  }
  for (const PendingPack& pack : m_pending) {
    others.push_back(pack.entry.id);
  }
  std::sort(others.begin(), others.end());
  for (const QString& id : others) {
    if (std::find(ids.cbegin(), ids.cend(), id) == ids.cend()) {
      ids.push_back(id);
    }
  }
  return ids;
}

void PackManager::refreshIndex()
{
  if (m_indexState == IndexState::Loading) {
    return;
  }
  if (!isAllowedPackUrl(m_indexUrl, m_allowLocalHttp)) {
    m_indexState = IndexState::Failed;
    m_indexError = tr("The address of the list of packs is not secure: %1")
                       .arg(m_indexUrl.toString());
    emit packsChanged();
    return;
  }
  HttpRequest request;
  request.url = m_indexUrl;
  m_indexTicket = m_registry.network()->start(request);
  m_indexState = IndexState::Loading;
  m_indexError.clear();
  m_poll.start();
  emit packsChanged();
}

void PackManager::pollIndex()
{
  if (!m_indexTicket || !m_indexTicket->isFinished()) {
    return;
  }
  const HttpResponse response = m_indexTicket->response();
  m_indexTicket.reset();
  if (!response.isSuccess() || response.status != 200) {
    m_indexState = IndexState::Failed;
    m_indexError = tr("Cannot load the list of packs: %1")
                       .arg(response.error == NetworkError::None
                                ? tr("HTTP status %1").arg(response.status)
                                : response.errorString);
    emit packsChanged();
    return;
  }
  QString error;
  std::vector<QString> skipped;
  std::optional<PackIndex> index =
      PackIndex::fromJson(response.body, &error, &skipped, m_allowLocalHttp);
  for (const QString& problem : skipped) {
    qWarning("%s", qUtf8Printable(problem));
  }
  if (!index) {
    m_indexState = IndexState::Failed;
    m_indexError = tr("Cannot read the list of packs: %1").arg(error);
  } else {
    m_index = std::move(index);
    m_indexState = IndexState::Loaded;
  }
  emit packsChanged();
}

bool PackManager::install(const QString& id, QString* errorString)
{
  const std::map<QString, std::unique_ptr<PackInstall>>::iterator running =
      m_installs.find(id);
  if (running != m_installs.end() && running->second->progress().state ==
                                         PackInstallProgress::State::Running) {
    return true;
  }
  std::optional<PackIndexEntry> entry =
      m_index ? m_index->pack(id) : std::nullopt;
  if (!entry) {
    const std::optional<PendingPack> download = pending(id);
    if (download) {
      entry = download->entry;
    }
  }
  if (!entry) {
    if (errorString) {
      *errorString = tr("The pack %1 is not in the list of packs.").arg(id);
    }
    return false;
  }
  if (running != m_installs.end()) {
    m_installs.erase(running); // A paused or failed one: start afresh.
  }
  QString error;
  const QString staging = m_library.prepareStaging(*entry, &error);
  if (staging.isEmpty()) {
    qWarning("Pack %s: %s", qUtf8Printable(id), qUtf8Printable(error));
    if (errorString) {
      *errorString = tr("Cannot prepare the download: %1").arg(error);
    }
    return false;
  }
  PackInstallSettings settings;
  settings.allowLocalHttp = m_allowLocalHttp;
  std::unique_ptr<PackInstall> install = std::make_unique<PackInstall>(
      m_registry.network(), *entry, staging, settings);
  install->start();
  m_installs[id] = std::move(install);
  m_pending = m_library.pending();
  m_poll.start();
  emit packsChanged();
  return true;
}

void PackManager::pause(const QString& id)
{
  const std::map<QString, std::unique_ptr<PackInstall>>::iterator install =
      m_installs.find(id);
  if (install != m_installs.end()) {
    install->second->pause();
    emit progressChanged();
    emit packsChanged();
  }
}

void PackManager::cancel(const QString& id)
{
  const std::map<QString, std::unique_ptr<PackInstall>>::iterator install =
      m_installs.find(id);
  if (install != m_installs.end()) {
    install->second->cancel();
    m_installs.erase(install);
  }
  m_library.discardStaging(id);
  m_pending = m_library.pending();
  emit progressChanged();
  emit packsChanged();
}

void PackManager::remove(const QString& id)
{
  cancel(id);
  // Close its files first: the layers let go of their providers.
  std::vector<InstalledPack> others = m_installed;
  others.erase(std::remove_if(others.begin(), others.end(),
                              [&id](const InstalledPack& pack) {
                                return pack.manifest.id == id;
                              }),
               others.end());
  m_registry.setInstalledPacks(others);
  emit layersChanged();
  if (!m_library.remove(id)) {
    qWarning("Pack %s: cannot remove it", qUtf8Printable(id));
  }
  reload();
}

void PackManager::activate(const QString& id)
{
  if (installedPack(id)) {
    // An update: close the old version's files first.
    std::vector<InstalledPack> others = m_installed;
    others.erase(std::remove_if(others.begin(), others.end(),
                                [&id](const InstalledPack& pack) {
                                  return pack.manifest.id == id;
                                }),
                 others.end());
    m_registry.setInstalledPacks(others);
    emit layersChanged();
  }
  QString error;
  if (!m_library.activate(id, &error)) {
    qWarning("Pack %s: %s", qUtf8Printable(id), qUtf8Printable(error));
  }
  reload();
}

std::optional<PackInstallProgress>
PackManager::progress(const QString& id) const
{
  const std::map<QString, std::unique_ptr<PackInstall>>::const_iterator
      install = m_installs.find(id);
  if (install == m_installs.end()) {
    return std::nullopt;
  }
  return install->second->progress();
}

QString PackManager::statusText() const
{
  QStringList parts;
  for (const std::pair<const QString, std::unique_ptr<PackInstall>>& install :
       m_installs) {
    const PackInstallProgress progress = install.second->progress();
    if (progress.state == PackInstallProgress::State::Running) {
      parts << tr("Pack %1: %2 %")
                   .arg(install.second->entry().name)
                   .arg(std::floor(progress.fraction() * 100.0));
    }
  }
  return parts.join(QStringLiteral(" · "));
}

void PackManager::poll()
{
  pollIndex();
  std::vector<QString> verified;
  bool running = false;
  bool changed = false;
  for (const std::pair<const QString, std::unique_ptr<PackInstall>>& install :
       m_installs) {
    const PackInstallProgress::State state = install.second->progress().state;
    if (state == PackInstallProgress::State::Verified) {
      verified.push_back(install.first);
    }
    running = running || state == PackInstallProgress::State::Running;
    const std::map<QString, PackInstallProgress::State>::iterator last =
        m_states.find(install.first);
    changed = changed || last == m_states.end() || last->second != state;
    m_states[install.first] = state;
  }
  for (const QString& id : verified) {
    m_installs.erase(id);
    activate(id);
  }
  if (!running && !m_indexTicket) {
    m_poll.stop();
  }
  emit progressChanged();
  if (changed) {
    emit packsChanged();
  }
}

} // namespace ts
