// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#include "concurrency.h"
#include "fixturepack.h"
#include "mockhttpserver.h"
#include "mockpacks.h"
#include "ts/network/networkclient.h"
#include "ts/packs/basepack.h"
#include "ts/packs/packindex.h"
#include "ts/packs/packinstall.h"
#include "ts/packs/packlibrary.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/providerregistry.h"
#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilekey.h"

namespace
{

const std::atomic<bool> notCancelled{false};

bool writeFile(const QString& path, const QByteArray& contents)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}

/** Writes a small pack (two files, one dataset) into @p directory. */
// A test helper; every call names both strings literally.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ts::PackManifest writePack(const QString& directory, const QString& id)
{
  const QDir dir(directory);
  dir.mkpath(QStringLiteral("licences"));
  writeFile(dir.filePath(QStringLiteral("tiles.sqlite")),
            QByteArray(5000, 'x'));
  writeFile(dir.filePath(QStringLiteral("licences/NOTICE.txt")),
            QByteArrayLiteral("Notice\n"));
  ts::PackManifest manifest;
  manifest.id = id;
  manifest.version = QStringLiteral("7");
  for (const QString& path : {QStringLiteral("licences/NOTICE.txt"),
                              QStringLiteral("tiles.sqlite")}) {
    ts::PackFile file;
    file.path = path;
    file.sizeBytes = QFileInfo(dir.filePath(path)).size();
    file.sha256 = ts::fileSha256(dir.filePath(path), notCancelled);
    manifest.files.push_back(file);
  }
  ts::PackDataset dataset;
  dataset.id = QStringLiteral("imagery");
  dataset.kind = ts::PackDataset::Kind::Imagery;
  dataset.file = QStringLiteral("tiles.sqlite");
  dataset.name = QStringLiteral("Tiles");
  dataset.licence = QStringLiteral("CC0-1.0");
  dataset.attribution = QStringLiteral("Nobody");
  dataset.source.name = QStringLiteral("Source");
  dataset.source.version = QStringLiteral("1.0");
  dataset.source.url = QStringLiteral("https://example.org/source");
  dataset.source.sha256 = QString(64, QLatin1Char('a'));
  manifest.datasets.push_back(dataset);
  manifest.notices.push_back(QStringLiteral("licences/NOTICE.txt"));
  writeFile(dir.filePath(ts::PackManifest::fileName()),
            QJsonDocument(manifest.toJson()).toJson());
  return manifest;
}

/** A network client for the mock server: no proxy, quick to give up. */
std::shared_ptr<ts::NetworkClient>
testNetwork(qint64 maximumResponseBytes = qint64{16} * 1024 * 1024)
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.attempts = 1;
  settings.timeout = std::chrono::milliseconds(3000);
  settings.maximumResponseBytes = maximumResponseBytes;
  return std::make_shared<ts::NetworkClient>(settings);
}

/** Settings for test installs: small chunks, local http allowed. */
ts::PackInstallSettings testInstallSettings()
{
  ts::PackInstallSettings settings;
  settings.chunkBytes = 1000;
  settings.allowLocalHttp = true;
  return settings;
}

/** Downloads @p entry into @p library and waits; returns the progress. */
ts::PackInstallProgress
installAndWait(ts::PackLibrary& library, const ts::PackIndexEntry& entry,
               const std::shared_ptr<ts::NetworkClient>& network)
{
  QString error;
  const QString staging = library.prepareStaging(entry, &error);
  if (staging.isEmpty()) {
    ts::PackInstallProgress failed;
    failed.state = ts::PackInstallProgress::State::Failed;
    failed.errorString = error;
    return failed;
  }
  ts::PackInstall install(network, entry, staging, testInstallSettings());
  install.start();
  install.waitUntilSettled(std::chrono::seconds(20));
  return install.progress();
}

/** An index entry for pack @p id with one file, a.bin. */
QJsonObject indexEntry(const QString& id, const QString& manifestUrl,
                       const QString& fileUrl)
{
  return QJsonObject{
      {QStringLiteral("id"), id},
      {QStringLiteral("version"), QStringLiteral("1")},
      {QStringLiteral("name"), id},
      {QStringLiteral("bytes"), 10},
      {QStringLiteral("manifest"),
       QJsonObject{{QStringLiteral("url"), manifestUrl},
                   {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
                   {QStringLiteral("bytes"), 5}}},
      {QStringLiteral("files"),
       QJsonObject{{QStringLiteral("a.bin"), fileUrl}}}};
}

/** Every file (not directory) under @p root, relative paths. */
QStringList filesUnder(const QString& root)
{
  QStringList files;
  QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    files << QDir(root).relativeFilePath(it.next());
  }
  return files;
}

} // namespace

