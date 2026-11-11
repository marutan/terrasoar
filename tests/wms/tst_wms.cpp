// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include "concurrency.h"
#include "mockhttpserver.h"
#include "mockwms.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/providers/providerregistry.h"
#include "ts/tiling/tileloader.h"
#include "ts/wms/wmscapabilities.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"

namespace
{

using ts::test::MockHttpServer;
using ts::test::MockWmsBehaviour;

QByteArray fixture(const QString& name)
{
  QFile file(QStringLiteral(TS_TEST_DATA_DIR "/wms/") + name);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

ts::WmsCapabilities capabilitiesOf(const char* name)
{
  QString error;
  const std::optional<ts::WmsCapabilities> parsed =
      ts::parseWmsCapabilities(fixture(QLatin1String(name)), &error);
  if (!parsed) {
    qWarning("%s: %s", name, qPrintable(error));
  }
  return parsed.value_or(ts::WmsCapabilities{});
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

/** A source for the mock server's "pattern" layer. */
ts::WmsSource mockSource(const MockHttpServer& server,
                         const QString& crs = QStringLiteral("EPSG:4326"),
                         const QString& version = QStringLiteral("1.3.0"))
{
  ts::WmsSource source;
  source.url = server.url(QStringLiteral("/wms"));
  source.version = version;
  source.layers = QStringList{QStringLiteral("pattern")};
  source.styles = QStringList{QString()};
  source.crs = crs;
  source.title = QStringLiteral("Test pattern");
  return source;
}

const std::atomic<bool> notCancelled{false};

QUrlQuery queryOf(const QUrl& url)
{
  return QUrlQuery(url);
}

} // namespace

/** Unit tests for ts/wms. */
class TestWms : public QObject
{
  Q_OBJECT

private slots:
  // --- Capabilities: real-world documents ------------------------------------

  void realDocuments_data()
  {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("version");
    QTest::addColumn<QString>("layer");
    QTest::addColumn<QString>("host");
    QTest::addColumn<QString>("crs");
    QTest::addColumn<int>("minimumLayers");
    QTest::newRow("terrestris 1.3.0")
        << QStringLiteral("terrestris-130.xml") << QStringLiteral("1.3.0")
        << QStringLiteral("OSM-WMS") << QStringLiteral("ows.terrestris.de")
        << QStringLiteral("EPSG:4326") << 5;
    QTest::newRow("terrestris 1.1.1")
        << QStringLiteral("terrestris-111.xml") << QStringLiteral("1.1.1")
        << QStringLiteral("OSM-WMS") << QStringLiteral("ows.terrestris.de")
        << QStringLiteral("EPSG:4326") << 5;
    QTest::newRow("GEBCO") << QStringLiteral("gebco-130.xml")
                           << QStringLiteral("1.3.0")
                           << QStringLiteral("GEBCO_LATEST")
                           << QStringLiteral("wms.gebco.net")
                           << QStringLiteral("EPSG:4326") << 3;
    QTest::newRow("USGS") << QStringLiteral("usgs-130.xml")
                          << QStringLiteral("1.3.0") << QStringLiteral("0")
                          << QStringLiteral("basemap.nationalmap.gov")
                          << QStringLiteral("CRS:84") << 1;
    QTest::newRow("EOX") << QStringLiteral("eox-111.xml")
                         << QStringLiteral("1.1.1")
                         << QStringLiteral("terrain-light")
                         << QStringLiteral("tiles.maps.eox.at")
                         << QStringLiteral("EPSG:4326") << 20;
    QTest::newRow("nowCOAST")
        << QStringLiteral("nowcoast-130.xml") << QStringLiteral("1.3.0")
        << QStringLiteral("") << QStringLiteral("nowcoast.noaa.gov")
        << QStringLiteral("CRS:84") << 5;
    QTest::newRow("GIBS") << QStringLiteral("gibs-4326-130.xml")
                          << QStringLiteral("1.3.0") << QStringLiteral("")
                          << QStringLiteral("gibs.earthdata.nasa.gov")
                          << QStringLiteral("EPSG:4326") << 20;
  }

  void realDocuments()
  {
    QFETCH(QString, file);
    QFETCH(QString, version);
    QFETCH(QString, layer);
    QFETCH(QString, host);
    QFETCH(QString, crs);
    QFETCH(int, minimumLayers);
    QString error;
    const std::optional<ts::WmsCapabilities> parsed =
        ts::parseWmsCapabilities(fixture(file), &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    const ts::WmsCapabilities caps = parsed.value_or(ts::WmsCapabilities{});
    QCOMPARE(caps.version, version);
    QVERIFY(!caps.title.isEmpty());
    QCOMPARE(caps.getMapUrl.host(), host);
    QVERIFY(caps.getMapFormats.contains(QStringLiteral("image/png")));
    const std::vector<const ts::WmsLayer*> named = caps.namedLayers();
    QVERIFY2(static_cast<int>(named.size()) >= minimumLayers,
             qPrintable(QString::number(named.size())));
    const ts::WmsLayer* chosen =
        layer.isEmpty() ? named.at(1) : caps.findLayer(layer);
    QVERIFY(chosen != nullptr);
    QVERIFY(!chosen->title.isEmpty());
    // Every one can be requested as tiles, geographic where offered.
    QCOMPARE(ts::chooseCrs(chosen->crs, caps.version), crs);
    QString sourceError;
    const std::optional<ts::WmsSource> source = ts::wmsSourceFor(
        caps, QStringList{chosen->name}, {}, QString(), &sourceError);
    QVERIFY2(source.has_value(), qPrintable(sourceError));
    const ts::WmsSource s = source.value_or(ts::WmsSource{});
    QCOMPARE(s.crs, crs);
    QCOMPARE(s.url, caps.getMapUrl);
    QVERIFY(!ts::getMapUrl(s, ts::TileKey{2, 3, 1}).isEmpty() ||
            (s.bounds && !s.bounds->contains(22.5, -22.5)));
  }

  void realDocumentDetails()
  {
    // A few things each server does its own way.
    const ts::WmsCapabilities terrestris = capabilitiesOf("terrestris-130.xml");
    QCOMPARE(terrestris.maxWidth, 8000);
    QCOMPARE(terrestris.maxHeight, 8000);
    QCOMPARE(terrestris.fees, QStringLiteral("None"));
    QVERIFY(terrestris.accessConstraints.contains(
        QStringLiteral("OpenStreetMap contributors")));
    const ts::WmsLayer* osm = terrestris.findLayer(QStringLiteral("OSM-WMS"));
    QVERIFY(osm != nullptr);
    QVERIFY(osm->geographicBounds.has_value());
    QVERIFY(osm->supportsCrs(QStringLiteral("EPSG:3857")));
    // The same server in 1.1.1 offers the same layers.
    const ts::WmsCapabilities old = capabilitiesOf("terrestris-111.xml");
    QCOMPARE(old.namedLayers().size(), terrestris.namedLayers().size());
    QVERIFY(
        old.findLayer(QStringLiteral("OSM-WMS"))->geographicBounds.has_value());

    // GIBS: the layer limit, a lower-case code at the root.
    const ts::WmsCapabilities gibs = capabilitiesOf("gibs-4326-130.xml");
    QCOMPARE(gibs.layerLimit, 5);
    QCOMPARE(gibs.maxWidth, 20000);
    QVERIFY(gibs.layers.front().supportsCrs(QStringLiteral("EPSG:4326")));

    // nowCOAST: children offer EPSG:3857 and CRS:84 and inherit EPSG:4326.
    const ts::WmsCapabilities nowcoast = capabilitiesOf("nowcoast-130.xml");
    const ts::WmsLayer* child = nowcoast.namedLayers().at(1);
    QVERIFY(child->supportsCrs(QStringLiteral("EPSG:4326")));
    QVERIFY(child->supportsCrs(QStringLiteral("EPSG:3857")));

    // EOX: an empty SRS is ignored; EPSG:900913 is Web Mercator.
    const ts::WmsCapabilities eox = capabilitiesOf("eox-111.xml");
    for (const ts::WmsLayer* layer : eox.namedLayers()) {
      QVERIFY(!layer->crs.contains(QString()));
    }
    QCOMPARE(eox.getMapUrl.toString(),
             QStringLiteral("http://tiles.maps.eox.at/?"));

    // USGS: ArcGIS's CRS:84 is preferred over its latitude-first EPSG:4326.
    const ts::WmsCapabilities usgs = capabilitiesOf("usgs-130.xml");
    QVERIFY(
        usgs.namedLayers().front()->supportsCrs(QStringLiteral("EPSG:102100")));
  }

  // --- Capabilities: unusual and broken documents ----------------------------

  void inheritance()
  {
    const ts::WmsCapabilities caps =
        capabilitiesOf("synthetic-111-inheritance.xml");
    QCOMPARE(caps.version, QStringLiteral("1.1.1"));
    QCOMPARE(caps.getMapFormats, (QStringList{QStringLiteral("image/png"),
                                              QStringLiteral("image/jpeg")}));
    // The GET address, not POST, with its own query.
    QCOMPARE(caps.getMapUrl.toString(),
             QStringLiteral(
                 "http://example.invalid/cgi-bin/mapserv?map=/maps/world.map"));
    // The group without a name is a layer of the tree, not a named one.
    QCOMPARE(caps.namedLayers().size(), std::size_t{4});
    QCOMPARE(caps.layers.size(), std::size_t{1});
    QCOMPARE(caps.layers.front().children.size(), std::size_t{3});
    QVERIFY(caps.layers.front().children.front().name.isEmpty());

    const ts::WmsLayer* relief = caps.findLayer(QStringLiteral("relief"));
    QVERIFY(relief != nullptr);
    // SRS: two from one element at the root, one from the group.
    QCOMPARE(relief->crs, (QStringList{QStringLiteral("EPSG:4326"),
                                       QStringLiteral("EPSG:3857"),
                                       QStringLiteral("EPSG:32633")}));
    // Styles added to the root's.
    QCOMPARE(relief->styles.size(), std::size_t{2});
    QCOMPARE(relief->styles.at(0).name, QStringLiteral("default"));
    QCOMPARE(relief->styles.at(1).name, QStringLiteral("grey"));
    QCOMPARE(relief->styles.at(1).legendUrl.toString(),
             QStringLiteral("http://example.invalid/legend.png"));
    // Attributes: its own, and inherited from the group.
    QVERIFY(relief->opaque);
    QVERIFY(relief->queryable);
    // Its own box replaces the root's.
    QVERIFY(relief->geographicBounds.has_value());
    QCOMPARE(relief->geographicBounds->westDeg, 5.0);
    QCOMPARE(relief->geographicBounds->northDeg, 55.0);
    // ScaleHint in pixel diagonals to scale denominators.
    QVERIFY(std::abs(relief->minScaleDenominator -
                     0.5 / std::sqrt(2.0) / 0.00028) < 1e-6);
    QVERIFY(std::abs(relief->maxScaleDenominator -
                     5000.0 / std::sqrt(2.0) / 0.00028) < 1e-3);
    QCOMPARE(relief->attributionTitle, QStringLiteral("Example Agency"));
    QCOMPARE(relief->attributionUrl.toString(),
             QStringLiteral("http://example.invalid/about"));

    const ts::WmsLayer* rivers = caps.findLayer(QStringLiteral("rivers"));
    QVERIFY(!rivers->queryable); // its own attribute wins
    QVERIFY(!rivers->opaque);
    QCOMPARE(rivers->attributionTitle, QStringLiteral("River Survey"));
    QCOMPARE(rivers->geographicBounds->westDeg, -180.0); // from the root
    QCOMPARE(caps.findLayer(QStringLiteral("poster"))->noSubsets, true);
    QCOMPARE(caps.findLayer(QStringLiteral("fixed"))->fixedWidth, 512);
    QVERIFY(caps.findLayer(QStringLiteral("nothing")) == nullptr);
  }

  void axesAndOddities()
  {
    const ts::WmsCapabilities caps = capabilitiesOf("synthetic-130-axes.xml");
    QCOMPARE(caps.version, QStringLiteral("1.3.0"));
    QCOMPARE(caps.layerLimit, 2);
    QCOMPARE(caps.maxWidth, 200);
    QCOMPARE(caps.getMapUrl.toString(),
             QStringLiteral("https://example.invalid/wms?"));
    // A latitude-first EPSG:4326 box is read as such.
    const ts::WmsLayer* lakes = caps.findLayer(QStringLiteral("lakes"));
    QVERIFY(lakes->geographicBounds.has_value());
    QCOMPARE(lakes->geographicBounds->westDeg, -10.0);
    QCOMPARE(lakes->geographicBounds->southDeg, 40.0);
    QCOMPARE(lakes->geographicBounds->eastDeg, 30.0);
    QCOMPARE(lakes->geographicBounds->northDeg, 60.0);
    QCOMPARE(lakes->minScaleDenominator, 1000.0);
    QCOMPARE(lakes->maxScaleDenominator, 5.0e7);
    // CRS:84 boxes are longitude first.
    const ts::WmsLayer* cities = caps.findLayer(QStringLiteral("cities"));
    QCOMPARE(cities->geographicBounds->westDeg, -10.0);
    QCOMPARE(cities->geographicBounds->southDeg, 40.0);
    QVERIFY(!caps.findLayer(QStringLiteral("roads"))->geographicBounds);
    // The lower-case code counts; the empty element doesn't.
    QCOMPARE(lakes->crs, QStringList{QStringLiteral("epsg:4326")});
    // Requested in the usual spelling.
    QCOMPARE(ts::chooseCrs(lakes->crs, caps.version),
             QStringLiteral("EPSG:4326"));
    QCOMPARE(ts::chooseCrs(cities->crs, caps.version),
             QStringLiteral("CRS:84"));
    // Web Mercator only, under an old code.
    const ts::WmsLayer* tiles = caps.findLayer(QStringLiteral("tiles"));
    QCOMPARE(ts::chooseCrs(tiles->crs, caps.version),
             QStringLiteral("EPSG:900913"));
  }

  void brokenDocuments_data()
  {
    QTest::addColumn<QByteArray>("document");
    QTest::addColumn<QString>("message");
    QTest::newRow("service exception")
        << fixture(QStringLiteral("exception-111.xml"))
        << QStringLiteral("Unsupported VERSION parameter value");
    QTest::newRow("HTML page") << fixture(QStringLiteral("error-page.html"))
                               << QStringLiteral("web page");
    QTest::newRow("truncated") << fixture(QStringLiteral("truncated-130.xml"))
                               << QStringLiteral("not valid XML");
    QTest::newRow("empty") << QByteArray() << QStringLiteral("empty");
    QTest::newRow("another document")
        << QByteArray("<kml><Document/></kml>") << QStringLiteral("<kml>");
    QTest::newRow("no GetMap")
        << QByteArray("<WMS_Capabilities version=\"1.3.0\"><Capability><Layer>"
                      "<Name>a</Name></Layer></Capability></WMS_Capabilities>")
        << QStringLiteral("where to send");
    QTest::newRow("no named layer")
        << QByteArray(
               "<WMS_Capabilities version=\"1.3.0\"><Capability><Request>"
               "<GetMap><DCPType><HTTP><Get><OnlineResource "
               "href=\"http://a/\"/></Get></HTTP></DCPType></GetMap>"
               "</Request><Layer><Title>t</Title></Layer></Capability>"
               "</WMS_Capabilities>")
        << QStringLiteral("no layers");
  }

  void brokenDocuments()
  {
    QFETCH(QByteArray, document);
    QFETCH(QString, message);
    QString error;
    QVERIFY(!ts::parseWmsCapabilities(document, &error));
    QVERIFY2(error.contains(message), qPrintable(error));
  }

  // --- Requests and axis order
  // --------------------------------------------------

  void axisOrder_data()
  {
    QTest::addColumn<QString>("version");
    QTest::addColumn<QString>("crs");
    QTest::addColumn<QString>("bbox");
    const QString lonFirst = QStringLiteral("-45,-22.5,-22.5,0");
    const QString latFirst = QStringLiteral("-22.5,-45,0,-22.5");
    QTest::newRow("1.1.1 EPSG:4326")
        << QStringLiteral("1.1.1") << QStringLiteral("EPSG:4326") << lonFirst;
    QTest::newRow("1.3.0 EPSG:4326")
        << QStringLiteral("1.3.0") << QStringLiteral("EPSG:4326") << latFirst;
    QTest::newRow("1.3.0 lower case")
        << QStringLiteral("1.3.0") << QStringLiteral("epsg:4326") << latFirst;
    QTest::newRow("1.3.0 CRS:84")
        << QStringLiteral("1.3.0") << QStringLiteral("CRS:84") << lonFirst;
    QTest::newRow("1.1.1 CRS:84")
        << QStringLiteral("1.1.1") << QStringLiteral("CRS:84") << lonFirst;
    QTest::newRow("1.3.0 EPSG:3857")
        << QStringLiteral("1.3.0") << QStringLiteral("EPSG:3857")
        << QStringLiteral(
               "-5009377.085697311,-2571663.0471736453,-2504688.5428486555,0");
    QTest::newRow("1.1.1 EPSG:900913")
        << QStringLiteral("1.1.1") << QStringLiteral("EPSG:900913")
        << QStringLiteral(
               "-5009377.085697311,-2571663.0471736453,-2504688.5428486555,0");
    QTest::newRow("unusable CRS")
        << QStringLiteral("1.3.0") << QStringLiteral("EPSG:32633")
        << QStringLiteral("");
  }

  void axisOrder()
  {
    // Tile 4/6/9: 45° W to 22.5° W, 22.5° S to the equator.
    QFETCH(QString, version);
    QFETCH(QString, crs);
    QFETCH(QString, bbox);
    const ts::GeoRect rect = ts::TileKey{3, 6, 4}.bounds();
    QCOMPARE(rect.westDeg, -45.0);
    QCOMPARE(rect.southDeg, -22.5);
    QCOMPARE(ts::bboxParameter(version, crs, rect), bbox);
  }

  void webMercatorEdges()
  {
    // The whole world: x from −πa to +πa (the east edge doesn't wrap), y
    // clipped to the Mercator limit, where the map is square.
    const QStringList world =
        ts::bboxParameter(QStringLiteral("1.3.0"), QStringLiteral("EPSG:3857"),
                          ts::GeoRect{-180.0, -90.0, 180.0, 90.0})
            .split(QLatin1Char(','));
    QCOMPARE(world.size(), 4);
    QVERIFY(std::abs(world.at(0).toDouble() + 20037508.342789244) < 1e-6);
    QVERIFY(std::abs(world.at(2).toDouble() - 20037508.342789244) < 1e-6);
    QVERIFY(std::abs(world.at(3).toDouble() - 20037508.342789244) < 1e-3);
    QVERIFY(std::abs(world.at(1).toDouble() + 20037508.342789244) < 1e-3);
    // Beyond the limit nothing is left.
    QVERIFY(!ts::webMercatorCoverage(ts::GeoRect{0.0, 85.2, 10.0, 90.0}));
    QVERIFY(ts::bboxParameter(QStringLiteral("1.3.0"),
                              QStringLiteral("EPSG:3857"),
                              ts::GeoRect{0.0, 85.2, 10.0, 90.0})
                .isEmpty());
    const std::optional<ts::GeoRect> clipped =
        ts::webMercatorCoverage(ts::GeoRect{0.0, 80.0, 10.0, 90.0});
    QVERIFY(clipped && clipped->northDeg < 85.06 && clipped->southDeg == 80.0);
  }

  void getMapRequests()
  {
    ts::WmsSource source;
    source.url = QUrl(QStringLiteral(
        "http://example.invalid/cgi-bin/mapserv?map=/maps/world.map&"
        "REQUEST=GetCapabilities&service=wms&Layers=old"));
    source.version = QStringLiteral("1.3.0");
    source.layers =
        QStringList{QStringLiteral("land"), QStringLiteral("roads")};
    source.styles = QStringList{QString(), QStringLiteral("thin")};
    source.format = QStringLiteral("image/png");
    source.crs = QStringLiteral("EPSG:4326");
    const QUrl url = ts::getMapUrl(source, ts::TileKey{3, 6, 4});
    QCOMPARE(url.host(), QStringLiteral("example.invalid"));
    QCOMPARE(url.path(), QStringLiteral("/cgi-bin/mapserv"));
    const QUrlQuery query = queryOf(url);
    // MapServer's map= stays; the WMS parameters in the base URL go.
    QCOMPARE(query.queryItemValue(QStringLiteral("map")),
             QStringLiteral("/maps/world.map"));
    QCOMPARE(query.allQueryItemValues(QStringLiteral("REQUEST")),
             QStringList{QStringLiteral("GetMap")});
    QVERIFY(!query.hasQueryItem(QStringLiteral("service")));
    QVERIFY(!query.hasQueryItem(QStringLiteral("Layers")));
    QCOMPARE(query.queryItemValue(QStringLiteral("SERVICE")),
             QStringLiteral("WMS"));
    QCOMPARE(query.queryItemValue(QStringLiteral("VERSION")),
             QStringLiteral("1.3.0"));
    QCOMPARE(query.queryItemValue(QStringLiteral("LAYERS")),
             QStringLiteral("land,roads"));
    QCOMPARE(query.queryItemValue(QStringLiteral("STYLES")),
             QStringLiteral(",thin"));
    QCOMPARE(query.queryItemValue(QStringLiteral("CRS")),
             QStringLiteral("EPSG:4326"));
    QVERIFY(!query.hasQueryItem(QStringLiteral("SRS")));
    QCOMPARE(query.queryItemValue(QStringLiteral("BBOX")),
             QStringLiteral("-22.5,-45,0,-22.5"));
    QCOMPARE(query.queryItemValue(QStringLiteral("WIDTH")),
             QStringLiteral("256"));
    QCOMPARE(query.queryItemValue(QStringLiteral("HEIGHT")),
             QStringLiteral("256"));
    QCOMPARE(query.queryItemValue(QStringLiteral("FORMAT")),
             QStringLiteral("image/png"));
    QCOMPARE(query.queryItemValue(QStringLiteral("TRANSPARENT")),
             QStringLiteral("TRUE"));
    QCOMPARE(query.queryItemValue(QStringLiteral("EXCEPTIONS")),
             QStringLiteral("XML"));
    // 1.1.1: SRS, longitude first, its exception format.
    source.version = QStringLiteral("1.1.1");
    source.transparent = false;
    const QUrlQuery old = queryOf(ts::getMapUrl(source, ts::TileKey{3, 6, 4}));
    QCOMPARE(old.queryItemValue(QStringLiteral("SRS")),
             QStringLiteral("EPSG:4326"));
    QVERIFY(!old.hasQueryItem(QStringLiteral("CRS")));
    QCOMPARE(old.queryItemValue(QStringLiteral("BBOX")),
             QStringLiteral("-45,-22.5,-22.5,0"));
    QCOMPARE(old.queryItemValue(QStringLiteral("TRANSPARENT")),
             QStringLiteral("FALSE"));
    QCOMPARE(old.queryItemValue(QStringLiteral("EXCEPTIONS")),
             QStringLiteral("application/vnd.ogc.se_xml"));
    // Outside the source's bounds: nothing to request.
    source.bounds = ts::GeoRect{0.0, 40.0, 20.0, 60.0};
    QVERIFY(ts::getMapUrl(source, ts::TileKey{3, 6, 4}).isEmpty());
    QVERIFY(!ts::getMapUrl(source, ts::TileKey{3, 8, 1}).isEmpty());
    // A URL ending in "?" makes a valid request too.
    source.url = QUrl(QStringLiteral("http://tiles.maps.eox.at/?"));
    source.bounds.reset();
    const QUrl eox = ts::getMapUrl(source, ts::TileKey{0, 0, 0});
    QCOMPARE(eox.path(), QStringLiteral("/"));
    QVERIFY(eox.query().startsWith(QStringLiteral("SERVICE=WMS")));
  }

  void capabilitiesRequests()
  {
    const QUrl url = ts::capabilitiesUrl(QUrl(QStringLiteral(
        "https://wms.gebco.net/mapserv?map=x&request=GetMap&VERSION=1.1.1")));
    const QUrlQuery query = queryOf(url);
    QCOMPARE(query.queryItemValue(QStringLiteral("map")), QStringLiteral("x"));
    QCOMPARE(query.queryItemValue(QStringLiteral("SERVICE")),
             QStringLiteral("WMS"));
    QCOMPARE(query.queryItemValue(QStringLiteral("REQUEST")),
             QStringLiteral("GetCapabilities"));
    QCOMPARE(query.allQueryItemValues(QStringLiteral("VERSION")),
             QStringList{QStringLiteral("1.3.0")});
    QVERIFY(!query.hasQueryItem(QStringLiteral("request")));
    QCOMPARE(queryOf(ts::capabilitiesUrl(QUrl(QStringLiteral("http://a/b")),
                                         QStringLiteral("1.1.1")))
                 .queryItemValue(QStringLiteral("VERSION")),
             QStringLiteral("1.1.1"));
  }

  void choices()
  {
    const QStringList offered = {QStringLiteral("EPSG:3857"),
                                 QStringLiteral("EPSG:4326"),
                                 QStringLiteral("CRS:84")};
    QCOMPARE(ts::chooseCrs(offered, QStringLiteral("1.3.0")),
             QStringLiteral("CRS:84"));
    QCOMPARE(ts::chooseCrs(offered, QStringLiteral("1.1.1")),
             QStringLiteral("EPSG:4326"));
    QCOMPARE(
        ts::chooseCrs({QStringLiteral("EPSG:102100")}, QStringLiteral("1.3.0")),
        QStringLiteral("EPSG:102100"));
    QVERIFY(
        ts::chooseCrs({QStringLiteral("EPSG:25832")}, QStringLiteral("1.3.0"))
            .isEmpty());
    for (const char* code : {"EPSG:3857", "epsg:900913", "EPSG:3785",
                             "EPSG:102100", "EPSG:102113"}) {
      QVERIFY(ts::isWebMercatorCrs(QLatin1String(code)));
    }
    QVERIFY(!ts::isWebMercatorCrs(QStringLiteral("EPSG:3395")));
    QVERIFY(ts::isGeographicCrs(QStringLiteral("crs:84")));
    const QStringList formats = {QStringLiteral("image/jpeg"),
                                 QStringLiteral(" image/png "),
                                 QStringLiteral("text/html")};
    QCOMPARE(ts::chooseFormat(formats, false), QStringLiteral("image/png"));
    QCOMPARE(ts::chooseFormat(formats, true), QStringLiteral("image/jpeg"));
    QCOMPARE(ts::chooseFormat({QStringLiteral("image/png; mode=8bit")}, false),
             QStringLiteral("image/png; mode=8bit"));
    QVERIFY(ts::chooseFormat({QStringLiteral("image/tiff")}, false).isEmpty());
    QVERIFY(ts::isTransparentFormat(QStringLiteral("image/png8")));
    QVERIFY(!ts::isTransparentFormat(QStringLiteral("image/jpeg")));
  }

  void sourcesFromCapabilities()
  {
    const ts::WmsCapabilities synthetic =
        capabilitiesOf("synthetic-111-inheritance.xml");
    QString error;
    // Two layers: the CRS both offer, the union of their boxes.
    std::optional<ts::WmsSource> source = ts::wmsSourceFor(
        synthetic, {QStringLiteral("relief"), QStringLiteral("rivers")},
        {QStringLiteral("grey")}, QString(), &error);
    QVERIFY2(source.has_value(), qPrintable(error));
    ts::WmsSource s = source.value_or(ts::WmsSource{});
    QCOMPARE(s.version, QStringLiteral("1.1.1"));
    QCOMPARE(s.crs, QStringLiteral("EPSG:4326"));
    QCOMPARE(s.layers,
             (QStringList{QStringLiteral("relief"), QStringLiteral("rivers")}));
    QCOMPARE(s.styles, (QStringList{QStringLiteral("grey"), QString()}));
    QCOMPARE(s.title, QStringLiteral("Shaded relief, Rivers"));
    QCOMPARE(s.attribution, QStringLiteral("Example Agency; River Survey"));
    QVERIFY(s.bounds.has_value());
    QCOMPARE(s.bounds->westDeg, -180.0);
    // An opaque layer: JPEG, not transparent.
    QCOMPARE(s.format, QStringLiteral("image/jpeg"));
    QVERIFY(!s.transparent);
    source = ts::wmsSourceFor(synthetic, {QStringLiteral("rivers")}, {},
                              QString(), &error);
    QCOMPARE(source.value_or(ts::WmsSource{}).format,
             QStringLiteral("image/png"));
    QVERIFY(source.value_or(ts::WmsSource{}).transparent);
    QCOMPARE(source.value_or(ts::WmsSource{}).tileSizePx, 256);
    // What can't be tiled, or doesn't exist.
    QVERIFY(!ts::wmsSourceFor(synthetic, {QStringLiteral("poster")}, {}, {},
                              &error));
    QVERIFY(error.contains(QStringLiteral("fixed size")));
    QVERIFY(!ts::wmsSourceFor(synthetic, {QStringLiteral("fixed")}, {}, {},
                              &error));
    QVERIFY(
        !ts::wmsSourceFor(synthetic, {QStringLiteral("x")}, {}, {}, &error));
    QVERIFY(error.contains(QStringLiteral("\"x\"")));
    QVERIFY(!ts::wmsSourceFor(synthetic, {}, {}, {}, &error));

    const ts::WmsCapabilities axes = capabilitiesOf("synthetic-130-axes.xml");
    // MaxWidth 200: 128-pixel tiles. A non-PNG transparent format.
    source = ts::wmsSourceFor(axes, {QStringLiteral("lakes")}, {}, {}, &error);
    QCOMPARE(source.value_or(ts::WmsSource{}).tileSizePx, 128);
    QCOMPARE(source.value_or(ts::WmsSource{}).format,
             QStringLiteral("image/png; mode=8bit"));
    // More layers than allowed.
    QVERIFY(
        !ts::wmsSourceFor(axes,
                          {QStringLiteral("lakes"), QStringLiteral("cities"),
                           QStringLiteral("roads")},
                          {}, {}, &error));
    QVERIFY(error.contains(QStringLiteral("at most 2")));
    // Nothing in common: lakes is geographic only, tiles Mercator only.
    QVERIFY(!ts::wmsSourceFor(
        axes, {QStringLiteral("lakes"), QStringLiteral("tiles")}, {}, {},
        &error));
    // Mercator only: drawn reprojected.
    source = ts::wmsSourceFor(axes, {QStringLiteral("tiles")}, {}, {}, &error);
    QCOMPARE(source.value_or(ts::WmsSource{}).imageProjection(),
             ts::TileImageProjection::WebMercator);
  }

  void sourceJson()
  {
    ts::WmsSource source;
    source.url = QUrl(QStringLiteral("https://example.invalid/wms?map=a"));
    source.version = QStringLiteral("1.1.1");
    source.layers = {QStringLiteral("a"), QStringLiteral("b")};
    source.styles = {QString(), QStringLiteral("s")};
    source.format = QStringLiteral("image/jpeg");
    source.transparent = false;
    source.crs = QStringLiteral("EPSG:3857");
    source.tileSizePx = 512;
    source.maximumLevel = 12;
    source.bounds = ts::GeoRect{-10.0, 20.0, 30.0, 40.0};
    source.title = QStringLiteral("A and B");
    source.attribution = QStringLiteral("Someone");
    const QJsonObject json = source.toJson();
    QCOMPARE(json.value(QStringLiteral("kind")).toString(),
             QStringLiteral("wms"));
    QString error;
    const std::optional<ts::WmsSource> back =
        ts::WmsSource::fromJson(json, &error);
    QVERIFY2(back.has_value(), qPrintable(error));
    const ts::WmsSource b = back.value_or(ts::WmsSource{});
    QCOMPARE(b.id(), source.id());
    QCOMPARE(b.toJson(), json);
    QCOMPARE(b.bounds->northDeg, 40.0);
    QCOMPARE(b.imageProjection(), ts::TileImageProjection::WebMercator);
    // Different requests, different ids.
    ts::WmsSource other = source;
    other.styles = {QString(), QString()};
    QVERIFY(other.id() != source.id());

    QJsonObject bad = json;
    bad.insert(QStringLiteral("url"), QStringLiteral("ftp://x/"));
    QVERIFY(!ts::WmsSource::fromJson(bad, &error));
    bad = json;
    bad.insert(QStringLiteral("layers"), QJsonArray());
    QVERIFY(!ts::WmsSource::fromJson(bad, &error));
    bad = json;
    bad.insert(QStringLiteral("crs"), QStringLiteral("EPSG:25832"));
    QVERIFY(!ts::WmsSource::fromJson(bad, &error));
    QVERIFY(error.contains(QStringLiteral("EPSG:25832")));
  }

  // --- Catalogue
  // ----------------------------------------------------------------

  void catalogue()
  {
    // The catalogue in the repository (data/wms-catalogue.json).
    QFile file(QStringLiteral(TS_REPO_DATA_DIR "/wms-catalogue.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString error;
    const std::optional<std::vector<ts::WmsCatalogueEntry>> entries =
        ts::parseWmsCatalogue(file.readAll(), &error);
    QVERIFY2(entries.has_value(), qPrintable(error));
    QVERIFY(entries->size() >= 3);
    for (const ts::WmsCatalogueEntry& entry : *entries) {
      QVERIFY(!entry.title.isEmpty());
      QCOMPARE(entry.url.scheme(), QStringLiteral("https"));
      QVERIFY(!entry.description.isEmpty());
      QVERIFY(!entry.attribution.isEmpty());
      QVERIFY(entry.termsUrl.isValid());
    }
    // Entries without a title or a web address are left out.
    const std::optional<std::vector<ts::WmsCatalogueEntry>> partial =
        ts::parseWmsCatalogue(
            "{\"format\": \"terrasoar-wms-catalogue\", \"version\": 1, "
            "\"services\": [ "
            "{\"title\": \"ok\", \"url\": \"http://a/wms\"}, "
            "{\"title\": \"\", \"url\": \"http://b/wms\"}, "
            "{\"title\": \"file\", \"url\": \"file:///etc/passwd\"}]}",
            &error);
    QCOMPARE(partial.value_or(std::vector<ts::WmsCatalogueEntry>{}).size(),
             std::size_t{1});
    QVERIFY(!ts::parseWmsCatalogue("[]", &error));
    QVERIFY(!ts::parseWmsCatalogue("{\"format\": \"x\"}", &error));
    QVERIFY(!ts::parseWmsCatalogue("{\"format\": \"terrasoar-wms-catalogue\", "
                                   "\"version\": 99, \"services\": []}",
                                   &error));
    QVERIFY(error.contains(QStringLiteral("newer")));
  }

  // --- The provider against the mock server ---------------------------------

  void mockServerImagesAgreeAcrossCrs()
  {
    // The mock's picture is the same in every CRS (the reference for the
    // tests below and for the golden images).
    QUrlQuery geographic;
    geographic.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("1.3.0"));
    geographic.addQueryItem(QStringLiteral("CRS"), QStringLiteral("CRS:84"));
    geographic.addQueryItem(QStringLiteral("BBOX"),
                            QStringLiteral("0,0,60,60"));
    geographic.addQueryItem(QStringLiteral("WIDTH"), QStringLiteral("120"));
    geographic.addQueryItem(QStringLiteral("HEIGHT"), QStringLiteral("120"));
    geographic.addQueryItem(QStringLiteral("TRANSPARENT"),
                            QStringLiteral("TRUE"));
    QUrlQuery latFirst = geographic;
    latFirst.removeAllQueryItems(QStringLiteral("CRS"));
    latFirst.addQueryItem(QStringLiteral("CRS"), QStringLiteral("EPSG:4326"));
    const QImage a = ts::test::mockWmsImage(geographic);
    const QImage b = ts::test::mockWmsImage(latFirst);
    QVERIFY(!a.isNull());
    QCOMPARE(a, b); // a square box reads the same both ways
    // The disc at 30° N 30° E is red.
    QCOMPARE(qRed(a.pixel(60, 60)), 220);
    QCOMPARE(qAlpha(a.pixel(5, 100)), 0);
  }

  void providerLoadsAndCaches()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(testNetwork());
    QTemporaryDir cacheDir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = cacheDir.path();
    const ts::TileKey key{3, 9, 2};
    {
      ts::WmsImageryProvider provider(mockSource(server), network, settings);
      QCOMPARE(provider.tileSizePx(), 256);
      QCOMPARE(provider.preferredLoaderThreads(), 6);
      QCOMPARE(provider.imageProjection(), ts::TileImageProjection::Geographic);
      QCOMPARE(provider.name(), QStringLiteral("Test pattern"));
      const ts::ImageLoadResult result =
          provider.loadTileImage(key, notCancelled);
      QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Image);
      QCOMPARE(result.image.size(), QSize(256, 256));
      // The image is what the server drew for that tile.
      QUrlQuery expected(ts::getMapUrl(provider.source(), key));
      QCOMPARE(result.image.convertToFormat(QImage::Format_ARGB32),
               ts::test::mockWmsImage(expected));
      QCOMPARE(ts::test::mockGetMapCount(server), 1);
      QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);
      // Again: from the disk cache, no request.
      QCOMPARE(provider.loadTileImage(key, notCancelled).outcome,
               ts::ImageLoadResult::Outcome::Image);
      QCOMPARE(ts::test::mockGetMapCount(server), 1);
      QCOMPARE(provider.statistics().cacheHits, 1);
      QVERIFY(provider.diskCache() != nullptr);
      QCOMPARE(provider.diskCache()->count(), 1);
    }
    // Another run of the program: still cached.
    ts::WmsImageryProvider again(mockSource(server), network, settings);
    QCOMPARE(again.loadTileImage(key, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(ts::test::mockGetMapCount(server), 1);
    // Outside the source's bounds: no data, no request.
    ts::WmsSource bounded = mockSource(server);
    bounded.bounds = ts::GeoRect{0.0, 0.0, 10.0, 10.0};
    ts::WmsImageryProvider small(bounded, network, settings);
    QCOMPARE(small.loadTileImage(ts::TileKey{3, 2, 2}, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
    QCOMPARE(ts::test::mockGetMapCount(server), 1);
  }

  void providerHonoursCacheHeaders()
  {
    // max-age 60 with an ETag; a fake clock moves on two minutes: the tile
    // is revalidated with If-None-Match and refreshed by the 304.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->maxAgeS = 60;
    behaviour->etag = true;
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(testNetwork());
    QTemporaryDir cacheDir;
    std::shared_ptr<std::atomic<long long>> clock =
        std::make_shared<std::atomic<long long>>(1714564800);
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = cacheDir.path();
    settings.clock = [clock]() { return clock->load(); };
    ts::WmsImageryProvider provider(mockSource(server), network, settings);
    const ts::TileKey key{2, 5, 1};
    QVERIFY(provider.loadTileImage(key, notCancelled).outcome ==
            ts::ImageLoadResult::Outcome::Image);
    clock->fetch_add(30);
    QVERIFY(!provider.loadTileImage(key, notCancelled).image.isNull());
    QCOMPARE(ts::test::mockGetMapCount(server), 1); // still fresh
    clock->fetch_add(90);
    QVERIFY(!provider.loadTileImage(key, notCancelled).image.isNull());
    QCOMPARE(ts::test::mockGetMapCount(server), 2);
    QVERIFY(!server.requests().back().header("if-none-match").isEmpty());
    QCOMPARE(provider.statistics().revalidated, 1);
    clock->fetch_add(30); // fresh again for 60 s after the 304
    QVERIFY(!provider.loadTileImage(key, notCancelled).image.isNull());
    QCOMPARE(ts::test::mockGetMapCount(server), 2);

    // no-store: never kept.
    behaviour->noStore = true;
    const ts::TileKey other{2, 6, 1};
    QVERIFY(!provider.loadTileImage(other, notCancelled).image.isNull());
    QVERIFY(!provider.loadTileImage(other, notCancelled).image.isNull());
    QCOMPARE(ts::test::mockGetMapCount(server), 4);
  }

  void providerOffline()
  {
    // The server goes away: saved tiles are shown (stale or not), others
    // fail; after three failures in a row the server is left alone.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->maxAgeS = 0; // everything stale at once
    std::unique_ptr<MockHttpServer> server =
        std::make_unique<MockHttpServer>(ts::test::mockWmsHandler(behaviour));
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(testNetwork());
    QTemporaryDir cacheDir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = cacheDir.path();
    ts::WmsImageryProvider provider(mockSource(*server), network, settings);
    std::atomic<int> statusChanges{0};
    provider.setStatusCallback([&statusChanges]() { ++statusChanges; });
    const ts::TileKey saved{3, 1, 1};
    QVERIFY(!provider.loadTileImage(saved, notCancelled).image.isNull());
    server.reset();

    const ts::ImageLoadResult stale =
        provider.loadTileImage(saved, notCancelled);
    QCOMPARE(stale.outcome, ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Offline);
    QVERIFY(provider.status().message.contains(QStringLiteral("127.0.0.1")));
    QCOMPARE(statusChanges.load(), 1);
    QCOMPARE(provider.statistics().servedStale, 1);
    QCOMPARE(provider.loadTileImage(ts::TileKey{3, 2, 1}, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QCOMPARE(provider.loadTileImage(ts::TileKey{3, 3, 1}, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    // Left alone now: no request, saved tiles still shown.
    const int attempts = network->attemptsSent();
    QCOMPARE(provider.loadTileImage(ts::TileKey{3, 4, 1}, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Failed);
    QCOMPARE(provider.loadTileImage(saved, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(network->attemptsSent(), attempts);
    QVERIFY(provider.statistics().skippedOffline >= 2);
    QCOMPARE(statusChanges.load(), 1); // one change, however many failures
  }

  void providerServerErrors_data()
  {
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("message");
    QTest::newRow("service exception")
        << 0 << QStringLiteral("Layer 'x' is not defined");
    QTest::newRow("HTML instead of an image")
        << 1 << QStringLiteral("text/html");
    QTest::newRow("HTTP 500") << 2 << QStringLiteral("500");
    QTest::newRow("oversized") << 3 << QStringLiteral("too large");
  }

  void providerServerErrors()
  {
    QFETCH(int, mode);
    QFETCH(QString, message);
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->serviceException = mode == 0;
    behaviour->html = mode == 1;
    behaviour->failStatus = mode == 2 ? 500 : 0;
    behaviour->oversized = mode == 3;
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    ts::NetworkSettings networkSettings = testNetwork();
    networkSettings.maximumResponseBytes = 1024 * 1024;
    ts::WmsImageryProvider provider(
        mockSource(server),
        std::make_shared<ts::NetworkClient>(networkSettings));
    const ts::ImageLoadResult result =
        provider.loadTileImage(ts::TileKey{1, 1, 0}, notCancelled);
    QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Failed);
    QVERIFY(result.image.isNull());
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Error);
    QVERIFY2(provider.status().message.contains(message),
             qPrintable(provider.status().message));
    // The server recovers: so does the status.
    behaviour->serviceException = false;
    behaviour->html = false;
    behaviour->failStatus = 0;
    behaviour->oversized = false;
    QCOMPARE(provider.loadTileImage(ts::TileKey{1, 1, 0}, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::Image);
    QCOMPARE(provider.status().state, ts::ProviderStatus::State::Ok);
  }

  void providerWebMercator()
  {
    // A Mercator-only server: requests in EPSG:3857, images delivered as
    // they are (the renderer reprojects them), nothing beyond 85°.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(
        ts::test::mockWmsHandler(behaviour, QStringLiteral("EPSG:3857")));
    ts::WmsImageryProvider provider(
        mockSource(server, QStringLiteral("EPSG:3857")),
        std::make_shared<ts::NetworkClient>(testNetwork()));
    QCOMPARE(provider.imageProjection(), ts::TileImageProjection::WebMercator);
    const ts::ImageLoadResult result =
        provider.loadTileImage(ts::TileKey{2, 4, 1}, notCancelled);
    QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Image);
    const QUrlQuery sent(server.requests().back().url);
    QCOMPARE(sent.queryItemValue(QStringLiteral("CRS")),
             QStringLiteral("EPSG:3857"));
    QVERIFY(sent.queryItemValue(QStringLiteral("BBOX"))
                .startsWith(QStringLiteral("0,")));
    QCOMPARE(provider.loadTileImage(ts::TileKey{6, 3, 0}, notCancelled).outcome,
             ts::ImageLoadResult::Outcome::NoData);
  }

  void loaderCancelsTilesLeavingTheView()
  {
    // A slow server; six tiles wanted, then none: every request is
    // cancelled (its connection dropped) and nothing is delivered.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->delayMs = 3000;
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(server),
            std::make_shared<ts::NetworkClient>(testNetwork()));
    ts::TileLoaderSettings loaderSettings;
    loaderSettings.buildMeshes = false;
    ts::TileLoader loader(provider, loaderSettings);
    QCOMPARE(loader.threadCount(), 6);
    std::vector<ts::TileLoadRequest> wanted;
    for (int x = 0; x < 6; ++x) {
      wanted.push_back(ts::TileLoadRequest{ts::TileKey{3, x, 1}, 0.0});
    }
    loader.setRequests(wanted);
    QVERIFY(
        QTest::qWaitFor([&]() { return server.requestCount() == 6; }, 5000));
    loader.setRequests({});
    QVERIFY(
        QTest::qWaitFor([&]() { return loader.cancelledCount() == 6; }, 3000));
    QVERIFY(
        QTest::qWaitFor([&]() { return server.abandonedCount() == 6; }, 3000));
    QCOMPARE(loader.loadedCount(), 0);
    QVERIFY(loader.takeLoaded(100).empty());
    QCOMPARE(loader.outstanding(), 0);
  }

  void loaderGoesIdleWhenTheServerFails()
  {
    // Failures aren't delivered and aren't asked for again at once, so
    // nothing stays outstanding and the view can go idle.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->failStatus = 503;
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(
            mockSource(server),
            std::make_shared<ts::NetworkClient>(testNetwork()));
    ts::TileLoaderSettings loaderSettings;
    loaderSettings.buildMeshes = false;
    loaderSettings.failureRetryDelay = std::chrono::milliseconds(300);
    ts::TileLoader loader(provider, loaderSettings);
    const std::vector<ts::TileLoadRequest> wanted = {
        ts::TileLoadRequest{ts::TileKey{2, 1, 1}, 0.0},
        ts::TileLoadRequest{ts::TileKey{2, 2, 1}, 0.0}};
    loader.setRequests(wanted);
    QVERIFY(QTest::qWaitFor([&]() { return loader.failedCount() == 2; }, 5000));
    QCOMPARE(loader.outstanding(), 0);
    QVERIFY(loader.takeLoaded(10).empty());
    const int sent = ts::test::mockGetMapCount(server);
    loader.setRequests(wanted); // too soon: not asked again
    QTest::qWait(100);
    QCOMPARE(ts::test::mockGetMapCount(server), sent);
    // The server recovers; after the delay the tiles come.
    behaviour->failStatus = 0;
    QTest::qWait(300);
    loader.setRequests(wanted);
    QVERIFY(QTest::qWaitFor([&]() { return loader.loadedCount() == 2; }, 5000));
    QCOMPARE(loader.takeLoaded(10).size(), std::size_t{2});
  }

  void registryMakesWmsProviders()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    ts::ProviderRegistry registry;
    ts::WmsProviderSettings settings;
    registry.setWmsNetwork(std::make_shared<ts::NetworkClient>(testNetwork()),
                           settings);
    ts::WmsSource source = mockSource(server);
    source.attribution = QStringLiteral("Mock Agency");
    QString error;
    const std::shared_ptr<ts::ImageryProvider> provider =
        registry.imagery(source.toJson(), &error);
    QVERIFY2(provider != nullptr, qPrintable(error));
    QCOMPARE(provider->id(), source.id());
    // Shared like any other source.
    QCOMPARE(registry.imagery(source.toJson(), &error), provider);
    QCOMPARE(registry.attribution(source.toJson()),
             QStringLiteral("Mock Agency"));
    QVERIFY(!provider->loadImage(ts::TileKey{1, 0, 0}, notCancelled).isNull());
    QJsonObject broken = source.toJson();
    broken.insert(QStringLiteral("crs"), QStringLiteral("EPSG:2056"));
    QVERIFY(registry.imagery(broken, &error) == nullptr);
    QVERIFY(!error.isEmpty());
  }

  void concurrentLoads()
  {
    // One provider, many loader threads, the disk cache shared (TSan in CI).
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir cacheDir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = cacheDir.path();
    ts::WmsImageryProvider provider(
        mockSource(server), std::make_shared<ts::NetworkClient>(testNetwork()),
        settings);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&provider](int thread) {
          bool ok = true;
          for (int i = 0; i < 6; ++i) {
            const std::atomic<bool> go{false};
            ok = ok &&
                 !provider.loadImage(ts::TileKey{4, (thread + i) % 8, 3}, go)
                      .isNull();
          }
          return ok;
        });
    QCOMPARE(failures, 0);
    QVERIFY(provider.diskCache()->count() <= 8);
  }

  void benchmarkParseCapabilities()
  {
    const QByteArray document = fixture(QStringLiteral("gibs-4326-130.xml"));
    QBENCHMARK
    {
      QString error;
      QVERIFY(ts::parseWmsCapabilities(document, &error).has_value());
    }
  }
};

QTEST_GUILESS_MAIN(TestWms)
#include "tst_wms.moc"
