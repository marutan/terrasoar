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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#include "concurrency.h"
#include "mockhttpserver.h"
#include "mockxyz.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/providers/providerregistry.h"
#include "ts/regions/regiondownload.h"
#include "ts/regions/regiongeometry.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/xyz/xyzimageryprovider.h"
#include "ts/xyz/xyzsource.h"

namespace
{

using ts::test::MockHttpServer;
using ts::test::MockWmtsBehaviour;

constexpr double a = 6378137.0;
constexpr double piValue = 3.14159265358979323846;

ts::NetworkSettings testNetwork()
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.retryDelay = std::chrono::milliseconds(20);
  settings.attempts = 2;
  settings.timeout = std::chrono::milliseconds(5000);
  return settings;
}

/** A source on the mock @p server at @p path ("/xyz/…" or "/tms/…"). */
ts::XyzSource
mockSource(const MockHttpServer& server,
           const QString& path = QStringLiteral("/xyz/{z}/{x}/{y}.png"))
{
  ts::XyzSource source;
  source.urlTemplate = server.url(QStringLiteral("/")).toString() + path.mid(1);
  source.maximumZoom = ts::test::mockXyzMaximumZoom;
  source.title = QStringLiteral("Mock tiles");
  source.attribution = QStringLiteral("Terra Soar tests");
  return source;
}

/** Share of pixels differing by more than @p tolerance (premultiplied). */
double mismatch(const QImage& actual, const QImage& expected, int32_t tolerance)
{
  if (actual.size() != expected.size()) {
    return 1.0;
  }
  const QImage x = actual.convertToFormat(QImage::Format_ARGB32);
  const QImage y = expected.convertToFormat(QImage::Format_ARGB32);
  int64_t bad = 0;
  for (int32_t row = 0; row < x.height(); ++row) {
    for (int32_t column = 0; column < x.width(); ++column) {
      const QRgb p = x.pixel(column, row);
      const QRgb q = y.pixel(column, row);
      const int32_t alphaP = qAlpha(p);
      const int32_t alphaQ = qAlpha(q);
      const int32_t difference = std::max(
          {std::abs(qRed(p) * alphaP / 255 - qRed(q) * alphaQ / 255),
           std::abs(qGreen(p) * alphaP / 255 - qGreen(q) * alphaQ / 255),
           std::abs(qBlue(p) * alphaP / 255 - qBlue(q) * alphaQ / 255),
           std::abs(alphaP - alphaQ)});
      if (difference > tolerance) {
        ++bad;
      }
    }
  }
  return static_cast<double>(bad) / static_cast<double>(x.width() * x.height());
}

/**
 * The mock's picture of quadtree tile @p key as a Web Mercator image (rows
 * evenly spaced in northing), as the renderer expects XYZ tiles.
 */
QImage expectedTile(ts::TileKey key)
{
  const ts::GeoRect rect = key.bounds();
  const double north =
      a * std::log(std::tan(piValue / 4.0 + rect.northDeg * piValue / 360.0));
  const double south =
      a * std::log(std::tan(piValue / 4.0 + rect.southDeg * piValue / 360.0));
  QImage image(256, 256, QImage::Format_ARGB32);
  for (int32_t row = 0; row < 256; ++row) {
    const double y = north - (row + 0.5) * (north - south) / 256.0;
    const double lat = std::atan(std::sinh(y / a)) * 180.0 / piValue;
    for (int32_t column = 0; column < 256; ++column) {
      const double lon =
          rect.westDeg + (column + 0.5) * rect.widthDeg() / 256.0;
      image.setPixel(column, row, ts::test::mockWmsColour(lon, lat, true));
    }
  }
  return image;
}

} // namespace

/**
 * Unit tests for ts/xyz: templates, addresses, the tile matrices made for
 * them, the catalogue, and the provider against a mock tile server.
 */
