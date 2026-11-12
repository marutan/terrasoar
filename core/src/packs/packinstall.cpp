// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/packs/packinstall.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QList>
#include <QSaveFile>

#include "ts/network/networkclient.h"
#include "ts/packs/packlibrary.h"

namespace ts
{

namespace
{

/** The first byte of a `Content-Range: bytes <first>-<last>/<total>`. */
std::optional<qint64> rangeStart(const QByteArray& contentRange)
{
  const QByteArray prefix = QByteArrayLiteral("bytes ");
  if (!contentRange.startsWith(prefix)) {
    return std::nullopt;
  }
  // "bytes <first>-<last>/<size>". Split, not indexOf(): its type differs
  // between Qt 5 (int) and Qt 6 (qsizetype).
  const QList<QByteArray> parts = contentRange.mid(prefix.size()).split('-');
  bool ok = false;
  const qint64 first = parts.size() > 1 ? parts.front().toLongLong(&ok) : 0;
  return ok ? std::optional<qint64>(first) : std::nullopt;
}

QString sha256(const QByteArray& data)
{
  return QString::fromLatin1(
      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString describe(const HttpResponse& response)
{
  return response.error == NetworkError::None
             ? QStringLiteral("HTTP %1").arg(response.status)
             : response.errorString;
}

} // namespace

PackInstall::PackInstall(std::shared_ptr<NetworkClient> network,
                         PackIndexEntry entry, QString stagingDirectory,
                         PackInstallSettings settings)
    : m_network(std::move(network)), m_entry(std::move(entry)),
      m_directory(std::move(stagingDirectory)), m_settings(settings)
{
}

PackInstall::~PackInstall()
{
  stop(PackInstallProgress::State::Paused);
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void PackInstall::start()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (m_progress.state != PackInstallProgress::State::Ready) {
    return;
  }
  m_progress.state = PackInstallProgress::State::Running;
  m_thread = std::thread([this]() { work(); });
}

void PackInstall::pause()
{
  stop(PackInstallProgress::State::Paused);
}

void PackInstall::cancel()
{
  stop(PackInstallProgress::State::Cancelled);
}

void PackInstall::stop(PackInstallProgress::State state)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_stop.store(true);
  // A running install takes either; a paused one can still be cancelled.
  const bool paused = m_progress.state == PackInstallProgress::State::Paused;
  if ((!m_progress.isOver() && !paused) ||
      (paused && state == PackInstallProgress::State::Cancelled)) {
    m_progress.state = state;
  }
  m_changed.notify_all();
}

void PackInstall::settle(PackInstallProgress::State state, const QString& error)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  // A pause or cancel from outside wins over what the thread found.
  if (m_progress.state == PackInstallProgress::State::Running) {
    m_progress.state = state;
    m_progress.errorString = error;
  }
  m_changed.notify_all();
}

void PackInstall::addDone(qint64 bytes)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_progress.doneBytes += bytes;
}

PackInstallProgress PackInstall::progress() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_progress;
}

std::optional<PackManifest> PackInstall::manifest() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_manifest;
}

bool PackInstall::waitUntilSettled(std::chrono::milliseconds timeout) const
{
  std::unique_lock<std::mutex> lock(m_mutex);
  return m_changed.wait_for(lock, timeout, [this]() {
    return m_progress.isOver() ||
           m_progress.state == PackInstallProgress::State::Paused;
  });
}

void PackInstall::work()
{
  if (!fetchManifest()) {
    return;
  }
  const std::optional<PackManifest> manifest = this->manifest();
  if (!manifest) {
    return;
  }
  for (const PackFile& file : manifest->files) {
    if (!fetchFile(file)) {
      return;
    }
  }
  settle(PackInstallProgress::State::Verified, QString());
}

