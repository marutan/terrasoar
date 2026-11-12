// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/packs/packlibrary.h"

#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace ts
{

namespace
{

QString pendingName()
{
  return QStringLiteral("pending.json"); // ts-notr: file name
}

QString removedMarkerName()
{
  return QStringLiteral(".removed"); // ts-notr: file name
}

QString stagingName()
{
  return QStringLiteral(".staging"); // ts-notr: directory name
}

QString trashName()
{
  return QStringLiteral(".trash"); // ts-notr: directory name
}

bool fail(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return false;
}

/** Reads `pending.json` in @p directory. */
std::optional<PendingPack> readPending(const QString& directory)
{
  QFile file(QDir(directory).filePath(pendingName()));
  if (!file.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
  // The entry was checked when the download started; this is our own file.
  std::optional<PackIndexEntry> entry = PackIndexEntry::fromJson(
      json.value(QStringLiteral("entry")).toObject(), true, nullptr);
  if (!entry) {
    return std::nullopt;
  }
  PendingPack pending;
  pending.entry = std::move(*entry);
  pending.directory = directory;
  pending.ready = json.value(QStringLiteral("ready")).toBool(false);
  return pending;
}

bool writePending(const QString& directory, const PackIndexEntry& entry,
                  bool ready, QString* errorString)
{
  QJsonObject json;
  json.insert(QStringLiteral("entry"), entry.toJson());
  json.insert(QStringLiteral("ready"), ready);
  QSaveFile file(QDir(directory).filePath(pendingName()));
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(QJsonDocument(json).toJson()) < 0 || !file.commit()) {
    return fail(errorString, QStringLiteral("Cannot write %1: %2")
                                 .arg(file.fileName(), file.errorString()));
  }
  return true;
}

/** The pack directories under @p root: not hidden, sorted by name. */
QStringList packNames(const QString& root)
{
  return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

} // namespace

PackLibrary::PackLibrary(QString root) : m_root(std::move(root)) {}

QString PackLibrary::stagedManifestName()
{
  return QStringLiteral("staged-manifest.json"); // ts-notr: file name
}

QString PackLibrary::packDirectory(const QString& id) const
{
  return QDir(m_root).filePath(id);
}

QString PackLibrary::stagingDirectory(const QString& id) const
{
  return QDir(QDir(m_root).filePath(stagingName())).filePath(id);
}

QString PackLibrary::trashSlot(const QString& id) const
{
  const QDir trash(QDir(m_root).filePath(trashName()));
  trash.mkpath(QStringLiteral("."));
  const QString stamp =
      QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
  QString slot = trash.filePath(id + QLatin1Char('-') + stamp);
  for (int n = 1; QFileInfo::exists(slot); ++n) {
    slot = trash.filePath(QStringLiteral("%1-%2-%3").arg(id, stamp).arg(n));
  }
  return slot;
}

std::vector<QString> PackLibrary::tidy()
{
  std::vector<QString> problems;
  // Packs marked removed go to the trash, which is then emptied.
  for (const QString& name : packNames(m_root)) {
    if (QFileInfo::exists(
            QDir(packDirectory(name)).filePath(removedMarkerName()))) {
      QDir().rename(packDirectory(name), trashSlot(name));
    }
  }
  // Ready downloads whose swap failed last time.
  for (const PendingPack& pending : this->pending()) {
    QString error;
    if (pending.ready && !swapIn(pending.entry.id, &error)) {
      problems.push_back(error);
    }
  }
  const QDir trash(QDir(m_root).filePath(trashName()));
  for (const QString& name :
       trash.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
    QDir entry(trash.filePath(name));
    if (!entry.removeRecursively() && !QFile::remove(trash.filePath(name))) {
      problems.push_back(
          QStringLiteral("Cannot delete %1").arg(trash.filePath(name)));
    }
  }
  return problems;
}

std::vector<InstalledPack> PackLibrary::installed() const
{
  std::vector<InstalledPack> packs;
  for (const QString& name : packNames(m_root)) {
    const QDir directory(packDirectory(name));
    if (name.startsWith(QLatin1Char('.')) || !isValidPackId(name) ||
        directory.exists(removedMarkerName())) {
      continue;
    }
    InstalledPack pack;
    pack.directory = directory.path();
    QString error;
    std::optional<PackManifest> manifest =
        PackManifest::read(pack.directory, &error);
    if (!manifest) {
      pack.manifest.id = name;
      pack.damaged = true;
      pack.problem = error;
      packs.push_back(pack);
      continue;
    }
    pack.manifest = std::move(*manifest);
    if (pack.manifest.id != name) {
      pack.damaged = true;
      pack.problem = QStringLiteral("%1: the manifest is for pack %2")
                         .arg(name, pack.manifest.id);
      pack.manifest.id = name;
    }
    for (const PackFile& file : pack.manifest.files) {
      const QFileInfo info(directory.filePath(file.path));
      if (!pack.damaged && (!info.isFile() || info.size() != file.sizeBytes)) {
        pack.damaged = true;
        pack.problem = QStringLiteral("%1: %2 is missing or the wrong size")
                           .arg(name, file.path);
      }
    }
    packs.push_back(pack);
  }
  return packs;
}

std::vector<PendingPack> PackLibrary::pending() const
{
  std::vector<PendingPack> downloads;
  const QDir staging(QDir(m_root).filePath(stagingName()));
  for (const QString& name : packNames(staging.path())) {
    std::optional<PendingPack> pending = readPending(staging.filePath(name));
    if (pending && pending->entry.id == name) {
      downloads.push_back(std::move(*pending));
    }
  }
  return downloads;
}

QString PackLibrary::prepareStaging(const PackIndexEntry& entry,
                                    QString* errorString)
{
  const QString directory = stagingDirectory(entry.id);
  const std::optional<PendingPack> existing = readPending(directory);
  if (!existing || existing->entry.version != entry.version) {
    // Another version, or nothing usable: start afresh.
    QDir(directory).removeRecursively();
  }
  if (!QDir().mkpath(directory)) {
    fail(errorString, QStringLiteral("Cannot create %1").arg(directory));
    return QString();
  }
  if (!writePending(directory, entry, false, errorString)) {
    return QString();
  }
  return directory;
}

void PackLibrary::discardStaging(const QString& id)
{
  const QString directory = stagingDirectory(id);
  if (!QDir(directory).removeRecursively() && QFileInfo::exists(directory)) {
    // Something is still open (Windows): leave it for tidy().
    QDir().rename(directory, trashSlot(id));
  }
}

bool PackLibrary::activate(const QString& id, QString* errorString)
{
  QDir staging(stagingDirectory(id));
  const std::optional<PendingPack> pending = readPending(staging.path());
  if (!pending) {
    return fail(errorString,
                QStringLiteral("No download of pack %1 to install").arg(id));
  }
  if (!writePending(staging.path(), pending->entry, true, errorString)) {
    return false;
  }
  if (staging.exists(stagedManifestName())) {
    staging.remove(PackManifest::fileName());
    if (!staging.rename(stagedManifestName(), PackManifest::fileName())) {
      return fail(errorString,
                  QStringLiteral("Cannot put the manifest of pack %1 in place")
                      .arg(id));
    }
  }
  if (!swapIn(id, errorString)) {
    return false;
  }
  tidy();
  return true;
}

bool PackLibrary::swapIn(const QString& id, QString* errorString)
{
  const QString target = packDirectory(id);
  const QString staging = stagingDirectory(id);
  QString old;
  if (QFileInfo::exists(target)) {
    old = trashSlot(id);
    if (!QDir().rename(target, old)) {
      return fail(errorString,
                  QStringLiteral("Cannot move the installed pack %1 aside "
                                 "(in use?); finishing at the next start")
                      .arg(id));
    }
  }
  if (!QDir().rename(staging, target)) {
    if (!old.isEmpty()) {
      QDir().rename(old, target);
    }
    return fail(errorString,
                QStringLiteral("Cannot move the download of pack %1 into "
                               "place; finishing at the next start")
                    .arg(id));
  }
  QFile::remove(QDir(target).filePath(pendingName()));
  if (!old.isEmpty()) {
    QDir(old).removeRecursively();
  }
  return true;
}

bool PackLibrary::remove(const QString& id)
{
  const QString directory = packDirectory(id);
  if (!QFileInfo::exists(directory)) {
    return true;
  }
  const QString slot = trashSlot(id);
  if (QDir().rename(directory, slot)) {
    QDir(slot).removeRecursively();
    return true;
  }
  // In use (Windows): mark it for tidy() to delete at the next start.
  QFile marker(QDir(directory).filePath(removedMarkerName()));
  const bool marked = marker.open(QIODevice::WriteOnly);
  marker.close();
  return marked;
}

} // namespace ts