class TestXyz : public QObject
{
  Q_OBJECT

private slots:
  void templates_data()
  {
    QTest::addColumn<QString>("urlTemplate");
    QTest::addColumn<QStringList>("subdomains");
    QTest::addColumn<bool>("usable");
    QTest::newRow("osm")
        << QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png")
        << QStringList() << true;
    QTest::newRow("row first")
        << QStringLiteral("https://example.org/MapServer/tile/{z}/{y}/{x}")
        << QStringList() << true;
    QTest::newRow("tms -y")
        << QStringLiteral("http://example.org/{z}/{x}/{-y}.jpg")
        << QStringList() << true;
    QTest::newRow("subdomains")
        << QStringLiteral("https://{s}.example.org/{z}/{x}/{y}.png")
        << QStringList{QStringLiteral("a"), QStringLiteral("b")} << true;
    QTest::newRow("upper case")
        << QStringLiteral("https://example.org/{Z}/{X}/{Y}.png")
        << QStringList() << true;
    QTest::newRow("subdomains missing")
        << QStringLiteral("https://{s}.example.org/{z}/{x}/{y}.png")
        << QStringList() << false;
    QTest::newRow("no y") << QStringLiteral("https://example.org/{z}/{x}.png")
                          << QStringList() << false;
    QTest::newRow("unknown variable")
        << QStringLiteral("https://example.org/{z}/{x}/{y}.png?key={apikey}")
        << QStringList() << false;
    QTest::newRow("not http")
        << QStringLiteral("ftp://example.org/{z}/{x}/{y}.png") << QStringList()
        << false;
    QTest::newRow("empty") << QString() << QStringList() << false;
  }

  void templates()
  {
    QFETCH(QString, urlTemplate);
    QFETCH(QStringList, subdomains);
    QFETCH(bool, usable);
    const QString problem = ts::checkXyzTemplate(urlTemplate, subdomains);
    QCOMPARE(problem.isEmpty(), usable);
  }

  void formats()
  {
    QCOMPARE(ts::xyzFormatFor(QStringLiteral("https://e.org/{z}/{x}/{y}.png")),
             QStringLiteral("image/png"));
    QCOMPARE(ts::xyzFormatFor(QStringLiteral("https://e.org/{z}/{x}/{y}.JPG")),
             QStringLiteral("image/jpeg"));
    QCOMPARE(
        ts::xyzFormatFor(QStringLiteral("https://e.org/{z}/{x}/{y}.jpeg?a=1")),
        QStringLiteral("image/jpeg"));
    QCOMPARE(ts::xyzFormatFor(QStringLiteral("https://e.org/{z}/{x}/{y}.webp")),
             QStringLiteral("image/webp"));
    QCOMPARE(ts::xyzFormatFor(QStringLiteral("https://e.org/tile/{z}/{y}/{x}")),
             QStringLiteral("image/png"));
  }

  void tileUrls()
  {
    ts::XyzSource source;
    source.urlTemplate =
        QStringLiteral("https://{s}.example.org/{z}/{x}/{y}.png?k=v");
    source.subdomains = QStringList{QStringLiteral("a"), QStringLiteral("b"),
                                    QStringLiteral("c")};
    QCOMPARE(ts::xyzTileUrl(source, 3, 4, 2).toString(),
             QStringLiteral("https://a.example.org/3/4/2.png?k=v"));
    // The same tile from the same subdomain, every time.
    QCOMPARE(ts::xyzTileUrl(source, 3, 4, 2), ts::xyzTileUrl(source, 3, 4, 2));
    QCOMPARE(ts::xyzTileUrl(source, 3, 4, 3).host(),
             QStringLiteral("b.example.org"));
    // TMS: rows from the south.
    source.tms = true;
    QCOMPARE(ts::xyzTileUrl(source, 3, 4, 2).path(),
             QStringLiteral("/3/4/5.png"));
    ts::XyzSource flipped;
    flipped.urlTemplate = QStringLiteral("https://example.org/{z}/{-y}/{x}");
    QCOMPARE(ts::xyzTileUrl(flipped, 0, 0, 0).path(), QStringLiteral("/0/0/0"));
    QCOMPARE(ts::xyzTileUrl(flipped, 10, 7, 0).path(),
             QStringLiteral("/10/1023/7"));
    ts::XyzSource upper;
    upper.urlTemplate = QStringLiteral("https://example.org/{Z}/{X}/{Y}");
    QCOMPARE(ts::xyzTileUrl(upper, 5, 6, 7).path(), QStringLiteral("/5/6/7"));
  }