bool PackInstall::fetchManifest()
{
  const QDir directory(m_directory);
  const QString path = directory.filePath(PackLibrary::stagedManifestName());
  QByteArray json;
  QFile staged(path);
  if (staged.open(QIODevice::ReadOnly)) {
    json = staged.readAll();
    staged.close();
  }
  if (sha256(json) != m_entry.manifestSha256 &&
      !downloadManifest(path, &json)) {
    return false;
  }
  QString error;
  std::optional<PackManifest> manifest =
      PackManifest::fromJson(QJsonDocument::fromJson(json).object(), &error);
  if (manifest && manifest->id != m_entry.id) {
    error = QStringLiteral("it is for pack %1").arg(manifest->id);
    manifest.reset();
  }
  if (manifest) {
    for (const PackFile& file : manifest->files) {
      if (m_entry.files.count(file.path) == 0) {
        error =
            QStringLiteral("the index has no address for %1").arg(file.path);
        manifest.reset();
        break;
      }
    }
  }
  if (!manifest) {
    settle(PackInstallProgress::State::Failed,
           QCoreApplication::translate("ts::PackInstall",
                                       "The pack's manifest can't be used: %1")
               .arg(error));
    return false;
  }
  // What is on disk already counts as done.
  qint64 total = 0;
  qint64 done = 0;
  for (const PackFile& file : manifest->files) {
    total += file.sizeBytes;
    const QFileInfo finished(directory.filePath(file.path));
    const QFileInfo part(directory.filePath(file.path) +
                         QStringLiteral(".part"));
    if (finished.isFile() && finished.size() == file.sizeBytes) {
      done += file.sizeBytes;
    } else if (part.isFile()) {
      done += std::min(part.size(), file.sizeBytes);
    }
  }
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_manifest = std::move(manifest);
  m_progress.totalBytes = total;
  m_progress.doneBytes = done;
  return true;
}

bool PackInstall::downloadManifest(const QString& path, QByteArray* json)
{
  HttpRequest request;
  request.url = m_entry.manifestUrl;
  const HttpResponse response = m_network->fetch(request, m_stop);
  if (response.error == NetworkError::Cancelled || m_stop.load()) {
    return false;
  }
  if (!response.isSuccess() || response.status != 200) {
    settle(PackInstallProgress::State::Paused,
           QCoreApplication::translate(
               "ts::PackInstall", "Cannot download the pack's manifest: %1")
               .arg(describe(response)));
    return false;
  }
  if (sha256(response.body) != m_entry.manifestSha256) {
    settle(PackInstallProgress::State::Failed,
           QCoreApplication::translate(
               "ts::PackInstall",
               "The pack's manifest is damaged (checksum mismatch)."));
    return false;
  }
  *json = response.body;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(*json) < 0 ||
      !file.commit()) {
    settle(PackInstallProgress::State::Failed,
           QCoreApplication::translate("ts::PackInstall", "Cannot write %1: %2")
               .arg(path, file.errorString()));
    return false;
  }
  return true;
}

PackInstall::Chunk PackInstall::takeChunk(const PackFile& file,
                                          const HttpResponse& response,
                                          qint64 have, QByteArray* body)
{
  if (response.error == NetworkError::Cancelled || m_stop.load()) {
    return Chunk::Stop;
  }
  const QString noResume = QCoreApplication::translate(
      "ts::PackInstall",
      "The server of %1 does not support resuming downloads.");
  if (response.error == NetworkError::TooLarge) {
    // More than a chunk came back: the server ignored the range.
    settle(PackInstallProgress::State::Failed, noResume.arg(file.path));
    return Chunk::Stop;
  }
  if (response.error != NetworkError::None) {
    settle(
        PackInstallProgress::State::Paused,
        QCoreApplication::translate("ts::PackInstall", "Cannot download %1: %2")
            .arg(file.path, describe(response)));
    return Chunk::Stop;
  }
  switch (response.status) {
  case 416:
    return Chunk::Complete; // Nothing more to have: check what there is.
  case 200:
    // The whole file, from a server that ignores ranges.
    if (response.body.size() != file.sizeBytes) {
      settle(PackInstallProgress::State::Failed, noResume.arg(file.path));
      return Chunk::Stop;
    }
    *body = response.body;
    return Chunk::Restart;
  case 206:
    if (rangeStart(response.header(QByteArrayLiteral("content-range"))) !=
            std::optional<qint64>(have) ||
        response.body.isEmpty()) {
      settle(PackInstallProgress::State::Failed,
             QCoreApplication::translate(
                 "ts::PackInstall", "The server sent the wrong part of %1.")
                 .arg(file.path));
      return Chunk::Stop;
    }
    *body = response.body;
    body->truncate(static_cast<int>(
        std::min<qint64>(body->size(), file.sizeBytes - have)));
    return Chunk::Append;
  default: {
    // 408, 429 and 5xx were retried by the client already.
    const bool later = response.status == 408 || response.status == 429 ||
                       response.status >= 500;
    settle(
        later ? PackInstallProgress::State::Paused
              : PackInstallProgress::State::Failed,
        QCoreApplication::translate("ts::PackInstall", "Cannot download %1: %2")
            .arg(file.path, describe(response)));
    return Chunk::Stop;
  }
  }
}

