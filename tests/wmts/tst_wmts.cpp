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
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include "mockhttpserver.h"
#include "mockwmts.h"
#include "ts/geo/wgs84.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/providers/providerregistry.h"
#include "ts/regions/regiondownload.h"
#include "ts/regions/regiongeometry.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wmts/wmtscapabilities.h"
#include "ts/wmts/wmtsimageryprovider.h"
#include "ts/wmts/wmtssource.h"

namespace
{

using ts::test::MockHttpServer;
using ts::test::MockWmtsBehaviour;

constexpr double a = 6378137.0;
constexpr double piValue = 3.14159265358979323846;

QByteArray fixture(const QString& name)
{
  QFile file(QStringLiteral(TS_TEST_DATA_DIR "/wmts/") + name);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

ts::WmtsCapabilities capabilitiesOf(const char* name)
{
  QString error;
  const std::optional<ts::WmtsCapabilities> parsed =
      ts::parseWmtsCapabilities(fixture(QLatin1String(name)), &error);
  if (!parsed) {
    qWarning("%s: %s", name, qPrintable(error));
  }
  return parsed.value_or(ts::WmtsCapabilities{});
}

/** The mock server's capabilities, tiles at @p server (empty if broken). */
ts::WmtsCapabilities mockCapabilities(const QUrl& server)
{
  QString error;
  const std::optional<ts::WmtsCapabilities> parsed =
      ts::parseWmtsCapabilities(ts::test::mockWmtsCapabilities(server), &error);
  if (!parsed) {
    qWarning("mock capabilities: %s", qPrintable(error));
  }
  return parsed.value_or(ts::WmtsCapabilities{});
}

ts::NetworkSettings testNetwork()
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.retryDelay = std::chrono::milliseconds(20);
  settings.attempts = 2;
  settings.timeout = std::chrono::milliseconds(5000);
  return settings;
}

/** A geographic source lined up with the quadtree, ten matrices. */
ts::WmtsSource alignedSource()
{
  ts::WmtsSource source;
  source.url = QUrl(QStringLiteral("https://example.org/wmts"));
  source.layer = QStringLiteral("layer");
  source.style = QStringLiteral("default");
  source.tileMatrixSet = QStringLiteral("WorldCRS84Quad");
  source.crs = QStringLiteral("CRS:84");
  for (int32_t z = 0; z < 10; ++z) {
    ts::WmtsMatrix matrix;
    matrix.identifier = QString::number(z);
    matrix.pixelSize = 180.0 / 256.0 / std::ldexp(1.0, z);
    matrix.originX = -180.0;
    matrix.originY = 90.0;
    matrix.matrixWidth = int64_t{2} << z;
    matrix.matrixHeight = int64_t{1} << z;
    matrix.maxColumn = matrix.matrixWidth - 1;
    matrix.maxRow = matrix.matrixHeight - 1;
    source.matrices.push_back(matrix);
  }
  return source;
}

/**
 * Fraction of pixels of @p actual and @p expected differing by more than
 * @p tolerance.
 */
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
      // Compare premultiplied, so differences in invisible colour don't
      // count.
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
 * The mock's picture of quadtree tile @p key, as the renderer expects it:
 * rows evenly spaced in latitude, or in northing for @p mercator; on white
 * if @p opaque (JPEG).
 */
QImage expectedTile(ts::TileKey key, bool mercator, bool opaque)
{
  const ts::GeoRect rect = key.bounds();
  const double north =
      mercator ? a * std::log(std::tan(piValue / 4.0 +
                                       rect.northDeg * piValue / 360.0))
               : rect.northDeg;
  const double south =
      mercator ? a * std::log(std::tan(piValue / 4.0 +
                                       rect.southDeg * piValue / 360.0))
               : rect.southDeg;
  QImage image(256, 256, QImage::Format_ARGB32);
  for (int32_t row = 0; row < 256; ++row) {
    const double y = north - (row + 0.5) * (north - south) / 256.0;
    const double lat =
        mercator ? std::atan(std::sinh(y / a)) * 180.0 / piValue : y;
    for (int32_t column = 0; column < 256; ++column) {
      const double lon =
          rect.westDeg + (column + 0.5) * rect.widthDeg() / 256.0;
      image.setPixel(column, row, ts::test::mockWmsColour(lon, lat, !opaque));
    }
  }
  return image;
}

} // namespace

/**
 * Unit tests for WMTS: capabilities, tile matrices, requests and the
 * provider.
 */
class TestWmts : public QObject
{
  Q_OBJECT

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void realDocuments_data()
  {
    QTest::addColumn<QString>("file");
    QTest::addColumn<int32_t>("layers");
    QTest::addColumn<QString>("firstLayer");
    QTest::addColumn<QString>("set");
    QTest::addColumn<QString>("crs");
    QTest::addColumn<int32_t>("matrices");
    QTest::addColumn<bool>("kvp");
    QTest::addColumn<bool>("templates");
    QTest::newRow("basemap.at")
        << QStringLiteral("basemapat.xml") << 7
        << QStringLiteral("geolandbasemap") << QStringLiteral("google3857")
        << QStringLiteral("EPSG:3857") << 21 << false << true;
    QTest::newRow("EMODnet")
        << QStringLiteral("emodnet-bathymetry.xml") << 6
        << QStringLiteral("baselayer") << QStringLiteral("inspire_quad")
        << QStringLiteral("EPSG:4326") << 0 << false << true;
    QTest::newRow("GIBS 4326")
        << QStringLiteral("gibs-epsg4326.xml") << 12
        << QStringLiteral("MERRA2_2m_Air_Temperature_Monthly")
        << QStringLiteral("2km") << QStringLiteral("CRS:84") << 6 << true
        << true;
    QTest::newRow("GIBS 3857")
        << QStringLiteral("gibs-epsg3857.xml") << 12
        << QStringLiteral("MERRA2_2m_Air_Temperature_Monthly")
        << QStringLiteral("GoogleMapsCompatible_Level6")
        << QStringLiteral("EPSG:3857") << 7 << true << true;
    QTest::newRow("PDOK") << QStringLiteral("pdok.xml") << 5
                          << QStringLiteral("standaard")
                          << QStringLiteral("EPSG:28992")
                          << QStringLiteral("EPSG:28992") << 0 << true << true;
    QTest::newRow("swisstopo")
        << QStringLiteral("swisstopo.xml") << 8
        << QStringLiteral(
               "ch.agroscope.amphibien-ausbreitungskarten_alytes_obstetricans")
        << QStringLiteral("3857_17") << QStringLiteral("EPSG:3857") << 18
        << false << true;
    QTest::newRow("USGS ArcGIS")
        << QStringLiteral("usgs-arcgis.xml") << 1 << QStringLiteral("USGSTopo")
        << QStringLiteral("GoogleMapsCompatible") << QStringLiteral("EPSG:3857")
        << 19 << true << true;
  }