  void serviceUrls()
  {
    QCOMPARE(ts::xyzServiceUrl(
                 QStringLiteral("https://{s}.tile.example.org:8443/{z}/{x}/"
                                "{y}.png"),
                 QStringList{QStringLiteral("b")})
                 .toString(),
             QStringLiteral("https://b.tile.example.org:8443/"));
    QCOMPARE(
        ts::xyzServiceUrl(
            QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
            {})
            .host(),
        QStringLiteral("tile.openstreetmap.org"));
  }

  void matrices()
  {
    ts::XyzSource source;
    source.urlTemplate = QStringLiteral("https://example.org/{z}/{x}/{y}.png");
    source.minimumZoom = 2;
    source.maximumZoom = 12;
    const ts::WmtsSource wmts = source.toWmtsSource();
    QCOMPARE(wmts.crs, QStringLiteral("EPSG:3857"));
    QCOMPARE(wmts.imageProjection(), ts::TileImageProjection::WebMercator);
    QCOMPARE(static_cast<int32_t>(wmts.matrices.size()), 11);
    QCOMPARE(wmts.matrices.front().identifier, QStringLiteral("2"));
    QCOMPARE(wmts.matrices.front().matrixWidth, int64_t{4});
    // Zoom 0's pixel: the equator over 256 pixels.
    QVERIFY(std::abs(wmts.matrices.front().pixelSize -
                     2.0 * piValue * a / 1024.0) < 1e-6);
    QCOMPARE(wmts.matrices.back().maxRow, int64_t{4095});
    // Quadtree level n is zoom n + 1 for 256-pixel tiles, zoom n for 512.
    QCOMPARE(wmts.maximumLevel, 11);
    source.tileSizePx = 512;
    QCOMPARE(source.toWmtsSource().maximumLevel, 12);
    QCOMPARE(source.toWmtsSource().matrices.front().tileWidth, 512);

    // Bounds limit the rows and columns: the UK at zoom 6.
    source.tileSizePx = 256;
    source.minimumZoom = 6;
    source.maximumZoom = 6;
    source.bounds = ts::GeoRect{-8.0, 49.9, 2.0, 60.9};
    const ts::WmtsMatrix uk = source.toWmtsSource().matrices.front();
    QCOMPARE(uk.minColumn, int64_t{30});
    QCOMPARE(uk.maxColumn, int64_t{32});
    QCOMPARE(uk.minRow, int64_t{18});
    QCOMPARE(uk.maxRow, int64_t{21});
  }

