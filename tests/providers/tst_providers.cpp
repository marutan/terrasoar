// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "concurrency.h"
#include "ts/deps/sqliteconnection.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/providers/testproviders.h"

namespace
{

const std::atomic<bool> notCancelled{false};

/** Number of pixels of @p image that are exactly @p color. */
int countColor(const QImage& image, QRgb color)
{
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      count += image.pixel(x, y) == color ? 1 : 0;
    }
  }
  return count;
}

QByteArray encodePng(const QImage& image)
{
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

/** A tile whose colour encodes its key, for checking what comes back. */
QImage keyedTile(ts::TileKey key, int size)
{
  QImage image(size, size, QImage::Format_RGB32);
  image.fill(QColor(key.level * 20, key.x * 10, key.y * 10));
  return image;
}

/** Little-endian WKB, built by hand. */
class Wkb
{
public:
  Wkb& byte(std::uint8_t value)
  {
    m_bytes.append(static_cast<char>(value));
    return *this;
  }
  Wkb& uint32(std::uint32_t value)
  {
    for (int i = 0; i < 4; ++i) {
      byte(static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(i))));
    }
    return *this;
  }
  Wkb& float64(double value)
  {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    for (int i = 0; i < 8; ++i) {
      byte(static_cast<std::uint8_t>(bits >> (8U * static_cast<unsigned>(i))));
    }
    return *this;
  }
  /** A polygon of one ring through @p lonLat pairs (closed here). */
  Wkb& polygon(const std::vector<double>& lonLat)
  {
    const std::uint32_t points = static_cast<std::uint32_t>(lonLat.size() / 2);
    byte(1).uint32(3).uint32(1).uint32(points + 1);
    for (double value : lonLat) {
      float64(value);
    }
    return float64(lonLat[0]).float64(lonLat[1]);
  }
  /** A line string through @p lonLat pairs. */
  Wkb& lineString(const std::vector<double>& lonLat)
  {
    byte(1).uint32(2).uint32(static_cast<std::uint32_t>(lonLat.size() / 2));
    for (double value : lonLat) {
      float64(value);
    }
    return *this;
  }
  QByteArray bytes() const { return m_bytes; }

private:
  QByteArray m_bytes;
};

/** A GeoPackage geometry blob: header without envelope, then WKB. */
QByteArray gpkgBlob(const QByteArray& wkb)
{
  Wkb header;
  header.byte('G').byte('P').byte(0).byte(0x01).uint32(4326);
  return header.bytes() + wkb;
}

/** A square of @p sizeDeg at (@p lonDeg, @p latDeg) as WKB. */
QByteArray square(double lonDeg, double latDeg, double sizeDeg)
{
  return Wkb()
      .polygon({lonDeg, latDeg, lonDeg + sizeDeg, latDeg, lonDeg + sizeDeg,
                latDeg + sizeDeg, lonDeg, latDeg + sizeDeg})
      .bytes();
}

/** Writes the GeoPackage used by the feature tests into @p path. */
bool writeFeaturePackage(const QString& path)
{
  ts::GeoPackageFeatureWriter writer(path);
  ts::FeatureTableSpec coarse;
  coarse.name = QStringLiteral("land_coarse");
  coarse.storageLevel = 1;
  coarse.minLevel = 1;
  coarse.maxLevel = 2;
  ts::FeatureTableSpec fine = coarse;
  fine.name = QStringLiteral("land_fine");
  fine.storageLevel = 3;
  fine.minLevel = 3;
  fine.maxLevel = -1;
  ts::FeatureTableSpec lines = fine;
  lines.name = QStringLiteral("rivers");
  lines.geometry = ts::FeatureTableSpec::Geometry::Lines;
  lines.attributes = {QStringLiteral("rank")};
  // Level 1 tile (1, 1, 0): 0°..90°E, 0°..90°N. Level 3 tile (3, 9, 1):
  // 22.5°..33.75°E, 67.5°..78.75°N.
  return writer.addTable(coarse) && writer.addTable(fine) &&
         writer.addTable(lines) &&
         writer.putFeature(coarse.name, ts::TileKey{1, 1, 0},
                           square(10.0, 10.0, 20.0)) &&
         writer.putFeature(fine.name, ts::TileKey{3, 9, 1},
                           square(23.0, 70.0, 2.0)) &&
         writer.putFeature(fine.name, ts::TileKey{3, 9, 1},
                           square(27.0, 70.0, 2.0)) &&
         writer.putFeature(lines.name, ts::TileKey{3, 9, 1},
                           Wkb().lineString({23.0, 68.0, 30.0, 75.0}).bytes(),
                           {4}) &&
         writer.finish();
}

} // namespace