/**
 * Unit tests for ts/packs: manifests, verification, finding the base pack,
 * the index, downloading, installing and removing extra packs.
 */
class TestPacks : public QObject
{
  Q_OBJECT

private slots:
  void sha256()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("abc"));
    QVERIFY(writeFile(path, QByteArrayLiteral("abc")));
    QCOMPARE(ts::fileSha256(path, notCancelled),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223"
                            "b00361a396177a9cb410ff61f20015ad"));
    QVERIFY(
        ts::fileSha256(dir.filePath(QStringLiteral("missing")), notCancelled)
            .isEmpty());
    const std::atomic<bool> cancelled{true};
    QVERIFY(ts::fileSha256(path, cancelled).isEmpty());
  }

  void manifestRoundTrip()
  {
    const QTemporaryDir dir;
    const ts::PackManifest written =
        writePack(dir.path(), QStringLiteral("test-pack"));
    QString error;
    const std::optional<ts::PackManifest> read =
        ts::PackManifest::read(dir.path(), &error);
    if (!read) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(read.value().toJson(), written.toJson());
    QCOMPARE(read.value().files.size(), std::size_t{2});
    QCOMPARE(read.value()
                 .dataset(QStringLiteral("imagery"))
                 .value_or(ts::PackDataset())
                 .licence,
             QStringLiteral("CC0-1.0"));
    QVERIFY(!read.value().dataset(QStringLiteral("nothing")).has_value());
  }

  void manifestRejectsBadInput_data()
  {
    QTest::addColumn<QString>("change");
    QTest::newRow("newer format") << QStringLiteral("format");
    QTest::newRow("no id") << QStringLiteral("id");
    QTest::newRow("parent path") << QStringLiteral("../escape");
    QTest::newRow("absolute path") << QStringLiteral("/etc/passwd");
    QTest::newRow("drive path") << QStringLiteral("C:/x");
    QTest::newRow("backslash") << QStringLiteral("a\\b");
    QTest::newRow("short checksum") << QStringLiteral("sha");
    QTest::newRow("unknown kind") << QStringLiteral("kind");
    QTest::newRow("unlisted file") << QStringLiteral("dataset file");
  }

  void manifestRejectsBadInput()
  {
    QFETCH(QString, change);
    const QTemporaryDir dir;
    QJsonObject json =
        writePack(dir.path(), QStringLiteral("test-pack")).toJson();
    QJsonArray files = json.value(QStringLiteral("files")).toArray();
    QJsonObject file = files.at(0).toObject();
    QJsonArray datasets = json.value(QStringLiteral("datasets")).toArray();
    QJsonObject dataset = datasets.at(0).toObject();
    if (change == QLatin1String("format")) {
      json.insert(QStringLiteral("format"),
                  ts::PackManifest::currentFormat + 1);
    } else if (change == QLatin1String("id")) {
      json.remove(QStringLiteral("id"));
    } else if (change == QLatin1String("sha")) {
      file.insert(QStringLiteral("sha256"), QStringLiteral("abc"));
    } else if (change == QLatin1String("kind")) {
      dataset.insert(QStringLiteral("kind"), QStringLiteral("sound"));
    } else if (change == QLatin1String("dataset file")) {
      dataset.insert(QStringLiteral("file"), QStringLiteral("other.sqlite"));
    } else {
      file.insert(QStringLiteral("path"), change);
    }
    files.replace(0, file);
    datasets.replace(0, dataset);
    json.insert(QStringLiteral("files"), files);
    json.insert(QStringLiteral("datasets"), datasets);
    QString error;
    QVERIFY(!ts::PackManifest::fromJson(json, &error).has_value());
    QVERIFY(!error.isEmpty());
  }

  void unreadableManifest()
  {
    const QTemporaryDir dir;
    QString error;
    QVERIFY(!ts::PackManifest::read(dir.path(), &error).has_value());
    QVERIFY(!error.isEmpty());
    QVERIFY(writeFile(dir.filePath(ts::PackManifest::fileName()),
                      QByteArrayLiteral("{ not json")));
    QVERIFY(!ts::PackManifest::read(dir.path(), &error).has_value());
  }

  void verifyFindsCorruption()
  {
    const QTemporaryDir dir;
    const ts::PackManifest manifest =
        writePack(dir.path(), QStringLiteral("test-pack"));
    QVERIFY(manifest.verify(dir.path(), notCancelled).ok);

    // One changed byte: the size is right, the checksum is not.
    const QString tiles = dir.filePath(QStringLiteral("tiles.sqlite"));
    QByteArray changed(5000, 'x');
    changed[2500] = 'y';
    QVERIFY(writeFile(tiles, changed));
    ts::PackVerification result = manifest.verify(dir.path(), notCancelled);
    QVERIFY(!result.ok);
    QCOMPARE(result.problems.size(), std::size_t{1});
    QVERIFY(result.problems[0].contains(QLatin1String("checksum")));

    // Truncated, and a missing file: one problem each.
    QVERIFY(writeFile(tiles, QByteArray(10, 'x')));
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("licences/NOTICE.txt"))));
    result = manifest.verify(dir.path(), notCancelled);
    QCOMPARE(result.problems.size(), std::size_t{2});

    const std::atomic<bool> cancelled{true};
    QVERIFY(!manifest.verify(dir.path(), cancelled).ok);
  }

  void concurrentVerify()
  {
    // Reentrant: each thread verifies with its own manifest copy.
    const QTemporaryDir dir;
    const ts::PackManifest manifest =
        writePack(dir.path(), QStringLiteral("test-pack"));
    const QString path = dir.path();
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&manifest, &path](int) {
          // Each thread its own instance: the reentrancy under test.
          // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
          const ts::PackManifest copy = manifest;
          for (int i = 0; i < 20; ++i) {
            if (!copy.verify(path, notCancelled).ok) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void findBasePack()
  {
    // Layout: <root>/bin/<app>, packs in <root>/bin/basepack,
    // <root>/share/terrasoar/basepack and <root>/basepack.
    const QTemporaryDir root;
    const QDir dir(root.path());
    dir.mkpath(QStringLiteral("bin"));
    const QString applicationDir = dir.filePath(QStringLiteral("bin"));
    qunsetenv("TS_BASEPACK_DIR");
    QVERIFY(ts::findBasePack(applicationDir).isEmpty());

    // The build-tree location is found...
    dir.mkpath(QStringLiteral("basepack"));
    writePack(dir.filePath(QStringLiteral("basepack")), ts::basePackId());
    QCOMPARE(ts::findBasePack(applicationDir),
             QDir::cleanPath(dir.filePath(QStringLiteral("basepack"))));
    // ...but an installed pack comes first, and a pack with another id is
    // not the base pack.
    dir.mkpath(QStringLiteral("share/terrasoar/basepack"));
    writePack(dir.filePath(QStringLiteral("share/terrasoar/basepack")),
              ts::basePackId());
    dir.mkpath(QStringLiteral("bin/basepack"));
    writePack(dir.filePath(QStringLiteral("bin/basepack")),
              QStringLiteral("other-pack"));
    QCOMPARE(ts::findBasePack(applicationDir),
             QDir::cleanPath(
                 dir.filePath(QStringLiteral("share/terrasoar/basepack"))));

    // TS_BASEPACK_DIR replaces the search path.
    const QTemporaryDir other;
    qputenv("TS_BASEPACK_DIR", other.path().toUtf8());
    QCOMPARE(ts::basePackSearchPath(applicationDir),
             QStringList{QDir::cleanPath(other.path())});
    QVERIFY(ts::findBasePack(applicationDir).isEmpty());
    writePack(other.path(), ts::basePackId());
    QCOMPARE(ts::findBasePack(applicationDir), QDir::cleanPath(other.path()));
    qunsetenv("TS_BASEPACK_DIR");
  }

  void indexParsing()
  {
    // One good entry, then a bad id, plain http, loopback http (allowed
    // only in tests) and the first id again.
    const QJsonArray packs{
        indexEntry(QStringLiteral("one"),
                   QStringLiteral("https://example.org/one/manifest.json"),
                   QStringLiteral("https://example.org/one/a.bin")),
        indexEntry(QStringLiteral("Bad Id"),
                   QStringLiteral("https://example.org/m"),
                   QStringLiteral("https://example.org/a")),
        indexEntry(QStringLiteral("plain"),
                   QStringLiteral("http://example.org/m"),
                   QStringLiteral("https://example.org/a")),
        indexEntry(QStringLiteral("local"),
                   QStringLiteral("http://127.0.0.1:1/m"),
                   QStringLiteral("http://127.0.0.1:1/a")),
        indexEntry(QStringLiteral("one"),
                   QStringLiteral("https://example.org/m"),
                   QStringLiteral("https://example.org/a"))};
    const QByteArray json =
        QJsonDocument(QJsonObject{{QStringLiteral("format"), 1},
                                  {QStringLiteral("packs"), packs}})
            .toJson();
    QString error;
    std::vector<QString> skipped;
    std::optional<ts::PackIndex> index =
        ts::PackIndex::fromJson(json, &error, &skipped);
    QVERIFY(index);
    QCOMPARE(index.value_or(ts::PackIndex()).packs.size(), std::size_t{1});
    QCOMPARE(skipped.size(), std::size_t{4});
    const ts::PackIndexEntry one = index.value_or(ts::PackIndex())
                                       .pack(QStringLiteral("one"))
                                       .value_or(ts::PackIndexEntry());
    QCOMPARE(one.version, QStringLiteral("1"));
    QCOMPARE(one.files.at(QStringLiteral("a.bin")),
             QUrl(QStringLiteral("https://example.org/one/a.bin")));
    // Round trip of an entry.
    QCOMPARE(ts::PackIndexEntry::fromJson(one.toJson(), false, nullptr)
                 .value_or(ts::PackIndexEntry())
                 .toJson(),
             one.toJson());

    // Tests may allow http to the loopback address, nothing else.
    skipped.clear();
    index = ts::PackIndex::fromJson(json, &error, &skipped, true);
    QCOMPARE(index.value_or(ts::PackIndex()).packs.size(), std::size_t{2});

    QVERIFY(!ts::PackIndex::fromJson(
        QJsonDocument(QJsonObject{{QStringLiteral("format"), 2}}).toJson(),
        &error, nullptr));
    QVERIFY(error.contains(QStringLiteral("format")));
    QVERIFY(!ts::PackIndex::fromJson("[]", &error, nullptr));
  }

  void manifestReplaces()
  {
    const QTemporaryDir dir;
    ts::PackManifest manifest =
        writePack(dir.path(), QStringLiteral("test-pack"));
    QVERIFY(!manifest.toJson()
                 .value(QStringLiteral("datasets"))
                 .toArray()
                 .at(0)
                 .toObject()
                 .contains(QStringLiteral("replaces")));
    manifest.datasets.at(0).replaces = QStringLiteral("bluemarble");
    QCOMPARE(ts::PackManifest::fromJson(manifest.toJson(), nullptr)
                 .value_or(ts::PackManifest())
                 .datasets.at(0)
                 .replaces,
             QStringLiteral("bluemarble"));
  }

  void installFromScratch()
  {
    ts::test::MockHttpServer server;
    const ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(4500, 'v'));
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, true);
    });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    const ts::PackInstallProgress progress =
        installAndWait(library, pack.entry, testNetwork());
    QCOMPARE(progress.state, ts::PackInstallProgress::State::Verified);
    QCOMPARE(progress.totalBytes, pack.entry.sizeBytes);
    QCOMPARE(progress.doneBytes, pack.entry.sizeBytes);
    // 4500 bytes in chunks of 1000: five requests, each a range.
    int tileRequests = 0;
    for (const ts::test::MockHttpRequest& request : server.requests()) {
      if (request.url.path().endsWith(QStringLiteral("tiles.sqlite"))) {
        ++tileRequests;
        QVERIFY(
            request.header(QByteArrayLiteral("range")).startsWith("bytes="));
      }
    }
    QCOMPARE(tileRequests, 5);

    QString error;
    QVERIFY2(library.activate(QStringLiteral("full"), &error),
             qPrintable(error));
    const std::vector<ts::InstalledPack> installed = library.installed();
    QCOMPARE(installed.size(), std::size_t{1});
    QVERIFY(!installed.at(0).damaged);
    QCOMPARE(installed.at(0).manifest.version, QStringLiteral("1"));
    QVERIFY(library.pending().empty());
    // Only the pack's own files and its manifest are left.
    QStringList files = filesUnder(root.path());
    files.sort();
    QCOMPARE(files, (QStringList{QStringLiteral("full/licences/NOTICE.txt"),
                                 QStringLiteral("full/manifest.json"),
                                 QStringLiteral("full/tiles.sqlite")}));
    QVERIFY(installed.at(0)
                .manifest.verify(installed.at(0).directory, notCancelled)
                .ok);
  }

  void interruptedDownloadResumes()
  {
    ts::test::MockHttpServer server;
    const ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(4500, 'r'));
    // The connection drops after two chunks of the tile file.
    std::shared_ptr<std::atomic<int>> chunks =
        std::make_shared<std::atomic<int>>(0);
    server.setHandler(
        [files = pack.files, chunks](const ts::test::MockHttpRequest& r) {
          if (r.url.path().endsWith(QStringLiteral("tiles.sqlite")) &&
              chunks->fetch_add(1) >= 2) {
            ts::test::MockHttpResponse response;
            response.hangUp = true;
            return response;
          }
          return ts::test::serve(files, r, true);
        });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    const std::shared_ptr<ts::NetworkClient> network = testNetwork();
    ts::PackInstallProgress progress =
        installAndWait(library, pack.entry, network);
    QCOMPARE(progress.state, ts::PackInstallProgress::State::Paused);
    QVERIFY(!progress.errorString.isEmpty());
    QCOMPARE(library.pending().size(), std::size_t{1});
    QVERIFY(QFileInfo(QDir(library.stagingDirectory(QStringLiteral("full")))
                          .filePath(QStringLiteral("tiles.sqlite.part")))
                .size() == 2000);

    // The server is back: a new install continues from byte 2000.
    server.resetLog();
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, true);
    });
    progress = installAndWait(library, pack.entry, network);
    QCOMPARE(progress.state, ts::PackInstallProgress::State::Verified);
    QByteArray firstRange;
    int manifestRequests = 0;
    for (const ts::test::MockHttpRequest& request : server.requests()) {
      if (request.url.path().endsWith(QStringLiteral("tiles.sqlite")) &&
          firstRange.isEmpty()) {
        firstRange = request.header(QByteArrayLiteral("range"));
      }
      manifestRequests +=
          request.url.path().endsWith(QStringLiteral("manifest.json")) ? 1 : 0;
    }
    QCOMPARE(firstRange, QByteArray("bytes=2000-2999"));
    QCOMPARE(manifestRequests, 0); // Kept from the first attempt.
    QString error;
    QVERIFY2(library.activate(QStringLiteral("full"), &error),
             qPrintable(error));
    QVERIFY(!library.installed().at(0).damaged);
  }

  void pauseAndCancel()
  {
    ts::test::MockHttpServer server;
    const ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(4500, 'p'));
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      if (r.url.path().endsWith(QStringLiteral("tiles.sqlite"))) {
        ts::test::MockHttpResponse response;
        response.stall = true;
        return response;
      }
      return ts::test::serve(files, r, true);
    });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    QString error;
    const QString staging = library.prepareStaging(pack.entry, &error);
    {
      ts::PackInstall install(testNetwork(), pack.entry, staging,
                              testInstallSettings());
      install.start();
      QVERIFY(
          QTest::qWaitFor([&]() { return server.requestCount() >= 3; }, 10000));
      QElapsedTimer timer;
      timer.start();
      install.pause();
      QVERIFY(install.waitUntilSettled(std::chrono::seconds(5)));
      QCOMPARE(install.progress().state,
               ts::PackInstallProgress::State::Paused);
      // Cancel after a pause still counts.
      install.cancel();
      QCOMPARE(install.progress().state,
               ts::PackInstallProgress::State::Cancelled);
      QVERIFY(timer.elapsed() < 2000); // Not waiting for the stalled reply.
    }
    library.discardStaging(QStringLiteral("full"));
    library.tidy();
    QVERIFY(library.pending().empty());
    QCOMPARE(filesUnder(root.path()), QStringList());
  }

  void serverIgnoringRanges()
  {
    ts::test::MockHttpServer server;
    const ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(4500, 'i'));
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, false);
    });
    // Whole files that fit the response limit are fine...
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    QCOMPARE(installAndWait(library, pack.entry, testNetwork()).state,
             ts::PackInstallProgress::State::Verified);
    // ...larger ones can't be had in chunks: a clear failure.
    const QTemporaryDir other;
    ts::PackLibrary small(other.path());
    const ts::PackInstallProgress progress =
        installAndWait(small, pack.entry, testNetwork(3000));
    QCOMPARE(progress.state, ts::PackInstallProgress::State::Failed);
    QVERIFY2(progress.errorString.contains(QStringLiteral("resuming")),
             qPrintable(progress.errorString));
  }

  void badChecksumIsRejected()
  {
    ts::test::MockHttpServer server;
    ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(4500, 'c'));
    // The server's file differs from what the manifest lists.
    pack.files[QStringLiteral("/full/1/tiles.sqlite")][100] = 'X';
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, true);
    });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    const ts::PackInstallProgress progress =
        installAndWait(library, pack.entry, testNetwork());
    QCOMPARE(progress.state, ts::PackInstallProgress::State::Failed);
    QVERIFY2(progress.errorString.contains(QStringLiteral("checksum")),
             qPrintable(progress.errorString));
    QVERIFY(library.installed().empty());
    QVERIFY(!QFileInfo::exists(
        QDir(library.stagingDirectory(QStringLiteral("full")))
            .filePath(QStringLiteral("tiles.sqlite.part"))));

    // A manifest that doesn't match the index is refused too.
    ts::test::RemotePack wrong = pack;
    wrong.entry.manifestSha256 = QString(64, QLatin1Char('0'));
    library.discardStaging(QStringLiteral("full"));
    const ts::PackInstallProgress manifest =
        installAndWait(library, wrong.entry, testNetwork());
    QCOMPARE(manifest.state, ts::PackInstallProgress::State::Failed);
    QVERIFY(manifest.errorString.contains(QStringLiteral("manifest")));
  }

  void updateAndRemove()
  {
    ts::test::MockHttpServer server;
    const ts::test::RemotePack v1 =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(1500, '1'));
    const ts::test::RemotePack v2 =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("2"), QByteArray(2500, '2'));
    ts::test::Served files = v1.files;
    files.insert(v2.files.cbegin(), v2.files.cend());
    server.setHandler([files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, true);
    });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    const std::shared_ptr<ts::NetworkClient> network = testNetwork();
    QString error;
    QCOMPARE(installAndWait(library, v1.entry, network).state,
             ts::PackInstallProgress::State::Verified);
    QVERIFY(library.activate(QStringLiteral("full"), &error));
    QCOMPARE(installAndWait(library, v2.entry, network).state,
             ts::PackInstallProgress::State::Verified);
    QVERIFY2(library.activate(QStringLiteral("full"), &error),
             qPrintable(error));
    const std::vector<ts::InstalledPack> installed = library.installed();
    QCOMPARE(installed.size(), std::size_t{1});
    QCOMPARE(installed.at(0).manifest.version, QStringLiteral("2"));
    QCOMPARE(QFileInfo(QDir(installed.at(0).directory)
                           .filePath(QStringLiteral("tiles.sqlite")))
                 .size(),
             qint64{2500});
    QCOMPARE(filesUnder(root.path()).size(), 3);

    // Removing leaves no file behind.
    QVERIFY(library.remove(QStringLiteral("full")));
    QVERIFY(library.installed().empty());
    QCOMPARE(filesUnder(root.path()), QStringList());
  }

  void damagedPackIsReported()
  {
    ts::test::MockHttpServer server;
    const ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(1500, 'd'));
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, true);
    });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    QString error;
    QCOMPARE(installAndWait(library, pack.entry, testNetwork()).state,
             ts::PackInstallProgress::State::Verified);
    QVERIFY(library.activate(QStringLiteral("full"), &error));
    QFile::resize(QDir(library.packDirectory(QStringLiteral("full")))
                      .filePath(QStringLiteral("tiles.sqlite")),
                  10);
    const std::vector<ts::InstalledPack> installed = library.installed();
    QCOMPARE(installed.size(), std::size_t{1});
    QVERIFY(installed.at(0).damaged);
    QVERIFY(installed.at(0).problem.contains(QStringLiteral("tiles.sqlite")));
  }

  void readySwapFinishesAtStart()
  {
    // A download whose swap failed (a file in use on Windows) is ready in
    // the staging area; the next start's tidy() installs it.
    ts::test::MockHttpServer server;
    const ts::test::RemotePack pack =
        ts::test::remotePack(server, QStringLiteral("full"),
                             QStringLiteral("1"), QByteArray(1500, 's'));
    server.setHandler([files = pack.files](const ts::test::MockHttpRequest& r) {
      return ts::test::serve(files, r, true);
    });
    const QTemporaryDir root;
    {
      ts::PackLibrary library(root.path());
      QCOMPARE(installAndWait(library, pack.entry, testNetwork()).state,
               ts::PackInstallProgress::State::Verified);
      // What activate() does before its renames.
      const QDir staging(library.stagingDirectory(QStringLiteral("full")));
      QVERIFY(
          QFile::rename(staging.filePath(ts::PackLibrary::stagedManifestName()),
                        staging.filePath(ts::PackManifest::fileName())));
      QJsonObject pending;
      pending.insert(QStringLiteral("entry"), pack.entry.toJson());
      pending.insert(QStringLiteral("ready"), true);
      QVERIFY(writeFile(staging.filePath(QStringLiteral("pending.json")),
                        QJsonDocument(pending).toJson()));
      QVERIFY(library.installed().empty());
      QVERIFY(library.pending().at(0).ready);
    }
    ts::PackLibrary restarted(root.path());
    QVERIFY(restarted.tidy().empty());
    QCOMPARE(restarted.installed().size(), std::size_t{1});
    QVERIFY(!restarted.installed().at(0).damaged);
    QVERIFY(restarted.pending().empty());
  }

  void registryUsesReplacingPack()
  {
    const QTemporaryDir base;
    const ts::PackManifest baseManifest =
        ts::test::writeFixtureBasePack(base.path(), QColor(10, 20, 30));
    const QTemporaryDir full;
    ts::PackManifest fullManifest =
        ts::test::writeFixtureBasePack(full.path(), QColor(200, 100, 50));
    fullManifest.id = QStringLiteral("bluemarble-full");
    fullManifest.datasets.resize(1); // Only the imagery.
    fullManifest.datasets.at(0).replaces = QStringLiteral("bluemarble");
    fullManifest.datasets.at(0).attribution = QStringLiteral("Full imagery");

    ts::ProviderRegistry registry;
    registry.setBasePack(base.path(), baseManifest);
    const QJsonObject source{
        {QStringLiteral("kind"), QStringLiteral("basepack")},
        {QStringLiteral("dataset"), QStringLiteral("bluemarble")}};
    const QJsonObject shoreline{
        {QStringLiteral("kind"), QStringLiteral("basepack")},
        {QStringLiteral("dataset"), QStringLiteral("shoreline")}};
    QString error;
    const std::shared_ptr<ts::ImageryProvider> before =
        registry.imagery(source, &error);
    QVERIFY(before);
    QCOMPARE(registry.attribution(source), QStringLiteral("Fixture imagery"));
    const ts::TileKey root = ts::TileKey::roots().at(0);
    QCOMPARE(before->loadImage(root, notCancelled).pixelColor(5, 5),
             QColor(10, 20, 30));

    ts::InstalledPack installed;
    installed.directory = full.path();
    installed.manifest = fullManifest;
    registry.setInstalledPacks({installed});
    const std::shared_ptr<ts::ImageryProvider> after =
        registry.imagery(source, &error);
    QVERIFY(after);
    QVERIFY(after != before);
    QCOMPARE(after->loadImage(root, notCancelled).pixelColor(5, 5),
             QColor(200, 100, 50));
    QCOMPARE(registry.attribution(source), QStringLiteral("Full imagery"));
    // Datasets it doesn't replace still come from the base pack.
    QCOMPARE(registry.attribution(shoreline),
             QStringLiteral("Fixture shoreline"));

    // A damaged pack is not used; nor is a removed one.
    installed.damaged = true;
    registry.setInstalledPacks({installed});
    QCOMPARE(registry.attribution(source), QStringLiteral("Fixture imagery"));
    registry.setInstalledPacks({});
    QCOMPARE(registry.imagery(source, &error), before);
  }
};

QTEST_GUILESS_MAIN(TestPacks)
#include "tst_packs.moc"