  void jsonRoundTrip()
  {
    ts::XyzSource source;
    source.urlTemplate =
        QStringLiteral("https://{s}.example.org/{z}/{x}/{y}.jpg");
    source.subdomains = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    source.tms = true;
    source.minimumZoom = 1;
    source.maximumZoom = 15;
    source.tileSizePx = 512;
    source.format = ts::xyzFormatFor(source.urlTemplate);
    source.bounds = ts::GeoRect{-10.0, 40.0, 10.0, 60.0};
    source.title = QStringLiteral("Example");
    source.attribution = QStringLiteral("© Example");
    source.service = QUrl(QStringLiteral("https://example.org/"));
    QString error;
    const std::optional<ts::XyzSource> back =
        ts::XyzSource::fromJson(source.toJson(), &error);
    if (!back) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(back->urlTemplate, source.urlTemplate);
    QCOMPARE(back->subdomains, source.subdomains);
    QCOMPARE(back->tms, true);
    QCOMPARE(back->minimumZoom, 1);
    QCOMPARE(back->maximumZoom, 15);
    QCOMPARE(back->tileSizePx, 512);
    QCOMPARE(back->format, QStringLiteral("image/jpeg"));
    QVERIFY(back->bounds.has_value());
    QCOMPARE(back->bounds.value_or(ts::GeoRect{}).northDeg, 60.0);
    QCOMPARE(back->title, source.title);
    QCOMPARE(back->attribution, source.attribution);
    QCOMPARE(back->service, source.service);
    QCOMPARE(back->id(), source.id());
    // The row order and the zooms are part of the id, the subdomains not.
    ts::XyzSource other = source;
    other.tms = false;
    QVERIFY(other.id() != source.id());
    other = source;
    other.subdomains.append(QStringLiteral("c"));
    QCOMPARE(other.id(), source.id());

    // Refused: not XYZ, an unusable template, zooms the wrong way round.
    QJsonObject json = source.toJson();
    json.insert(QStringLiteral("kind"), QStringLiteral("wmts"));
    QVERIFY(!ts::XyzSource::fromJson(json, &error));
    json = source.toJson();
    json.remove(QStringLiteral("subdomains"));
    QVERIFY(!ts::XyzSource::fromJson(json, &error));
    QVERIFY(error.contains(QLatin1String("{s}")));
    json = source.toJson();
    json.insert(QStringLiteral("minZoom"), 16);
    QVERIFY(!ts::XyzSource::fromJson(json, &error));
    json = source.toJson();
    json.insert(QStringLiteral("maxZoom"), 31);
    QVERIFY(!ts::XyzSource::fromJson(json, &error));
    json = source.toJson();
    json.insert(QStringLiteral("tileSize"), 8);
    QVERIFY(!ts::XyzSource::fromJson(json, &error));
  }