/** Unit tests for ts/providers: test imagery, tile stores, GeoPackages. */
class TestProviders : public QObject
{
  Q_OBJECT

private slots:
  void solidColor()
  {
    ts::SolidColorImageryProvider provider(QColor(10, 20, 30, 128), 32, 9);
    QCOMPARE(provider.tileSizePx(), 32);
    QCOMPARE(provider.maximumLevel(), 9);
    const QImage image = provider.loadImage(ts::TileKey{3, 1, 2}, notCancelled);
    QCOMPARE(image.size(), QSize(32, 32));
    QCOMPARE(QColor::fromRgba(image.pixel(5, 7)), QColor(10, 20, 30, 128));
    // Beyond the deepest level, invalid keys and cancelled loads: nothing.
    QVERIFY(provider.loadImage(ts::TileKey{10, 0, 0}, notCancelled).isNull());
    QVERIFY(provider.loadImage(ts::TileKey{1, 9, 0}, notCancelled).isNull());
    const std::atomic<bool> cancelled{true};
    QVERIFY(provider.loadImage(ts::TileKey{1, 0, 0}, cancelled).isNull());
    QVERIFY(provider.id() !=
            ts::SolidColorImageryProvider(QColor(10, 20, 30)).id());
    QVERIFY(provider.name().contains(QStringLiteral("#800a141e")));
  }

  void checkerboard()
  {
    ts::CheckerboardImageryProvider provider(128);
    const ts::TileKey key{5, 37, 11};
    const QImage image = provider.loadImage(key, notCancelled);
    QCOMPARE(image.size(), QSize(128, 128));
    // Deterministic: the same key gives the same pixels.
    QCOMPARE(provider.loadImage(key, notCancelled), image);
    // Outlined, with a white-on-black label in the middle.
    QCOMPARE(image.pixel(0, 64), qRgba(40, 40, 40, 255));
    QCOMPARE(image.pixel(127, 5), qRgba(40, 40, 40, 255));
    QVERIFY(countColor(image, qRgba(255, 255, 255, 255)) > 20);
    QCOMPARE(image.pixel(64, 64 - 2 * 5 / 2 - 2), qRgba(0, 0, 0, 255));
    // Neighbouring squares differ; another level has other colours.
    QVERIFY(image.pixel(4, 4) != image.pixel(20, 4));
    const QImage deeper = provider.loadImage(key.child(0), notCancelled);
    QVERIFY(deeper.pixel(4, 4) != image.pixel(4, 4));
    QCOMPARE(provider.id(), ts::CheckerboardImageryProvider(128).id());
    QVERIFY(provider.id() != ts::CheckerboardImageryProvider(256).id());
  }

