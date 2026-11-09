// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <optional>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "concurrency.h"
#include "ts/packs/basepack.h"
#include "ts/packs/packmanifest.h"

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

} // namespace

/** Unit tests for ts/packs: manifests, verification, finding the pack. */
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
};

QTEST_GUILESS_MAIN(TestPacks)
#include "tst_packs.moc"