  void realDocuments()
  {
    // Every real document reads, with its layers and tile matrix sets.
    QFETCH(QString, file);
    QFETCH(int32_t, layers);
    QFETCH(QString, firstLayer);
    QFETCH(QString, set);
    QFETCH(QString, crs);
    QFETCH(int32_t, matrices);
    QFETCH(bool, kvp);
    QFETCH(bool, templates);
    QString error;
    const std::optional<ts::WmtsCapabilities> capabilities =
        ts::parseWmtsCapabilities(fixture(file), &error);
    if (!capabilities) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(static_cast<int32_t>(capabilities->layers.size()), layers);
    QCOMPARE(capabilities->layers.front().identifier, firstLayer);
    QVERIFY(!capabilities->title.isEmpty());
    const ts::WmtsTileMatrixSet* found = capabilities->findTileMatrixSet(set);
    if (found == nullptr) {
      QFAIL("tile matrix set missing");
    }
    QCOMPARE(ts::normalisedWmtsCrs(found->supportedCrs), crs);
    if (matrices > 0) {
      QCOMPARE(static_cast<int32_t>(found->matrices.size()), matrices);
    }
    QCOMPARE(!capabilities->getTileKvpUrl.isEmpty(), kvp);
    const bool anyTemplate = std::any_of(
        capabilities->layers.begin(), capabilities->layers.end(),
        [](const ts::WmtsLayer& layer) { return !layer.resourceUrls.empty(); });
    QCOMPARE(anyTemplate, templates);
    for (const ts::WmtsLayer& layer : capabilities->layers) {
      QVERIFY(!layer.identifier.isEmpty());
      QVERIFY(!layer.tileMatrixSetLinks.empty());
      // Only tile templates are kept (GIBS also lists "Domains" ones).
      for (const ts::WmtsResourceUrl& resource : layer.resourceUrls) {
        QCOMPARE(resource.resourceType.toLower(), QStringLiteral("tile"));
      }
    }
  }

  void realDocumentDetails()
  {
    // basemap.at: two RESTful hosts per layer; Austria's bounds.
    const ts::WmtsCapabilities basemap = capabilitiesOf("basemapat.xml");
    const ts::WmtsLayer* geoland =
        basemap.findLayer(QStringLiteral("geolandbasemap"));
    if (geoland == nullptr) {
      QFAIL("geolandbasemap missing");
    }
    QVERIFY(geoland->resourceUrls.size() >= 2);
    if (!geoland->wgs84Bounds) {
      QFAIL("no bounds");
    }
    QCOMPARE(geoland->wgs84Bounds->westDeg, 8.782379);
    QCOMPARE(geoland->wgs84Bounds->northDeg, 49.037872);
    // Its Get addresses are all marked RESTful: no KVP.
    QVERIFY(basemap.getTileKvpUrl.isEmpty());

    // GIBS: the KVP address (attributes in single quotes, after a
    // RESTful one), and a Time dimension with its default.
    const ts::WmtsCapabilities gibs = capabilitiesOf("gibs-epsg4326.xml");
    QCOMPARE(
        gibs.getTileKvpUrl,
        QUrl(QStringLiteral(
            "https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/wmts.cgi?")));
    const ts::WmtsLayer& merra = gibs.layers.front();
    QCOMPARE(static_cast<int32_t>(merra.dimensions.size()), 1);
    QCOMPARE(merra.dimensions.front().identifier, QStringLiteral("Time"));
    QCOMPARE(merra.dimensions.front().defaultValue,
             QStringLiteral("2026-08-01"));
    QVERIFY(!merra.dimensions.front().values.isEmpty());

    // EMODnet: limits on some layers' links (16 matrices of 16), and
    // inspire_quad's corner latitude first.
    const ts::WmtsCapabilities emodnet =
        capabilitiesOf("emodnet-bathymetry.xml");
    const ts::WmtsLayer* atlas =
        emodnet.findLayer(QStringLiteral("mean_atlas_land_2020"));
    if (atlas == nullptr) {
      QFAIL("mean_atlas_land_2020 missing");
    }
    const ts::WmtsTileMatrixSetLink* link =
        atlas->findLink(QStringLiteral("inspire_quad"));
    if (link == nullptr) {
      QFAIL("inspire_quad link missing");
    }
    QCOMPARE(static_cast<int32_t>(link->limits.size()), 16);
    QString sourceError;
    const std::optional<ts::WmtsSource> limitedSource = ts::wmtsSourceFor(
        emodnet, QStringLiteral("mean_atlas_land_2020"),
        ts::WmtsChoices{
            QString(), QString(), QStringLiteral("inspire_quad"), {}},
        &sourceError);
    if (!limitedSource) {
      QFAIL(qPrintable(sourceError));
    }
    QCOMPARE(static_cast<int32_t>(limitedSource->matrices.size()), 16);
    const ts::WmtsLayer* baselayer =
        emodnet.findLayer(QStringLiteral("baselayer"));
    if (baselayer == nullptr) {
      QFAIL("baselayer missing");
    }
    const ts::WmtsTileMatrixSetLink* open =
        baselayer->findLink(QStringLiteral("inspire_quad"));
    if (open == nullptr) {
      QFAIL("baselayer's inspire_quad link missing");
    }
    QVERIFY(open->limits.empty());
    const ts::WmtsTileMatrixSet* quad =
        emodnet.findTileMatrixSet(QStringLiteral("inspire_quad"));
    if (quad == nullptr) {
      QFAIL("inspire_quad missing");
    }
    QCOMPARE(quad->matrices.front().topLeftFirst, 90.0);
    QCOMPARE(quad->matrices.front().topLeftSecond, -180.0);

    // PDOK: matrix identifiers with leading zeros are kept as written.
    const ts::WmtsCapabilities pdok = capabilitiesOf("pdok.xml");
    const ts::WmtsTileMatrixSet* rd =
        pdok.findTileMatrixSet(QStringLiteral("EPSG:28992"));
    if (rd == nullptr) {
      QFAIL("EPSG:28992 missing");
    }
    QCOMPARE(rd->matrices.front().identifier, QStringLiteral("00"));
  }

  void syntheticDocument()
  {
    const ts::WmtsCapabilities capabilities =
        capabilitiesOf("synthetic-kvp-limits.xml");
    QCOMPARE(capabilities.title, QStringLiteral("Synthetic KVP service"));
    QCOMPARE(capabilities.fees, QStringLiteral("none"));
    // Both AccessConstraints, joined.
    QCOMPARE(capabilities.accessConstraints,
             QStringLiteral("Attribution required. No bulk downloads."));
    QCOMPARE(capabilities.providerName,
             QStringLiteral("Example Mapping Agency"));
    QCOMPARE(capabilities.providerSite,
             QUrl(QStringLiteral("https://maps.example.org/")));
    // The KVP Get, though a RESTful one is listed first.
    QCOMPARE(capabilities.getTileKvpUrl,
             QUrl(QStringLiteral("https://maps.example.org/wmts?map=base&")));
    // The layer without an identifier is dropped.
    QCOMPARE(static_cast<int32_t>(capabilities.layers.size()), 3);

    const ts::WmtsLayer* limited =
        capabilities.findLayer(QStringLiteral("limited"));
    if (limited == nullptr) {
      QFAIL("limited missing");
    }
    QCOMPARE(limited->title, QStringLiteral("Limited"));
    QCOMPARE(limited->defaultStyle(), QStringLiteral("night"));
    QCOMPARE(limited->styles.at(1).legendUrl,
             QUrl(QStringLiteral("https://maps.example.org/legend/night.png")));
    QCOMPARE(limited->formats, QStringList({QStringLiteral("image/jpeg"),
                                            QStringLiteral("image/png")}));
    QCOMPARE(limited->dimensions.front().unitOfMeasure, QStringLiteral("m"));
    QCOMPARE(limited->dimensions.front().values.size(), 3);
    // The FeatureInfo template is ignored.
    QVERIFY(limited->resourceUrls.empty());
    // The broken limits entry is dropped.
    QCOMPARE(
        static_cast<int32_t>(limited->tileMatrixSetLinks.front().limits.size()),
        2);

    // The broken matrix is dropped; the rest kept in document order.
    const ts::WmtsTileMatrixSet* lonFirst =
        capabilities.findTileMatrixSet(QStringLiteral("LonFirst4326"));
    if (lonFirst == nullptr) {
      QFAIL("LonFirst4326 missing");
    }
    QCOMPARE(static_cast<int32_t>(lonFirst->matrices.size()), 3);
    QCOMPARE(lonFirst->matrices.front().identifier, QStringLiteral("2"));
    const ts::WmtsTileMatrixSet* mercator =
        capabilities.findTileMatrixSet(QStringLiteral("Mercator900913"));
    if (mercator == nullptr) {
      QFAIL("Mercator900913 missing");
    }
    QCOMPARE(mercator->wellKnownScaleSet,
             QStringLiteral("urn:ogc:def:wkss:OGC:1.0:GoogleMapsCompatible"));
  }