bool PackInstall::fetchFile(const PackFile& file)
{
  const QDir directory(m_directory);
  const QString finalPath = directory.filePath(file.path);
  const QFileInfo finished(finalPath);
  // Files are renamed into place only after their checksum matched.
  if (finished.isFile() && finished.size() == file.sizeBytes) {
    return true;
  }
  QDir().mkpath(finished.absolutePath());
  const QString partPath = finalPath + QStringLiteral(".part");
  QFile part(partPath);
  if (!part.open(QIODevice::ReadWrite)) {
    settle(PackInstallProgress::State::Failed,
           QCoreApplication::translate("ts::PackInstall", "Cannot write %1: %2")
               .arg(partPath, part.errorString()));
    return false;
  }
  qint64 have = part.size();
  if (have > file.sizeBytes) {
    addDone(-std::min(have, file.sizeBytes));
    part.resize(0);
    have = 0;
  }
  part.seek(have);
  const QUrl url = m_entry.files.at(file.path);
  while (have < file.sizeBytes) {
    const qint64 last =
        std::min(have + m_settings.chunkBytes, file.sizeBytes) - 1;
    HttpRequest request;
    request.url = url;
    request.headers.emplace_back(QByteArrayLiteral("Range"),
                                 QByteArrayLiteral("bytes=") +
                                     QByteArray::number(have) + '-' +
                                     QByteArray::number(last));
    const HttpResponse response = m_network->fetch(request, m_stop);
    QByteArray body;
    const Chunk chunk = takeChunk(file, response, have, &body);
    if (chunk == Chunk::Stop) {
      return false;
    }
    if (chunk == Chunk::Complete) {
      break;
    }
    if (chunk == Chunk::Restart) {
      addDone(-have);
      part.resize(0);
      part.seek(0);
      have = 0;
    }
    if (part.write(body) != body.size() || !part.flush()) {
      settle(
          PackInstallProgress::State::Failed,
          QCoreApplication::translate("ts::PackInstall", "Cannot write %1: %2")
              .arg(partPath, part.errorString()));
      return false;
    }
    have += body.size();
    addDone(body.size());
  }
  part.close();
  const QString sha256 = fileSha256(partPath, m_stop);
  if (m_stop.load()) {
    return false;
  }
  if (sha256 != file.sha256) {
    addDone(-QFileInfo(partPath).size());
    QFile::remove(partPath);
    settle(PackInstallProgress::State::Failed,
           QCoreApplication::translate("ts::PackInstall",
                                       "%1 is damaged (checksum mismatch).")
               .arg(file.path));
    return false;
  }
  QFile::remove(finalPath);
  if (!QFile::rename(partPath, finalPath)) {
    settle(PackInstallProgress::State::Failed,
           QCoreApplication::translate("ts::PackInstall", "Cannot write %1.")
               .arg(finalPath));
    return false;
  }
  return true;
}

} // namespace ts
