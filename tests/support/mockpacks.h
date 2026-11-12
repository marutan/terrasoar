// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Data packs served by a MockHttpServer, for tests of the pack downloads.
 */
#ifndef TS_TESTS_MOCKPACKS_H
#define TS_TESTS_MOCKPACKS_H

#include <algorithm>
#include <map>
#include <utility>

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QList>
#include <QString>

#include "mockhttpserver.h"
#include "ts/packs/packindex.h"
#include "ts/packs/packmanifest.h"

namespace ts::test
{

/** Contents served by the mock server, by URL path. */
using Served = std::map<QString, QByteArray>;

/**
 * Answers @p request from @p content: honours `Range: bytes=a-b` with a
 * 206 when @p ranges is true, else sends the whole file.
 */
inline MockHttpResponse serve(const Served& content,
                              const MockHttpRequest& request, bool ranges)
{
  const Served::const_iterator found = content.find(request.url.path());
  if (found == content.end()) {
    return mockResponse(404, QByteArray());
  }
  const QByteArray& data = found->second;
  const QByteArray range = request.header(QByteArrayLiteral("range"));
  if (!ranges || !range.startsWith("bytes=")) {
    return mockResponse(200, data);
  }
  const QList<QByteArray> parts = range.mid(6).split('-');
  const qint64 size = data.size();
  const qint64 first = parts.value(0).toLongLong();
  const qint64 last =
      parts.value(1).isEmpty()
          ? size - 1
          : std::min<qint64>(parts.value(1).toLongLong(), size - 1);
  if (first >= size) {
    return mockResponse(416, QByteArray());
  }
  // Test files are small: int fits, and is what Qt 5's mid() takes.
  MockHttpResponse response =
      mockResponse(206, data.mid(static_cast<int>(first),
                                 static_cast<int>(last - first + 1)));
  response.headers.emplace_back(
      QByteArrayLiteral("Content-Range"),
      QStringLiteral("bytes %1-%2/%3").arg(first).arg(last).arg(size).toUtf8());
  return response;
}

/** A pack as a server has it: its files and the index entry for it. */
struct RemotePack {
  /** Its files (and manifest) by URL path, for serve(). */
  Served files;
  /** Its index entry, with the server's URLs. */
  PackIndexEntry entry;
};

/**
 * Makes pack @p id version @p version, whose dataset replaces the base
 * pack's "bluemarble", with @p tiles as its tile file, served under
 * `/<id>/<version>/` by @p server.
 */
// Every call names id and version literally.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline RemotePack remotePack(const MockHttpServer& server, const QString& id,
                             const QString& version, const QByteArray& tiles)
{
  RemotePack pack;
  const QString base = QStringLiteral("/%1/%2/").arg(id, version);
  const std::map<QString, QByteArray> contents = {
      {QStringLiteral("licences/NOTICE.txt"), QByteArrayLiteral("Notice\n")},
      {QStringLiteral("tiles.sqlite"), tiles}};
  PackManifest manifest;
  manifest.id = id;
  manifest.version = version;
  for (const std::pair<const QString, QByteArray>& file : contents) {
    PackFile packFile;
    packFile.path = file.first;
    packFile.sizeBytes = file.second.size();
    packFile.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(file.second, QCryptographicHash::Sha256)
            .toHex());
    manifest.files.push_back(packFile);
    pack.files[base + file.first] = file.second;
    pack.entry.files[file.first] = server.url(base + file.first);
    pack.entry.sizeBytes += file.second.size();
  }
  PackDataset dataset;
  dataset.id = QStringLiteral("bluemarble");
  dataset.file = QStringLiteral("tiles.sqlite");
  dataset.name = QStringLiteral("Full Blue Marble");
  dataset.attribution = QStringLiteral("Full attribution");
  dataset.replaces = QStringLiteral("bluemarble");
  manifest.datasets.push_back(dataset);
  const QByteArray json = QJsonDocument(manifest.toJson()).toJson();
  pack.files[base + PackManifest::fileName()] = json;
  pack.entry.id = id;
  pack.entry.version = version;
  pack.entry.name = QStringLiteral("Test pack");
  pack.entry.manifestUrl = server.url(base + PackManifest::fileName());
  pack.entry.manifestBytes = json.size();
  pack.entry.manifestSha256 = QString::fromLatin1(
      QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex());
  return pack;
}

} // namespace ts::test

#endif // TS_TESTS_MOCKPACKS_H