  void notCapabilities_data()
  {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("message");
    QTest::newRow("exception")
        << QStringLiteral("exception.xml")
        << QStringLiteral("The server reported an error: The value of the "
                          "service parameter \"wmts\" is invalid");
    QTest::newRow("html") << QStringLiteral("error-page.html")
                          << QStringLiteral("web page");
    QTest::newRow("truncated")
        << QStringLiteral("truncated.xml") << QStringLiteral("not valid XML");
    QTest::newRow("wms") << QStringLiteral("../wms/terrestris-130.xml")
                         << QStringLiteral("Add WMS Layer");
    QTest::newRow("empty") << QStringLiteral("missing-file")
                           << QStringLiteral("empty");
  }

  void notCapabilities()
  {
    // What isn't WMTS capabilities gives a sentence, not an XML error.
    QFETCH(QString, file);
    QFETCH(QString, message);
    QString error;
    QVERIFY(!ts::parseWmtsCapabilities(fixture(file), &error));
    QVERIFY2(error.contains(message), qPrintable(error));
  }

  void crsNormalisation_data()
  {
    QTest::addColumn<QString>("written");
    QTest::addColumn<QString>("normalised");
    QTest::newRow("CRS84 URN")
        << QStringLiteral("urn:ogc:def:crs:OGC:1.3:CRS84")
        << QStringLiteral("CRS:84");
    QTest::newRow("CRS84 URN 2") << QStringLiteral("urn:ogc:def:crs:OGC::CRS84")
                                 << QStringLiteral("CRS:84");
    QTest::newRow("CRS84 URL")
        << QStringLiteral("http://www.opengis.net/def/crs/OGC/1.3/CRS84")
        << QStringLiteral("CRS:84");
    QTest::newRow("4326 URN") << QStringLiteral("urn:ogc:def:crs:EPSG::4326")
                              << QStringLiteral("EPSG:4326");
    QTest::newRow("4326 URL")
        << QStringLiteral("http://www.opengis.net/def/crs/EPSG/0/4326")
        << QStringLiteral("EPSG:4326");
    QTest::newRow("3857 versioned")
        << QStringLiteral("urn:ogc:def:crs:EPSG:6.18.3:3857")
        << QStringLiteral("EPSG:3857");
    QTest::newRow("3857 GIBS")
        << QStringLiteral("urn:ogc:def:crs:EPSG:6.18:3:3857")
        << QStringLiteral("EPSG:3857");
    QTest::newRow("900913")
        << QStringLiteral("EPSG:900913") << QStringLiteral("EPSG:3857");
    QTest::newRow("102100")
        << QStringLiteral(" epsg:102100 ") << QStringLiteral("EPSG:3857");
    QTest::newRow("27700") << QStringLiteral("urn:ogc:def:crs:EPSG::27700")
                           << QStringLiteral("EPSG:27700");
    QTest::newRow("unknown")
        << QStringLiteral("urn:ogc:def:crs:IGNF::LAMB93") << QString();
  }

  void crsNormalisation()
  {
    QFETCH(QString, written);
    QFETCH(QString, normalised);
    QCOMPARE(ts::normalisedWmtsCrs(written), normalised);
  }

  void pixelSizes()
  {
    // Scale denominator to resolution: degrees and metres.
    ts::WmtsTileMatrix quad;
    quad.scaleDenominator = 279541132.0143589;
    QCOMPARE(ts::wmtsPixelSize(quad, QStringLiteral("CRS:84")), 0.703125);
    QCOMPARE(ts::wmtsPixelSize(quad, QStringLiteral("EPSG:4326")), 0.703125);
    ts::WmtsTileMatrix google;
    google.scaleDenominator = 559082264.0287178;
    QVERIFY(std::abs(ts::wmtsPixelSize(google, QStringLiteral("EPSG:3857")) -
                     156543.03392804097) < 1e-6);
  }

  void cornerAxisOrder()
  {
    // EPSG:4326 is latitude first; CRS:84 and Web Mercator x first; a
    // longitude written first in EPSG:4326 is recognised.
    ts::WmtsTileMatrix matrix;
    matrix.scaleDenominator = 279541132.0143589;
    matrix.topLeftFirst = 90.0;
    matrix.topLeftSecond = -180.0;
    ts::WmtsMatrix usable = ts::wmtsMatrix(matrix, QStringLiteral("EPSG:4326"));
    QCOMPARE(usable.originX, -180.0);
    QCOMPARE(usable.originY, 90.0);
    matrix.topLeftFirst = -180.0;
    matrix.topLeftSecond = 90.0;
    usable = ts::wmtsMatrix(matrix, QStringLiteral("EPSG:4326"));
    QCOMPARE(usable.originX, -180.0);
    QCOMPARE(usable.originY, 90.0);
    usable = ts::wmtsMatrix(matrix, QStringLiteral("CRS:84"));
    QCOMPARE(usable.originX, -180.0);
    QCOMPARE(usable.originY, 90.0);
    matrix.topLeftFirst = -20037508.342789244;
    matrix.topLeftSecond = 20037508.342789244;
    usable = ts::wmtsMatrix(matrix, QStringLiteral("EPSG:3857"));
    QCOMPARE(usable.originX, -20037508.342789244);
    QCOMPARE(usable.originY, 20037508.342789244);
  }

  void alignedGeographicWindow()
  {
    // WorldCRS84Quad: quadtree tile (z, x, y) is WMTS tile (z, x, y).
    const ts::WmtsSource source = alignedSource();
    for (const ts::TileKey key : {ts::TileKey{0, 1, 0}, ts::TileKey{3, 5, 2},
                                  ts::TileKey{9, 700, 300}}) {
      const int32_t matrix = ts::wmtsMatrixFor(source, key);
      QCOMPARE(matrix, key.level);
      const std::optional<ts::WmtsTileWindow> window =
          ts::wmtsTileWindow(source, key, matrix);
      if (!window) {
        QFAIL("no window");
      }
      QCOMPARE(window->firstColumn, static_cast<int64_t>(key.x));
      QCOMPARE(window->firstRow, static_cast<int64_t>(key.y));
      QCOMPARE(window->columns, 1);
      QCOMPARE(window->rows, 1);
      QVERIFY(std::abs(window->left) < 1e-6);
      QVERIFY(std::abs(window->top) < 1e-6);
      QVERIFY(std::abs(window->width - 256.0) < 1e-6);
      QVERIFY(std::abs(window->height - 256.0) < 1e-6);
    }
    // Deeper than the finest matrix: the finest, a part of one tile.
    const ts::TileKey deep{11, 2800, 1200};
    QCOMPARE(ts::wmtsMatrixFor(source, deep), 9);
    const std::optional<ts::WmtsTileWindow> window =
        ts::wmtsTileWindow(source, deep, 9);
    if (!window) {
      QFAIL("no window");
    }
    QCOMPARE(window->columns, 1);
    QCOMPARE(window->firstColumn, int64_t{700});
    QCOMPARE(window->firstRow, int64_t{300});
    QVERIFY(std::abs(window->width - 64.0) < 1e-6);
    QVERIFY(std::abs(window->left - 0.0) < 1e-6);
  }