  void catalogue()
  {
    // The catalogue that comes with the program reads; every entry credits
    // its source and links its terms; OpenStreetMap's volunteer-run tile
    // servers and Google's unofficial one forbid bulk downloads.
    QFile file(QStringLiteral(TS_REPO_DATA_DIR "/xyz-catalogue.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray json = file.readAll();
    QString error;
    const std::optional<std::vector<ts::XyzCatalogueEntry>> entries =
        ts::parseXyzCatalogue(json, &error);
    if (!entries) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(entries->size() >= 4);
    std::vector<ts::WmsCatalogueEntry> services;
    for (const ts::XyzCatalogueEntry& entry : *entries) {
      QVERIFY(!entry.title.isEmpty());
      QVERIFY(!entry.attribution.isEmpty());
      QVERIFY(entry.termsUrl.isValid());
      QVERIFY(entry.urlTemplate.startsWith(QLatin1String("https://")));
      QVERIFY(entry.minimumZoom <= entry.maximumZoom);
      QVERIFY2(ts::XyzSource::fromJson(entry.source().toJson(), &error),
               qPrintable(error));
      services.push_back(entry.serviceEntry());
    }
    QCOMPARE(
        ts::bulkDownloadPolicy(
            QUrl(QStringLiteral("https://tile.openstreetmap.org/")), services),
        ts::BulkDownloadPolicy::Forbidden);
    // A subdomain's tiles are matched by the first subdomain's host.
    QCOMPARE(
        ts::bulkDownloadPolicy(
            QUrl(QStringLiteral("https://a.tile.opentopomap.org/")), services),
        ts::BulkDownloadPolicy::Forbidden);
    // Google's tile server isn't a published API: browsing only.
    QCOMPARE(ts::bulkDownloadPolicy(
                 QUrl(QStringLiteral("https://mt1.google.com/")), services),
             ts::BulkDownloadPolicy::Forbidden);

    // Not a catalogue, another kind, a newer version; unusable entries are
    // skipped.
    QVERIFY(!ts::parseXyzCatalogue("[]", &error));
    QFile wmts(QStringLiteral(TS_REPO_DATA_DIR "/wmts-catalogue.json"));
    QVERIFY(wmts.open(QIODevice::ReadOnly));
    QVERIFY(!ts::parseXyzCatalogue(wmts.readAll(), &error));
    QVERIFY(error.contains(QLatin1String("XYZ")));
    QVERIFY(!ts::parseXyzCatalogue(
        "{\"format\": \"terrasoar-xyz-catalogue\", \"version\": 2}", &error));
    const std::optional<std::vector<ts::XyzCatalogueEntry>> partly =
        ts::parseXyzCatalogue(
            "{\"format\": \"terrasoar-xyz-catalogue\", \"version\": 1, "
            "\"services\": ["
            "{\"title\": \"A\", \"template\": \"https://a/{z}/{x}\"},"
            "{\"title\": \"\", \"template\": \"https://b/{z}/{x}/{y}\"},"
            "{\"title\": \"C\", \"template\": \"https://c/{z}/{x}/{y}\", "
            "\"maxZoom\": 99}]}",
            &error);
    if (!partly) {
      QFAIL("partly is missing");
    }
    QCOMPARE(static_cast<int32_t>(partly->size()), 1);
    QCOMPARE(partly->front().maximumZoom, 30);
  }

  void providerDraws_data()
  {
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("tms");
    QTest::addColumn<QStringList>("subdomains");
    QTest::newRow("xyz") << QStringLiteral("/xyz/{z}/{x}/{y}.png") << false
                         << QStringList();
    QTest::newRow("tms flag")
        << QStringLiteral("/tms/{z}/{x}/{y}.png") << true << QStringList();
    QTest::newRow("tms -y")
        << QStringLiteral("/tms/{z}/{x}/{-y}.png") << false << QStringList();
    QTest::newRow("subdomains")
        << QStringLiteral("/{s}/xyz/{z}/{x}/{y}.png") << false
        << QStringList{QStringLiteral("a"), QStringLiteral("b")};
  }

  void providerDraws()
  {
    // Every way of addressing the same tiles draws the same Earth, as a Web
    // Mercator image: quadtree tile (2, 1, 1) from two zoom-3 tiles.
    QFETCH(QString, path);
    QFETCH(bool, tms);
    QFETCH(QStringList, subdomains);
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockXyzHandler(behaviour));
    ts::XyzSource source = mockSource(server, path);
    source.tms = tms;
    source.subdomains = subdomains;
    ts::XyzImageryProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    QCOMPARE(provider.id(), source.id());
    QCOMPARE(provider.name(), source.title);
    QCOMPARE(provider.imageProjection(), ts::TileImageProjection::WebMercator);
    QCOMPARE(provider.maximumLevel(), ts::test::mockXyzMaximumZoom - 1);
    const std::atomic<bool> cancelled{false};
    const ts::TileKey key{2, 1, 1};
    const ts::ImageLoadResult result = provider.loadTileImage(key, cancelled);
    QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Image);
    const double different = mismatch(result.image, expectedTile(key), 60);
    QVERIFY2(
        different <= 0.02,
        qPrintable(QStringLiteral("%1 of the pixels differ").arg(different)));
    const std::vector<ts::test::MockXyzTileRequest> tiles =
        ts::test::mockXyzTileRequests(server);
    QCOMPARE(static_cast<int32_t>(tiles.size()), 2);
    for (const ts::test::MockXyzTileRequest& tile : tiles) {
      QCOMPARE(tile.z, 3);
      QCOMPARE(tile.tms, path.startsWith(QLatin1String("/tms")));
      QCOMPARE(tile.subdomain.isEmpty(), subdomains.isEmpty());
    }
  }