  void concurrentSharedProvider()
  {
    // Thread-safe: one provider loading on many threads gives the same
    // tiles as on one.
    ts::CheckerboardImageryProvider provider(64);
    const QImage expected =
        provider.loadImage(ts::TileKey{7, 100, 50}, notCancelled);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&provider, &expected](int thread) {
          for (int i = 0; i < 50; ++i) {
            const QImage other = provider.loadImage(
                ts::TileKey{7, 100 + thread, 50 + i}, notCancelled);
            if (other.isNull()) {
              return false;
            }
          }
          return provider.loadImage(ts::TileKey{7, 100, 50}, notCancelled) ==
                 expected;
        });
    QCOMPARE(failures, 0);
  }

  void sqliteTileStore()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tiles.sqlite"));
    ts::SqliteTileStoreInfo info;
    info.format = QStringLiteral("png");
    info.tileSizePx = 16;
    info.minLevel = 0;
    info.maxLevel = 2;
    info.name = QStringLiteral("Test 'tiles'");
    {
      ts::SqliteTileStoreWriter writer(path, info);
      QVERIFY2(writer.errorString().isEmpty(),
               qPrintable(writer.errorString()));
      for (int level = 0; level <= 2; ++level) {
        for (int y = 0; y < ts::TileKey::rows(level); ++y) {
          for (int x = 0; x < ts::TileKey::columns(level); ++x) {
            const ts::TileKey key{level, x, y};
            QVERIFY(writer.putTile(key, encodePng(keyedTile(key, 16))));
          }
        }
      }
      // A key stored twice is refused.
      QVERIFY(!writer.putTile(ts::TileKey{2, 7, 3}, QByteArrayLiteral("x")));
      QVERIFY(!writer.finish());
    }
    QVERIFY(!ts::SqliteTileImageryProvider(path).isValid()); // rolled back
    QVERIFY(QFile::remove(path));
    {
      ts::SqliteTileStoreWriter writer(path, info);
      for (int level = 0; level <= 2; ++level) {
        for (int y = 0; y < ts::TileKey::rows(level); ++y) {
          for (int x = 0; x < ts::TileKey::columns(level); ++x) {
            const ts::TileKey key{level, x, y};
            QVERIFY(writer.putTile(key, encodePng(keyedTile(key, 16))));
          }
        }
      }
      QVERIFY(writer.finish());
    }
    // The writer refuses an existing file.
    QVERIFY(!ts::SqliteTileStoreWriter(path, info).errorString().isEmpty());

    ts::SqliteTileImageryProvider provider(path);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    QCOMPARE(provider.name(), QStringLiteral("Test 'tiles'"));
    QCOMPARE(provider.maximumLevel(), 2);
    QCOMPARE(provider.tileSizePx(), 16);
    QVERIFY(provider.id().endsWith(QStringLiteral("tiles.sqlite")));
    for (const ts::TileKey key : {ts::TileKey{0, 1, 0}, ts::TileKey{2, 5, 3}}) {
      const QImage image = provider.loadImage(key, notCancelled);
      QCOMPARE(image.size(), QSize(16, 16));
      QCOMPARE(QColor(image.pixel(8, 8)), keyedTile(key, 16).pixelColor(8, 8));
    }
    // Deeper than stored, invalid, cancelled: nothing.
    QVERIFY(provider.loadImage(ts::TileKey{3, 0, 0}, notCancelled).isNull());
    QVERIFY(provider.loadImage(ts::TileKey{1, 9, 0}, notCancelled).isNull());
    const std::atomic<bool> cancelled{true};
    QVERIFY(provider.loadImage(ts::TileKey{1, 0, 0}, cancelled).isNull());
  }

  void sqliteTileStoreDamage()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tiles.sqlite"));
    ts::SqliteTileStoreInfo info;
    info.tileSizePx = 16;
    {
      ts::SqliteTileStoreWriter writer(path, info);
      QVERIFY(writer.putTile(ts::TileKey{0, 0, 0},
                             encodePng(keyedTile(ts::TileKey{0, 0, 0}, 16))));
      // A tile that does not decode: no data.
      QVERIFY(writer.putTile(ts::TileKey{0, 1, 0},
                             QByteArrayLiteral("\xff\xd8 broken jpeg")));
      QVERIFY(writer.finish());
    }
    {
      ts::SqliteTileImageryProvider provider(path);
      QVERIFY(provider.isValid());
      QVERIFY(!provider.loadImage(ts::TileKey{0, 0, 0}, notCancelled).isNull());
      QVERIFY(provider.loadImage(ts::TileKey{0, 1, 0}, notCancelled).isNull());
    }
    // Unusable metadata, a missing file, a file that is not a database.
    {
      ts::SqliteConnection connection(
          path, ts::SqliteConnection::OpenMode::ReadWrite);
      QVERIFY(connection.execute(QStringLiteral(
          "UPDATE metadata SET value = 'mercator' WHERE name = 'scheme'")));
    }
    QVERIFY(!ts::SqliteTileImageryProvider(path).isValid());
    QVERIFY(!ts::SqliteTileImageryProvider(dir.filePath(QStringLiteral("none")))
                 .isValid());
    const QString text = dir.filePath(QStringLiteral("text.sqlite"));
    QFile file(text);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'z'));
    file.close();
    QVERIFY(!ts::SqliteTileImageryProvider(text).isValid());
  }

  void sqliteConcurrentSharedProvider()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tiles.sqlite"));
    ts::SqliteTileStoreInfo info;
    info.format = QStringLiteral("png");
    info.tileSizePx = 16;
    info.maxLevel = 3;
    {
      ts::SqliteTileStoreWriter writer(path, info);
      for (int y = 0; y < ts::TileKey::rows(3); ++y) {
        for (int x = 0; x < ts::TileKey::columns(3); ++x) {
          QVERIFY(
              writer.putTile(ts::TileKey{3, x, y},
                             encodePng(keyedTile(ts::TileKey{3, x, y}, 16))));
        }
      }
      QVERIFY(writer.finish());
    }
    ts::SqliteTileImageryProvider provider(path);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&provider](int thread) {
          for (int i = 0; i < 64; ++i) {
            const ts::TileKey key{3, (thread * 3 + i) % 16, i % 8};
            const QImage image = provider.loadImage(key, notCancelled);
            if (image.isNull() || QColor(image.pixel(1, 1)) !=
                                      keyedTile(key, 16).pixelColor(1, 1)) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void geoPackageFeatures()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("features.gpkg"));
    QVERIFY(writeFeaturePackage(path));
    ts::GeoPackageFeatureProvider provider(path);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    QCOMPARE(provider.name(), QStringLiteral("features"));
    QCOMPARE(provider.maximumLevel(), 3);
    QCOMPARE(provider.tables().size(), std::size_t{3});

    // Level 0: no table serves it.
    QVERIFY(!provider.loadFeatures(ts::TileKey{0, 1, 0}, notCancelled));

    // Level 2 is served by the coarse table, stored at level 1.
    std::optional<ts::FeatureTile> tile =
        provider.loadFeatures(ts::TileKey{2, 2, 1}, notCancelled);
    if (!tile) {
      QFAIL("no tile");
    }
    QVERIFY(tile.value().key == (ts::TileKey{1, 1, 0}));
    QCOMPARE(tile.value().features.size(), std::size_t{1});
    const ts::Feature& coarse = tile.value().features[0];
    QCOMPARE(coarse.type, ts::Feature::Type::Polygon);
    QCOMPARE(coarse.parts.size(), std::size_t{1});
    QCOMPARE(coarse.parts[0].size(), std::size_t{5});
    QCOMPARE(coarse.parts[0][2].lonDeg, 30.0);
    QCOMPARE(coarse.parts[0][2].latDeg, 30.0);
    QCOMPARE(coarse.attributes.value(QStringLiteral("table")).toString(),
             QStringLiteral("land_coarse"));

    // Level 6 under (3, 9, 1): two polygons and the river, with its rank.
    tile = provider.loadFeatures(ts::TileKey{6, 75, 12}, notCancelled);
    if (!tile) {
      QFAIL("no tile");
    }
    QVERIFY(tile.value().key == (ts::TileKey{3, 9, 1}));
    QCOMPARE(tile.value().features.size(), std::size_t{3});
    int polygons = 0;
    for (const ts::Feature& feature : tile.value().features) {
      if (feature.type == ts::Feature::Type::LineStrings) {
        QCOMPARE(feature.attributes.value(QStringLiteral("rank")).toInt(), 4);
        QCOMPARE(feature.parts[0].size(), std::size_t{2});
      } else {
        polygons += 1;
      }
    }
    QCOMPARE(polygons, 2);

    // A tile with nothing stored: a tile without features.
    tile = provider.loadFeatures(ts::TileKey{3, 0, 0}, notCancelled);
    if (!tile) {
      QFAIL("no tile");
    }
    QVERIFY(tile.value().features.empty());

    QVERIFY(!ts::GeoPackageFeatureProvider(dir.filePath(QStringLiteral("x")))
                 .isValid());
  }

  void geoPackageWriterRefusesBadInput()
  {
    const QTemporaryDir dir;
    ts::GeoPackageFeatureWriter writer(
        dir.filePath(QStringLiteral("bad.gpkg")));
    ts::FeatureTableSpec spec;
    spec.name = QStringLiteral("drop table; --");
    QVERIFY(!writer.addTable(spec));
    ts::GeoPackageFeatureWriter other(dir.filePath(QStringLiteral("ok.gpkg")));
    spec.name = QStringLiteral("land");
    QVERIFY(other.addTable(spec));
    QVERIFY(!other.putFeature(QStringLiteral("missing"), ts::TileKey{},
                              square(0, 0, 1)));
  }

  void decodeGeometry()
  {
    // A polygon, a multi line string, an empty geometry and a 3D line.
    std::vector<ts::Feature> features;
    QVERIFY(ts::decodeGeoPackageGeometry(
        reinterpret_cast<const unsigned char*>(
            gpkgBlob(square(1, 2, 3)).constData()),
        static_cast<std::size_t>(gpkgBlob(square(1, 2, 3)).size()), &features));
    QCOMPARE(features.size(), std::size_t{1});
    QCOMPARE(features[0].parts[0][1].lonDeg, 4.0);

    const QByteArray multi = gpkgBlob(Wkb()
                                          .byte(1)
                                          .uint32(5)
                                          .uint32(2)
                                          .lineString({0, 0, 1, 1})
                                          .lineString({2, 2, 3, 3, 4, 4})
                                          .bytes());
    features.clear();
    QVERIFY(ts::decodeGeoPackageGeometry(
        reinterpret_cast<const unsigned char*>(multi.constData()),
        static_cast<std::size_t>(multi.size()), &features));
    QCOMPARE(features.size(), std::size_t{1});
    QCOMPARE(features[0].parts.size(), std::size_t{2});
    QCOMPARE(features[0].parts[1].size(), std::size_t{3});

    Wkb emptyHeader;
    emptyHeader.byte('G').byte('P').byte(0).byte(0x11).uint32(4326);
    const QByteArray empty =
        emptyHeader.bytes() + Wkb().byte(1).uint32(3).uint32(0).bytes();
    features.clear();
    QVERIFY(ts::decodeGeoPackageGeometry(
        reinterpret_cast<const unsigned char*>(empty.constData()),
        static_cast<std::size_t>(empty.size()), &features));
    QVERIFY(features.empty());

    Wkb line3d;
    line3d.byte(1).uint32(1002).uint32(2);
    line3d.float64(5).float64(6).float64(100).float64(7).float64(8).float64(
        200);
    const QByteArray z = gpkgBlob(line3d.bytes());
    QVERIFY(ts::decodeGeoPackageGeometry(
        reinterpret_cast<const unsigned char*>(z.constData()),
        static_cast<std::size_t>(z.size()), &features));
    QCOMPARE(features.back().parts[0][1].lonDeg, 7.0);
    QCOMPARE(features.back().parts[0][1].latDeg, 8.0);
  }

  void decodeDamagedGeometry()
  {
    // Every truncation and every single-byte change of a valid blob is
    // either decoded or refused, never read past its end (ASan checks).
    const QByteArray blob = gpkgBlob(Wkb()
                                         .byte(1)
                                         .uint32(6)
                                         .uint32(2)
                                         .polygon({0, 0, 1, 0, 1, 1})
                                         .polygon({5, 5, 6, 5, 6, 6, 5, 6})
                                         .bytes());
    std::vector<ts::Feature> features;
    for (int size = 0; size < blob.size(); ++size) {
      const QByteArray part = blob.left(size);
      features.clear();
      QVERIFY(!ts::decodeGeoPackageGeometry(
          reinterpret_cast<const unsigned char*>(part.constData()),
          static_cast<std::size_t>(part.size()), &features));
      QVERIFY(features.empty());
    }
    for (int i = 0; i < blob.size(); ++i) {
      for (const int value : {0x00, 0x7f, 0xff}) {
        QByteArray changed = blob;
        changed[i] = static_cast<char>(value);
        features.clear();
        ts::decodeGeoPackageGeometry(
            reinterpret_cast<const unsigned char*>(changed.constData()),
            static_cast<std::size_t>(changed.size()), &features);
      }
    }
    QVERIFY(!ts::decodeGeoPackageGeometry(nullptr, 0, &features));
  }

  void geoPackageConcurrentSharedProvider()
  {
    const QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("features.gpkg"));
    QVERIFY(writeFeaturePackage(path));
    ts::GeoPackageFeatureProvider provider(path);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&provider](int thread) {
          for (int i = 0; i < 100; ++i) {
            const ts::TileKey key = (i + thread) % 2 == 0
                                        ? ts::TileKey{5, 37, 5}
                                        : ts::TileKey{2, 2, 1};
            const std::optional<ts::FeatureTile> tile =
                provider.loadFeatures(key, notCancelled);
            const std::size_t expected = key.level == 5 ? 3 : 1;
            if (!tile || tile->features.size() != expected) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkCheckerboard()
  {
    ts::CheckerboardImageryProvider provider;
    QBENCHMARK
    {
      provider.loadImage(ts::TileKey{12, 2100, 700}, notCancelled);
    }
  }
};

QTEST_GUILESS_MAIN(TestProviders)
#include "tst_providers.moc"