  void webMercatorWindow()
  {
    // GoogleMapsCompatible zoom z + 1 has the pixels of quadtree level z.
    // Tile (2, 1, 1): 135° W to 90° W, 0° to 45° N. Hand-computed: x from
    // pixel 256 to 512 of zoom 3 (column 1), y from 736.717 to 1024
    // (rows 2 and 3).
    ts::WmtsSource source;
    source.url = QUrl(QStringLiteral("https://example.org/wmts"));
    source.crs = QStringLiteral("EPSG:3857");
    for (int32_t z = 0; z < 6; ++z) {
      ts::WmtsMatrix matrix;
      matrix.identifier = QString::number(z);
      matrix.pixelSize = 2.0 * piValue * a / 256.0 / std::ldexp(1.0, z);
      matrix.originX = -piValue * a;
      matrix.originY = piValue * a;
      matrix.matrixWidth = int64_t{1} << z;
      matrix.matrixHeight = int64_t{1} << z;
      matrix.maxColumn = matrix.matrixWidth - 1;
      matrix.maxRow = matrix.matrixHeight - 1;
      source.matrices.push_back(matrix);
    }
    QCOMPARE(source.imageProjection(), ts::TileImageProjection::WebMercator);
    const ts::TileKey key{2, 1, 1};
    QCOMPARE(ts::wmtsMatrixFor(source, key), 3);
    const std::optional<ts::WmtsTileWindow> window =
        ts::wmtsTileWindow(source, key, 3);
    if (!window) {
      QFAIL("no window");
    }
    QCOMPARE(window->firstColumn, int64_t{1});
    QCOMPARE(window->columns, 1);
    QCOMPARE(window->firstRow, int64_t{2});
    QCOMPARE(window->rows, 2);
    QVERIFY(std::abs(window->left) < 1e-6);
    QVERIFY(std::abs(window->top - 224.71687560233988) < 1e-6);
    QVERIFY(std::abs(window->width - 256.0) < 1e-6);
    QVERIFY(std::abs(window->height - 287.2831243976601) < 1e-6);
    // Wholly beyond the Mercator limit (87.2° N to the pole) there is
    // nothing; partly beyond, the part inside.
    QVERIFY(!ts::wmtsTileWindow(source, ts::TileKey{6, 10, 0}, 5));
    QVERIFY(ts::wmtsTileWindow(source, ts::TileKey{5, 10, 0}, 5));
  }

  void unalignedWindow()
  {
    // 200-pixel tiles from (185° W, 95° N), the quadtree's pixel size:
    // tile (1, 1, 0) (90° W to 0°, 0° to 90° N) takes four of them. By
    // hand: x from 270.22 to 526.22 (columns 1, 2), y from 14.22 to 270.22
    // (rows 0, 1).
    ts::WmtsSource source;
    source.url = QUrl(QStringLiteral("https://example.org/wmts"));
    source.crs = QStringLiteral("EPSG:4326");
    for (int32_t z = 0; z < 4; ++z) {
      ts::WmtsMatrix matrix;
      matrix.identifier = QString::number(z);
      matrix.pixelSize = 180.0 / 256.0 / std::ldexp(1.0, z);
      matrix.originX = -185.0;
      matrix.originY = 95.0;
      matrix.tileWidth = 200;
      matrix.tileHeight = 200;
      matrix.matrixWidth = int64_t{3} << z;
      matrix.matrixHeight = int64_t{2} << z;
      matrix.maxColumn = matrix.matrixWidth - 1;
      matrix.maxRow = matrix.matrixHeight - 1;
      source.matrices.push_back(matrix);
    }
    const std::optional<ts::WmtsTileWindow> window =
        ts::wmtsTileWindow(source, ts::TileKey{1, 1, 0}, 1);
    if (!window) {
      QFAIL("no window");
    }
    QCOMPARE(window->firstColumn, int64_t{1});
    QCOMPARE(window->columns, 2);
    QCOMPARE(window->firstRow, int64_t{0});
    QCOMPARE(window->rows, 2);
    QVERIFY(std::abs(window->left - 70.22222222222223) < 1e-6);
    QVERIFY(std::abs(window->top - 14.222222222222221) < 1e-6);
    QVERIFY(std::abs(window->width - 256.0) < 1e-6);
    QVERIFY(std::abs(window->height - 256.0) < 1e-6);
  }

  void limitsAndBounds()
  {
    // From the synthetic document: limits allow only matrices 1 and 2,
    // within their rows and columns, and the layer's bounds.
    const ts::WmtsCapabilities capabilities =
        capabilitiesOf("synthetic-kvp-limits.xml");
    QString error;
    const std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        capabilities, QStringLiteral("limited"), ts::WmtsChoices{}, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(static_cast<int32_t>(source->matrices.size()), 2);
    QCOMPARE(source->matrices.at(0).identifier, QStringLiteral("1"));
    QCOMPARE(source->matrices.at(0).minColumn, int64_t{1});
    QCOMPARE(source->matrices.at(0).maxColumn, int64_t{2});
    QCOMPARE(source->matrices.at(1).identifier, QStringLiteral("2"));
    // The corner written longitude first, read right.
    QCOMPARE(source->matrices.at(0).originX, -180.0);
    QCOMPARE(source->matrices.at(0).originY, 90.0);
    // Level 0 is drawn from matrix 1 (the coarsest left): two tiles.
    const ts::TileKey west{0, 0, 0};
    QCOMPARE(ts::wmtsMatrixFor(*source, west), 0);
    // Outside the bounds (−10° to 20°, 40° to 60° N): nothing.
    QVERIFY(!ts::wmtsTileWindow(*source, ts::TileKey{3, 2, 1}, 1));
    // Inside, but in a column the limits leave out: nothing either.
    QVERIFY(ts::wmtsTileExists(*source, ts::WmtsTileIndex{1, 3, 1}));
    QVERIFY(!ts::wmtsTileExists(*source, ts::WmtsTileIndex{1, 2, 1}));
    QVERIFY(!ts::wmtsTileExists(*source, ts::WmtsTileIndex{1, 5, 0}));
    QVERIFY(ts::wmtsTileWindow(*source, ts::TileKey{2, 3, 0}, 1));
  }

  void requestUrls()
  {
    // KVP: the base URL's own items kept, every parameter set, dimension
    // values added.
    ts::WmtsSource source = alignedSource();
    source.url = QUrl(
        QStringLiteral("https://maps.example.org/wmts?map=base&SERVICE=WMS"));
    source.style = QStringLiteral("night");
    source.format = QStringLiteral("image/png");
    source.dimensions = {
        {QStringLiteral("elevation"), QStringLiteral("500")},
        {QStringLiteral("Time"), QStringLiteral("2026-01-01T00:00:00+01:00")}};
    QCOMPARE(ts::getTileUrl(source, ts::WmtsTileIndex{3, 5, 2})
                 .toString(QUrl::FullyEncoded),
             QStringLiteral(
                 "https://maps.example.org/wmts?map=base&SERVICE=WMTS"
                 "&REQUEST=GetTile&VERSION=1.0.0&LAYER=layer&STYLE=night"
                 "&FORMAT=image/png&TILEMATRIXSET=WorldCRS84Quad&TILEMATRIX=3"
                 "&TILEROW=2&TILECOL=5&elevation=500"
                 "&Time=2026-01-01T00:00:00%2B01:00"));
    // RESTful: variables matched without regard to case, values encoded,
    // unknown variables left as they are.
    source.urlTemplate = QStringLiteral(
        "https://tiles.example.org/{style}/{Time}/{TILEMATRIXSET}/"
        "{TileMatrix}/{TileRow}/{TileCol}.png?e={elevation}&k={Key}");
    QCOMPARE(ts::getTileUrl(source, ts::WmtsTileIndex{3, 5, 2})
                 .toString(QUrl::FullyEncoded),
             QStringLiteral("https://tiles.example.org/night/"
                            "2026-01-01T00%3A00%3A00%2B01%3A00/WorldCRS84Quad/"
                            "3/2/5.png?e=500&k=%7BKey%7D"));
  }