  void providerCaches()
  {
    // A second provider with the same cache draws without asking; a source
    // with the other row order is another cache.
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    behaviour->wms.maxAgeS = 3600;
    MockHttpServer server;
    server.setHandler(ts::test::mockXyzHandler(behaviour));
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    const ts::XyzSource source = mockSource(server);
    const std::atomic<bool> cancelled{false};
    {
      ts::XyzImageryProvider provider(
          source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
      QCOMPARE(provider.loadTileImage(ts::TileKey{3, 4, 2}, cancelled).outcome,
               ts::ImageLoadResult::Outcome::Image);
    }
    const int32_t asked = server.requestCount();
    QVERIFY(asked > 0);
    ts::XyzImageryProvider again(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    QCOMPARE(again.loadTileImage(ts::TileKey{3, 4, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(again.statistics().requests, 0);
    QCOMPARE(server.requestCount(), asked);
    ts::XyzSource flipped =
        mockSource(server, QStringLiteral("/tms/{z}/{x}/{y}.png"));
    flipped.tms = true;
    QVERIFY(ts::webCacheFileName(flipped.id()) !=
            ts::webCacheFileName(source.id()));
  }

  void providerKeepsATileCancelledWhileArriving()
  {
    // A tile cancelled while it arrives (the camera moved on) finishes and
    // goes into the cache: its connection is kept, and coming back to it
    // costs no request.
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    behaviour->wms.maxAgeS = 3600;
    behaviour->wms.restDelayMs = 300;
    MockHttpServer server;
    server.setHandler(ts::test::mockXyzHandler(behaviour));
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    const ts::XyzSource source = mockSource(server);
    const ts::TileKey key{3, 4, 2};
    {
      ts::XyzImageryProvider provider(
          source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
      std::atomic<bool> cancelled{false};
      std::thread canceller([&cancelled]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancelled.store(true);
      });
      QCOMPARE(provider.loadTileImage(key, cancelled).outcome,
               ts::ImageLoadResult::Outcome::Cancelled);
      canceller.join();
    }
    QCOMPARE(server.cutOffCount(), 0);
    const int32_t asked = server.requestCount();
    QVERIFY(asked > 0);
    // What arrived is in the cache: the same tile again asks only for the
    // pieces never started.
    behaviour->wms.restDelayMs = 0;
    ts::XyzImageryProvider again(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    const std::atomic<bool> notCancelled{false};
    QCOMPARE(again.loadTileImage(key, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QVERIFY(again.statistics().cacheHits >= asked);
  }

  void providerOutcomes()
  {
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockXyzHandler(behaviour));
    ts::XyzSource source = mockSource(server);
    const std::atomic<bool> cancelled{false};
    ts::XyzImageryProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    // 404: nothing there; the status stays Ok.
    behaviour->notFound = true;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 1, 1}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);
    behaviour->notFound = false;
    // A server error: failed, to be asked again.
    behaviour->wms.failStatus = 500;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 1, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Error);
    behaviour->wms.failStatus = 0;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 1, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);

    // Outside the bounds: no data and nothing asked.
    source.bounds = ts::GeoRect{0.0, 0.0, 10.0, 10.0};
    ts::XyzImageryProvider bounded(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const int32_t before = server.requestCount();
    QCOMPARE(bounded.loadTileImage(ts::TileKey{4, 2, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
    QCOMPARE(server.requestCount(), before);

    // Coarser than the service's coarsest zoom and too many tiles: none.
    source.bounds.reset();
    source.minimumZoom = 8;
    ts::XyzImageryProvider deep(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    QCOMPARE(deep.loadTileImage(ts::TileKey{0, 0, 0}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
    QCOMPARE(server.requestCount(), before);
  }

  void regionDownloadThenOffline()
  {
    // A region downloaded through RegionDownload, then drawn with the
    // server gone; terms that forbid it are refused.
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    ts::XyzSource source;
    int64_t tiles = 0;
    {
      std::shared_ptr<MockWmtsBehaviour> behaviour =
          std::make_shared<MockWmtsBehaviour>();
      behaviour->wms.maxAgeS = 3600;
      MockHttpServer server;
      server.setHandler(ts::test::mockXyzHandler(behaviour));
      source = mockSource(server);
      std::shared_ptr<ts::XyzImageryProvider> provider =
          std::make_shared<ts::XyzImageryProvider>(
              source, std::make_shared<ts::NetworkClient>(testNetwork()),
              settings);
      QCOMPARE(provider->serviceUrl(),
               ts::xyzServiceUrl(source.urlTemplate, {}));
      const ts::RegionGeometry area =
          ts::RegionGeometry::rectangle(-5.0, 50.0, 2.0, 56.0);
      QString error;
      ts::RegionDownload download(provider, 11, area, 0, 6);
      QVERIFY(download.progress().totalTiles > 0);
      QVERIFY2(download.start({}, &error), qPrintable(error));
      QVERIFY(download.waitUntilSettled(std::chrono::seconds(60)));
      const ts::RegionDownloadProgress progress = download.progress();
      QCOMPARE(progress.state, ts::RegionDownloadProgress::State::Finished);
      QCOMPARE(progress.failedTiles, int64_t{0});
      QVERIFY(progress.downloadedTiles > 0);
      tiles = provider->diskCache()->regionUsage(11).tiles;
      QVERIFY(tiles > 0);

      ts::XyzCatalogueEntry entry;
      entry.title = QStringLiteral("Mock tiles");
      entry.urlTemplate = source.urlTemplate;
      entry.allowBulkDownload = false;
      const int32_t before = server.requestCount();
      ts::RegionDownload refused(provider, 12, area, 0, 3);
      QVERIFY(!refused.start({entry.serviceEntry()}, &error));
      QVERIFY(error.contains(QLatin1String("Mock tiles")));
      QCOMPARE(refused.progress().state,
               ts::RegionDownloadProgress::State::Refused);
      QCOMPARE(server.requestCount(), before);
    }
    ts::XyzImageryProvider offline(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    const std::atomic<bool> cancelled{false};
    const ts::TileKey london =
        ts::TileKey::containing(5, ts::Geodetic{51.5, -0.1, 0.0});
    QCOMPARE(offline.loadTileImage(london, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(offline.statistics().requests, 0);
  }

  void registryCreatesXyz()
  {
    ts::XyzSource source;
    source.urlTemplate =
        QStringLiteral("https://tile.example.org/{z}/{x}/{y}.png");
    source.attribution = QStringLiteral("© Example");
    ts::ProviderRegistry registry;
    registry.setWmsNetwork(std::make_shared<ts::NetworkClient>(testNetwork()),
                           ts::WebTileSettings{});
    QString error;
    const std::shared_ptr<ts::ImageryProvider> provider =
        registry.imagery(source.toJson(), &error);
    if (!provider) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(std::dynamic_pointer_cast<ts::XyzImageryProvider>(provider));
    QVERIFY(std::dynamic_pointer_cast<ts::RegionTileSource>(provider));
    QCOMPARE(provider->id(), source.id());
    QCOMPARE(registry.imagery(source.toJson(), &error), provider);
    QCOMPARE(registry.attribution(source.toJson()), source.attribution);
    QJsonObject broken = source.toJson();
    broken.insert(QStringLiteral("template"), QStringLiteral("https://x/{z}"));
    QVERIFY(!registry.imagery(broken, &error));
    QVERIFY(!error.isEmpty());
  }

  void concurrentOwnInstances()
  {
    // XyzSource is reentrant: each thread fills its own source's template.
    const int32_t failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [](int32_t thread) {
          ts::XyzSource source;
          source.urlTemplate =
              QStringLiteral("https://{s}.example.org/{z}/{x}/{y}.png");
          source.subdomains = QStringList{QStringLiteral("a")};
          for (int32_t i = 0; i < 200; ++i) {
            const QUrl url = ts::xyzTileUrl(source, 10, thread, i);
            if (url.path() !=
                QStringLiteral("/10/%1/%2.png").arg(thread).arg(i)) {
              return false;
            }
          }
          return source.toWmtsSource().matrices.size() == 20;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkTileUrl()
  {
    ts::XyzSource source;
    source.urlTemplate =
        QStringLiteral("https://{s}.example.org/{z}/{x}/{y}.png");
    source.subdomains = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    int64_t row = 0;
    QBENCHMARK
    {
      ++row;
      const QUrl url = ts::xyzTileUrl(source, 12, 2047, row % 4096);
      QVERIFY(url.isValid());
    }
  }
};

QTEST_GUILESS_MAIN(TestXyz)
#include "tst_xyz.moc"