  void capabilitiesAddress()
  {
    QCOMPARE(ts::wmtsCapabilitiesUrl(
                 QUrl(QStringLiteral("https://basemap.at/wmts/1.0.0/"
                                     "WMTSCapabilities.xml"))),
             QUrl(QStringLiteral(
                 "https://basemap.at/wmts/1.0.0/WMTSCapabilities.xml")));
    QCOMPARE(ts::wmtsCapabilitiesUrl(QUrl(QStringLiteral(
                 "https://service.pdok.nl/x?request=GetCapabilities"
                 "&service=WMTS"))),
             QUrl(QStringLiteral("https://service.pdok.nl/x?request="
                                 "GetCapabilities&service=WMTS")));
    QCOMPARE(ts::wmtsCapabilitiesUrl(
                 QUrl(QStringLiteral("https://maps.example.org/wmts?map=base"
                                     "&LAYER=x"))),
             QUrl(QStringLiteral("https://maps.example.org/wmts?map=base"
                                 "&SERVICE=WMTS&REQUEST=GetCapabilities"
                                 "&VERSION=1.0.0")));
  }

  void sourcesFromRealDocuments()
  {
    QString error;
    // GIBS: the template with {Time}, the default time in it.
    const std::optional<ts::WmtsSource> gibs =
        ts::wmtsSourceFor(capabilitiesOf("gibs-epsg4326.xml"),
                          QStringLiteral("MERRA2_2m_Air_Temperature_Monthly"),
                          ts::WmtsChoices{}, &error);
    if (!gibs) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(gibs->urlTemplate.contains(QLatin1String("{Time}")));
    QCOMPARE(gibs->dimensions.size(), std::size_t{1});
    QCOMPARE(gibs->dimensions.front().second, QStringLiteral("2026-08-01"));
    QCOMPARE(gibs->crs, QStringLiteral("CRS:84"));
    QVERIFY(ts::getTileUrl(*gibs, ts::WmtsTileIndex{0, 0, 0})
                .toString()
                .contains(QLatin1String("/2026-08-01/2km/")));
    // GIBS 2km: 512-pixel tiles of 0.5625°; level 0's west tile is part
    // of the first, which reaches past the world's edge.
    QCOMPARE(gibs->matrices.front().pixelSize, 0.5625);
    const std::optional<ts::WmtsTileWindow> world =
        ts::wmtsTileWindow(*gibs, ts::TileKey{0, 0, 0}, 0);
    if (!world) {
      QFAIL("no world");
    }
    QCOMPARE(world->columns, 1);
    QCOMPARE(world->rows, 1);
    QVERIFY(std::abs(world->width - 320.0) < 1e-6);

    // A chosen time replaces the default, and changes the id.
    const std::optional<ts::WmtsSource> later =
        ts::wmtsSourceFor(capabilitiesOf("gibs-epsg4326.xml"),
                          QStringLiteral("MERRA2_2m_Air_Temperature_Monthly"),
                          ts::WmtsChoices{QString(),
                                          QString(),
                                          QString(),
                                          {{QStringLiteral("time"),
                                            QStringLiteral("2025-01-01")}}},
                          &error);
    if (!later) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(later->dimensions.front().second, QStringLiteral("2025-01-01"));
    QVERIFY(later->id() != gibs->id());

    // EMODnet: the geographic set preferred to Web Mercator; its limits.
    const std::optional<ts::WmtsSource> emodnet = ts::wmtsSourceFor(
        capabilitiesOf("emodnet-bathymetry.xml"), QStringLiteral("baselayer"),
        ts::WmtsChoices{}, &error);
    if (!emodnet) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(emodnet->tileMatrixSet, QStringLiteral("inspire_quad"));
    QCOMPARE(emodnet->imageProjection(), ts::TileImageProjection::Geographic);
    QCOMPARE(ts::usableTileMatrixSets(capabilitiesOf("emodnet-bathymetry.xml"),
                                      QStringLiteral("baselayer"))
                 .value(0),
             QStringLiteral("inspire_quad"));

    // PDOK: only its Web Mercator set can be drawn (RD New comes in 10c).
    const std::optional<ts::WmtsSource> pdok = ts::wmtsSourceFor(
        capabilitiesOf("pdok.xml"), QStringLiteral("standaard"),
        ts::WmtsChoices{}, &error);
    if (!pdok) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(pdok->tileMatrixSet, QStringLiteral("EPSG:3857"));
    QCOMPARE(pdok->imageProjection(), ts::TileImageProjection::WebMercator);

    // USGS: "image/jpgpng", the only format offered, is used.
    const std::optional<ts::WmtsSource> usgs = ts::wmtsSourceFor(
        capabilitiesOf("usgs-arcgis.xml"), QStringLiteral("USGSTopo"),
        ts::WmtsChoices{
            QString(), QString(), QStringLiteral("GoogleMapsCompatible"), {}},
        &error);
    if (!usgs) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(usgs->format, QStringLiteral("image/jpgpng"));
    QVERIFY(!usgs->urlTemplate.isEmpty());

    // basemap.at: RESTful, attributed to the provider or the service.
    const std::optional<ts::WmtsSource> basemap = ts::wmtsSourceFor(
        capabilitiesOf("basemapat.xml"), QStringLiteral("geolandbasemap"),
        ts::WmtsChoices{}, &error);
    if (!basemap) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(basemap->urlTemplate.startsWith(QLatin1String("https://maps")));
    QVERIFY(!basemap->attribution.isEmpty());
    QVERIFY(basemap->maximumLevel >= 15);

    // The synthetic document's KVP-only layer: the default style, PNG,
    // the elevation's default.
    const std::optional<ts::WmtsSource> limited =
        ts::wmtsSourceFor(capabilitiesOf("synthetic-kvp-limits.xml"),
                          QStringLiteral("limited"), ts::WmtsChoices{}, &error);
    if (!limited) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(limited->style, QStringLiteral("night"));
    QCOMPARE(limited->format, QStringLiteral("image/png"));
    QVERIFY(limited->urlTemplate.isEmpty());
    QCOMPARE(limited->url,
             QUrl(QStringLiteral("https://maps.example.org/wmts?map=base&")));
    QCOMPARE(limited->attribution, QStringLiteral("Example Mapping Agency"));
    // Web Mercator under EPSG:900913, through a link to a missing set.
    const std::optional<ts::WmtsSource> odd = ts::wmtsSourceFor(
        capabilitiesOf("synthetic-kvp-limits.xml"), QStringLiteral("oddlinks"),
        ts::WmtsChoices{}, &error);
    if (!odd) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(odd->crs, QStringLiteral("EPSG:3857"));
    QCOMPARE(odd->style, QStringLiteral("default"));
    QCOMPARE(odd->format, QStringLiteral("image/png8"));
  }

  void sourceErrors_data()
  {
    QTest::addColumn<QString>("layer");
    QTest::addColumn<QString>("style");
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("set");
    QTest::addColumn<QString>("message");
    QTest::newRow("no layer")
        << QStringLiteral("nothing") << QString() << QString() << QString()
        << QStringLiteral("has no layer nothing");
    QTest::newRow("national grid only")
        << QStringLiteral("gridonly") << QString() << QString() << QString()
        << QStringLiteral("urn:ogc:def:crs:EPSG::27700");
    QTest::newRow("no style")
        << QStringLiteral("limited") << QStringLiteral("day") << QString()
        << QString() << QStringLiteral("no style day");
    QTest::newRow("no format")
        << QStringLiteral("limited") << QString()
        << QStringLiteral("image/webp") << QString()
        << QStringLiteral("can't be requested as image/webp");
    QTest::newRow("unusable set")
        << QStringLiteral("gridonly") << QString() << QString()
        << QStringLiteral("BritishNationalGrid")
        << QStringLiteral("can't be drawn yet");
  }

  void sourceErrors()
  {
    QFETCH(QString, layer);
    QFETCH(QString, style);
    QFETCH(QString, format);
    QFETCH(QString, set);
    QFETCH(QString, message);
    QString error;
    QVERIFY(!ts::wmtsSourceFor(capabilitiesOf("synthetic-kvp-limits.xml"),
                               layer, ts::WmtsChoices{style, format, set, {}},
                               &error));
    QVERIFY2(error.contains(message), qPrintable(error));
  }

  void jsonRoundTrip()
  {
    QString error;
    const std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        capabilitiesOf("synthetic-kvp-limits.xml"), QStringLiteral("limited"),
        ts::WmtsChoices{
            QString(),
            QString(),
            QString(),
            {{QStringLiteral("elevation"), QStringLiteral("1000")}}},
        &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    const QJsonObject json = source->toJson();
    QCOMPARE(json.value(QStringLiteral("kind")).toString(),
             QStringLiteral("wmts"));
    const std::optional<ts::WmtsSource> back =
        ts::WmtsSource::fromJson(json, &error);
    if (!back) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(back->id(), source->id());
    QCOMPARE(back->matrices.size(), source->matrices.size());
    QCOMPARE(back->matrices.back().maxColumn,
             source->matrices.back().maxColumn);
    QCOMPARE(back->dimensions, source->dimensions);
    QCOMPARE(back->maximumLevel, source->maximumLevel);
    QVERIFY(back->bounds.has_value());
    QCOMPARE(back->attribution, source->attribution);
    QCOMPARE(back->toJson(), json);

    // What makes no sense is refused with a reason.
    QJsonObject broken = json;
    broken.insert(QStringLiteral("crs"), QStringLiteral("EPSG:27700"));
    QVERIFY(!ts::WmtsSource::fromJson(broken, &error));
    QVERIFY(error.contains(QLatin1String("27700")));
    broken = json;
    broken.remove(QStringLiteral("url"));
    QVERIFY(!ts::WmtsSource::fromJson(broken, &error));
    broken = json;
    broken.insert(
        QStringLiteral("matrices"),
        QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("0")}}});
    QVERIFY(!ts::WmtsSource::fromJson(broken, &error));
    broken = json;
    broken.insert(QStringLiteral("kind"), QStringLiteral("wms"));
    QVERIFY(!ts::WmtsSource::fromJson(broken, &error));
  }

  void providerDrawsEachSet_data()
  {
    QTest::addColumn<QString>("set");
    QTest::addColumn<QString>("format");
    QTest::addColumn<int32_t>("level");
    QTest::addColumn<int32_t>("x");
    QTest::addColumn<int32_t>("y");
    QTest::addColumn<int32_t>("requests");
    QTest::addColumn<double>("worst");
    // Lined up: one request, the tile itself.
    QTest::newRow("WorldCRS84Quad REST")
        << QStringLiteral("WorldCRS84Quad") << QString() << 3 << 9 << 2 << 1
        << 0.0;
    QTest::newRow("WorldCRS84Quad KVP JPEG")
        << QStringLiteral("WorldCRS84Quad") << QStringLiteral("image/jpeg") << 3
        << 9 << 2 << 1 << 0.01;
    // Web Mercator: two tiles (rows), cut and scaled; edges blur a little.
    QTest::newRow("GoogleMapsCompatible")
        << QStringLiteral("GoogleMapsCompatible") << QString() << 2 << 1 << 1
        << 2 << 0.02;
    // Not lined up: four tiles combined.
    QTest::newRow("Offset4326") << QStringLiteral("Offset4326") << QString()
                                << 1 << 1 << 0 << 4 << 0.02;
  }

  void providerDrawsEachSet()
  {
    // Every set draws the same Earth: the tile's picture, in the layout
    // the renderer expects, from the expected number of requests.
    QFETCH(QString, set);
    QFETCH(QString, format);
    QFETCH(int32_t, level);
    QFETCH(int32_t, x);
    QFETCH(int32_t, y);
    QFETCH(int32_t, requests);
    QFETCH(double, worst);
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
    QString error;
    const std::optional<ts::WmtsCapabilities> capabilities =
        ts::parseWmtsCapabilities(ts::test::mockWmtsCapabilities(server.url()),
                                  &error);
    if (!capabilities) {
      QFAIL(qPrintable(error));
    }
    const std::optional<ts::WmtsSource> source =
        ts::wmtsSourceFor(*capabilities, QStringLiteral("pattern"),
                          ts::WmtsChoices{QString(), format, set, {}}, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(source->urlTemplate.isEmpty(), !format.isEmpty());
    ts::WmtsImageryProvider provider(
        *source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const std::atomic<bool> cancelled{false};
    const ts::TileKey key{level, x, y};
    const ts::ImageLoadResult result = provider.loadTileImage(key, cancelled);
    QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(result.image.size(), QSize(256, 256));
    const bool mercator = set == QLatin1String("GoogleMapsCompatible");
    QCOMPARE(provider.imageProjection(),
             mercator ? ts::TileImageProjection::WebMercator
                      : ts::TileImageProjection::Geographic);
    const double different = mismatch(
        result.image,
        expectedTile(key, mercator, format == QLatin1String("image/jpeg")), 60);
    QVERIFY2(
        different <= worst + 1e-9,
        qPrintable(QStringLiteral("%1 of the pixels differ").arg(different)));
    const std::vector<ts::test::MockWmtsTileRequest> tiles =
        ts::test::mockWmtsTileRequests(server);
    QCOMPARE(static_cast<int32_t>(tiles.size()), requests);
    for (const ts::test::MockWmtsTileRequest& tile : tiles) {
      QCOMPARE(tile.time, QStringLiteral("2026-01-01"));
    }
  }

  void providerCachesWmtsTiles()
  {
    // Tiles are kept by their own key: a second provider with the same
    // cache serves them without asking, and quadtree tiles sharing a WMTS
    // tile fetch it once.
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    behaviour->wms.maxAgeS = 3600;
    MockHttpServer server;
    server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    QString error;
    const std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        mockCapabilities(server.url()), QStringLiteral("pattern"),
        ts::WmtsChoices{QString(), QString(), QStringLiteral("Offset4326"), {}},
        &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    const std::atomic<bool> cancelled{false};
    {
      ts::WmtsImageryProvider provider(
          *source, std::make_shared<ts::NetworkClient>(testNetwork()),
          settings);
      // Two neighbours: (1, 1, 0) takes columns 1–2, (1, 2, 0) columns
      // 2–3 of rows 0–1, so they share two tiles.
      QCOMPARE(provider.loadTileImage(ts::TileKey{1, 1, 0}, cancelled).outcome,
               ts::ImageLoadResult::Outcome::Image);
      QCOMPARE(provider.loadTileImage(ts::TileKey{1, 2, 0}, cancelled).outcome,
               ts::ImageLoadResult::Outcome::Image);
      QCOMPARE(
          static_cast<int32_t>(ts::test::mockWmtsTileRequests(server).size()),
          6);
      QCOMPARE(provider.statistics().cacheHits, 2);
    }
    ts::WmtsImageryProvider again(
        *source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    QCOMPARE(again.loadTileImage(ts::TileKey{1, 1, 0}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(
        static_cast<int32_t>(ts::test::mockWmtsTileRequests(server).size()), 6);
    QCOMPARE(again.statistics().requests, 0);

    // Another time is another source: other requests, another cache.
    const std::optional<ts::WmtsSource> later = ts::wmtsSourceFor(
        mockCapabilities(server.url()), QStringLiteral("pattern"),
        ts::WmtsChoices{
            QString(),
            QString(),
            QStringLiteral("Offset4326"),
            {{QStringLiteral("Time"), QStringLiteral("2026-02-01")}}},
        &error);
    if (!later) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(later->id() != source->id());
    QVERIFY(ts::webCacheFileName(later->id()) !=
            ts::webCacheFileName(source->id()));
    ts::WmtsImageryProvider other(
        *later, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    QCOMPARE(other.loadTileImage(ts::TileKey{1, 1, 0}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    const std::vector<ts::test::MockWmtsTileRequest> tiles =
        ts::test::mockWmtsTileRequests(server);
    QCOMPARE(static_cast<int32_t>(tiles.size()), 10);
    QCOMPARE(tiles.back().time, QStringLiteral("2026-02-01"));
  }

  void providerOutcomes()
  {
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
    QString error;
    std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        mockCapabilities(server.url()), QStringLiteral("pattern"),
        ts::WmtsChoices{
            QString(), QString(), QStringLiteral("WorldCRS84Quad"), {}},
        &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    const std::atomic<bool> cancelled{false};
    ts::WmtsImageryProvider provider(
        *source, std::make_shared<ts::NetworkClient>(testNetwork()));

    // 404: the service has nothing there, for good; the status stays Ok.
    behaviour->notFound = true;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 1, 1}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);
    behaviour->notFound = false;

    // A server error, an exception, a web page, far too much: failed (to
    // be asked again), and the layer's status says why.
    behaviour->wms.failStatus = 500;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 1, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Error);
    behaviour->wms.failStatus = 0;
    behaviour->wms.serviceException = true;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 2, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QVERIFY2(provider.status().message.contains(QLatin1String("not defined")),
             qPrintable(provider.status().message));
    behaviour->wms.serviceException = false;
    behaviour->wms.html = true;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 3, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QVERIFY(provider.status().message.contains(QLatin1String("text/html")));
    behaviour->wms.html = false;
    behaviour->wms.oversized = true;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 4, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    behaviour->wms.oversized = false;
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 5, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);

    // Outside the layer's bounds: no data, and nothing asked.
    source->bounds = ts::GeoRect{0.0, 0.0, 10.0, 10.0};
    ts::WmtsImageryProvider bounded(
        *source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const int32_t before = server.requestCount();
    QCOMPARE(bounded.loadTileImage(ts::TileKey{4, 2, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
    QCOMPARE(server.requestCount(), before);
    QCOMPARE(bounded.dataBounds().value_or(ts::GeoRect{}).eastDeg, 10.0);
  }

  void providerOffline()
  {
    // Nobody listening: failed, and the layer is offline.
    QString error;
    MockHttpServer server;
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
    std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        mockCapabilities(QUrl(QStringLiteral("http://127.0.0.1:9"))),
        QStringLiteral("pattern"),
        ts::WmtsChoices{
            QString(), QString(), QStringLiteral("WorldCRS84Quad"), {}},
        &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    ts::NetworkSettings network = testNetwork();
    network.attempts = 1;
    ts::WmtsImageryProvider provider(
        *source, std::make_shared<ts::NetworkClient>(network));
    const std::atomic<bool> cancelled{false};
    QCOMPARE(provider.loadTileImage(ts::TileKey{2, 1, 1}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Offline);
  }

  void providerCancels()
  {
    // A tile leaving the view is given up at once.
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    behaviour->wms.delayMs = 3000;
    MockHttpServer server;
    server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
    QString error;
    const std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        mockCapabilities(server.url()), QStringLiteral("pattern"),
        ts::WmtsChoices{QString(), QString(), QStringLiteral("Offset4326"), {}},
        &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    ts::WmtsImageryProvider provider(
        *source, std::make_shared<ts::NetworkClient>(testNetwork()));
    std::atomic<bool> cancelled{false};
    std::thread canceller([&cancelled]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      cancelled = true;
    });
    QElapsedTimer clock;
    clock.start();
    const ts::ImageLoadResult result =
        provider.loadTileImage(ts::TileKey{1, 1, 0}, cancelled);
    canceller.join();
    QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Cancelled);
    QVERIFY2(clock.elapsed() < 2000,
             qPrintable(QString::number(clock.elapsed())));
  }

  void catalogues()
  {
    // The WMTS catalogue that comes with the program reads, and each kind
    // of catalogue refuses the other's file: programs from before WMTS
    // fetch the WMS one and must never see a WMTS service in it.
    QFile file(QStringLiteral(TS_SOURCE_DIR "/data/wmts-catalogue.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray wmtsJson = file.readAll();
    QString error;
    const std::optional<std::vector<ts::WmsCatalogueEntry>> entries =
        ts::parseWmtsCatalogue(wmtsJson, &error);
    if (!entries) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(entries->size() >= 5);
    for (const ts::WmsCatalogueEntry& entry : *entries) {
      QVERIFY(!entry.title.isEmpty());
      QVERIFY(!entry.attribution.isEmpty());
      QVERIFY(entry.termsUrl.isValid());
      QCOMPARE(entry.url.scheme(), QStringLiteral("https"));
      // Every address is one the dialog can ask for capabilities.
      QVERIFY(ts::wmtsCapabilitiesUrl(entry.url).isValid());
    }
    // swisstopo's fair-use terms ask for its download service instead.
    QCOMPARE(ts::bulkDownloadPolicy(
                 QUrl(QStringLiteral("https://wmts.geo.admin.ch/EPSG/3857/"
                                     "1.0.0/WMTSCapabilities.xml")),
                 *entries),
             ts::BulkDownloadPolicy::Forbidden);
    QVERIFY(!ts::parseWmsCatalogue(wmtsJson, &error));
    QFile wms(QStringLiteral(TS_SOURCE_DIR "/data/wms-catalogue.json"));
    QVERIFY(wms.open(QIODevice::ReadOnly));
    QVERIFY(!ts::parseWmtsCatalogue(wms.readAll(), &error));
    QVERIFY(error.contains(QLatin1String("WMTS")));
  }

  void serviceAddress()
  {
    // The address for the catalogue's terms: the one the user connected
    // to, else the KVP address, else the template's server.
    ts::WmtsSource source = alignedSource();
    const std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(testNetwork());
    QCOMPARE(ts::WmtsImageryProvider(source, network).serviceUrl(),
             QUrl(QStringLiteral("https://example.org/wmts")));
    source.url = QUrl();
    source.urlTemplate = QStringLiteral(
        "https://tiles.example.org/a/{TileMatrix}/{TileRow}/{TileCol}.png");
    QCOMPARE(ts::WmtsImageryProvider(source, network).serviceUrl(),
             QUrl(QStringLiteral("https://tiles.example.org")));
    source.service =
        QUrl(QStringLiteral("https://tiles.example.org/WMTSCapabilities.xml"));
    QCOMPARE(ts::WmtsImageryProvider(source, network).serviceUrl(),
             source.service);
    // Kept in projects, but not part of the id (the same tiles).
    QString error;
    const std::optional<ts::WmtsSource> back =
        ts::WmtsSource::fromJson(source.toJson(), &error);
    if (!back) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(back->service, source.service);
    ts::WmtsSource other = source;
    other.service = QUrl();
    QCOMPARE(other.id(), source.id());
    QVERIFY(ts::WmtsImageryProvider(source, network).typicalTileBytes() > 0);
  }

  void fetchForRegion()
  {
    // A region keeps the WMTS tiles each quadtree tile is made from: one
    // when lined up, four from the unaligned grid; stored ones are only
    // pinned; failures and no data are told apart.
    std::shared_ptr<MockWmtsBehaviour> behaviour =
        std::make_shared<MockWmtsBehaviour>();
    behaviour->wms.maxAgeS = 3600;
    MockHttpServer server;
    server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    const std::atomic<bool> cancelled{false};
    QString error;
    using Outcome = ts::RegionTileResult::Outcome;

    const std::optional<ts::WmtsSource> aligned = ts::wmtsSourceFor(
        mockCapabilities(server.url()), QStringLiteral("pattern"),
        ts::WmtsChoices{
            QString(), QString(), QStringLiteral("WorldCRS84Quad"), {}},
        &error);
    if (!aligned) {
      QFAIL(qPrintable(error));
    }
    ts::WmtsImageryProvider quad(
        *aligned, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    ts::RegionTileResult result =
        quad.fetchForRegion(ts::TileKey{3, 9, 2}, 7, cancelled);
    QCOMPARE(result.outcome, Outcome::Downloaded);
    QVERIFY(result.bytes > 0);
    if (quad.diskCache() == nullptr) {
      QFAIL("no disk cache");
    }
    QCOMPARE(quad.diskCache()->regionUsage(7).tiles, int64_t{1});
    result = quad.fetchForRegion(ts::TileKey{3, 9, 2}, 7, cancelled);
    QCOMPARE(result.outcome, Outcome::AlreadyCached);
    QCOMPARE(
        static_cast<int32_t>(ts::test::mockWmtsTileRequests(server).size()), 1);
    // The tile is drawn from what the region stored.
    QCOMPARE(quad.loadTileImage(ts::TileKey{3, 9, 2}, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(
        static_cast<int32_t>(ts::test::mockWmtsTileRequests(server).size()), 1);

    const std::optional<ts::WmtsSource> offset = ts::wmtsSourceFor(
        mockCapabilities(server.url()), QStringLiteral("pattern"),
        ts::WmtsChoices{QString(), QString(), QStringLiteral("Offset4326"), {}},
        &error);
    if (!offset) {
      QFAIL(qPrintable(error));
    }
    ts::WmtsImageryProvider grid(
        *offset, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    result = grid.fetchForRegion(ts::TileKey{1, 1, 0}, 8, cancelled);
    QCOMPARE(result.outcome, Outcome::Downloaded);
    if (grid.diskCache() == nullptr) {
      QFAIL("no disk cache");
    }
    QCOMPARE(grid.diskCache()->regionUsage(8).tiles, int64_t{4});

    // Outside the bounds: nothing asked, no data.
    ts::WmtsSource bounded = *aligned;
    bounded.bounds = ts::GeoRect{0.0, 0.0, 10.0, 10.0};
    ts::WmtsImageryProvider small(
        bounded, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    QCOMPARE(small.fetchForRegion(ts::TileKey{4, 2, 2}, 9, cancelled).outcome,
             Outcome::NoData);
    // 404: no data. A server error: failed, with its reason.
    behaviour->notFound = true;
    QCOMPARE(quad.fetchForRegion(ts::TileKey{3, 10, 2}, 7, cancelled).outcome,
             Outcome::NoData);
    behaviour->notFound = false;
    behaviour->wms.failStatus = 500;
    result = quad.fetchForRegion(ts::TileKey{3, 11, 2}, 7, cancelled);
    QCOMPARE(result.outcome, Outcome::Failed);
    QVERIFY(!result.errorString.isEmpty());
    QVERIFY(!result.unreachable);
    behaviour->wms.failStatus = 0;
    // Without a disk cache there is nowhere to keep them.
    ts::WmtsImageryProvider uncached(
        *aligned, std::make_shared<ts::NetworkClient>(testNetwork()));
    result = uncached.fetchForRegion(ts::TileKey{3, 9, 3}, 7, cancelled);
    QCOMPARE(result.outcome, Outcome::Failed);
    QVERIFY(result.errorString.contains(QLatin1String("disk cache")));
  }

  void regionDownloadThenOffline()
  {
    // A whole region downloaded through RegionDownload, then drawn with
    // the server gone; a service whose terms forbid it is refused.
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    std::optional<ts::WmtsSource> source;
    int64_t tiles = 0;
    {
      std::shared_ptr<MockWmtsBehaviour> behaviour =
          std::make_shared<MockWmtsBehaviour>();
      behaviour->wms.maxAgeS = 3600;
      MockHttpServer server;
      server.setHandler(ts::test::mockWmtsHandler(behaviour, server.url()));
      QString error;
      source = ts::wmtsSourceFor(
          mockCapabilities(server.url()), QStringLiteral("pattern"),
          ts::WmtsChoices{
              QString(), QString(), QStringLiteral("GoogleMapsCompatible"), {}},
          &error);
      if (!source) {
        QFAIL(qPrintable(error));
      }
      source->service = server.url(QStringLiteral("/WMTSCapabilities.xml"));
      std::shared_ptr<ts::WmtsImageryProvider> provider =
          std::make_shared<ts::WmtsImageryProvider>(
              *source, std::make_shared<ts::NetworkClient>(testNetwork()),
              settings);
      const ts::RegionGeometry area =
          ts::RegionGeometry::rectangle(-5.0, 50.0, 2.0, 56.0);
      ts::RegionDownload download(provider, 11, area, 0, 6);
      QVERIFY(download.progress().totalTiles > 0);
      QVERIFY2(download.start({}, &error), qPrintable(error));
      QVERIFY(download.waitUntilSettled(std::chrono::seconds(60)));
      const ts::RegionDownloadProgress progress = download.progress();
      QCOMPARE(progress.state, ts::RegionDownloadProgress::State::Finished);
      QCOMPARE(progress.failedTiles, int64_t{0});
      QVERIFY(progress.downloadedTiles > 0);
      QVERIFY(progress.bytes > 0);
      tiles = provider->diskCache()->regionUsage(11).tiles;
      QVERIFY(tiles > 0);

      // Terms that forbid it: refused before anything is asked.
      ts::WmsCatalogueEntry entry;
      entry.title = QStringLiteral("Mock tiles");
      entry.url = source->service;
      entry.allowBulkDownload = false;
      const int32_t before = server.requestCount();
      ts::RegionDownload refused(provider, 12, area, 0, 3);
      QVERIFY(!refused.start({entry}, &error));
      QVERIFY(error.contains(QLatin1String("Mock tiles")));
      QCOMPARE(refused.progress().state,
               ts::RegionDownloadProgress::State::Refused);
      QCOMPARE(server.requestCount(), before);
    }
    // The server is gone: the region's tiles are drawn from the disk.
    ts::WmtsImageryProvider offline(
        *source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    const std::atomic<bool> cancelled{false};
    const ts::TileKey london =
        ts::TileKey::containing(5, ts::Geodetic{51.5, -0.1, 0.0});
    QCOMPARE(offline.loadTileImage(london, cancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(offline.statistics().requests, 0);
    QCOMPARE(offline.diskCache()->regionUsage(11).tiles, tiles);
  }

  void registryCreatesWmts()
  {
    // A "wmts" source in a layer definition becomes a WmtsImageryProvider,
    // shared, with its attribution.
    QString error;
    const std::optional<ts::WmtsSource> source = ts::wmtsSourceFor(
        capabilitiesOf("basemapat.xml"), QStringLiteral("geolandbasemap"),
        ts::WmtsChoices{}, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    ts::ProviderRegistry registry;
    registry.setWmsNetwork(std::make_shared<ts::NetworkClient>(testNetwork()),
                           ts::WebTileSettings{});
    const std::shared_ptr<ts::ImageryProvider> provider =
        registry.imagery(source->toJson(), &error);
    if (!provider) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(std::dynamic_pointer_cast<ts::WmtsImageryProvider>(provider));
    QCOMPARE(provider->id(), source->id());
    QCOMPARE(registry.imagery(source->toJson(), &error), provider);
    QCOMPARE(registry.attribution(source->toJson()), source->attribution);
    QJsonObject broken = source->toJson();
    broken.remove(QStringLiteral("matrices"));
    QVERIFY(!registry.imagery(broken, &error));
    QVERIFY(!error.isEmpty());
  }
};

QTEST_GUILESS_MAIN(TestWmts)
#include "tst_wmts.moc"
