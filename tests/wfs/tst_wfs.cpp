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
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include "concurrency.h"
#include "mockhttpserver.h"
#include "mockwfs.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/providers/providerregistry.h"
#include "ts/vector/stylerules.h"
#include "ts/vector/vectormesh.h"
#include "ts/wfs/wfscapabilities.h"
#include "ts/wfs/wfsfeatureprovider.h"
#include "ts/wfs/wfssource.h"

namespace
{

using ts::test::MockHttpServer;
using ts::test::MockWmsBehaviour;

const std::atomic<bool> notCancelled{false};

QByteArray fixture(const QString& name)
{
  QFile file(QStringLiteral(TS_TEST_DATA_DIR "/") + name);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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

/** A source for @p type of the mock at @p server, in @p format. */
// A feature type's name, then a format: hard to confuse in the calls.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ts::WfsSource
mockSource(const MockHttpServer& server, const QString& type,
           const QString& format = QStringLiteral("application/json"))
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  QString error;
  const std::optional<ts::WfsCapabilities> capabilities =
      ts::parseWfsCapabilities(
          ts::test::mockWfsCapabilities(server.url(QStringLiteral("/wfs"))),
          &error);
  if (!capabilities) {
    qWarning("%s", qPrintable(error));
    return ts::WfsSource{};
  }
  std::optional<ts::WfsSource> source =
      ts::wfsSourceFor(*capabilities, type, &error);
  if (!source) {
    qWarning("%s", qPrintable(error));
    return ts::WfsSource{};
  }
  source->outputFormat = format;
  return *source;
}

int64_t countParts(const std::vector<ts::Feature>& features)
{
  int64_t parts = 0;
  for (const ts::Feature& feature : features) {
    parts += static_cast<int64_t>(feature.parts.size());
  }
  return parts;
}

} // namespace

/**
 * Unit tests for ts/wfs: capabilities documents (tests/data/wfs), source
 * and request building, GetFeature answers in GeoJSON and GML with their
 * axis orders, style rules, and the feature provider against a mock WFS
 * (paging, caching, offline, picking).
 */
class TestWfs : public QObject
{
  Q_OBJECT

private slots:
  void realCapabilities_data()
  {
    QTest::addColumn<QString>("file");
    QTest::addColumn<int32_t>("types");
    QTest::addColumn<QString>("firstType");
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("paging");
    QTest::addColumn<QString>("format");
    QTest::newRow("PDOK (MapServer)")
        << "pdok-200.xml" << 3 << "bestuurlijkegebieden:Gemeentegebied"
        << "service.pdok.nl" << true << "application/json";
    QTest::newRow("EMODnet (GeoServer)")
        << "emodnet-200.xml" << 0 << "emodnet:download_tiles"
        << "ows.emodnet-bathymetry.eu" << true << "application/json";
    QTest::newRow("GeoMet (MapServer, trimmed)")
        << "geomet-200.xml" << 8 << "ec-msc:AQHI-OBS" << "geo.weather.gc.ca"
        << true << "";
  }

  void realCapabilities()
  {
    QFETCH(QString, file);
    QFETCH(int32_t, types);
    QFETCH(QString, firstType);
    QFETCH(QString, host);
    QFETCH(bool, paging);
    QFETCH(QString, format);
    QString error;
    const std::optional<ts::WfsCapabilities> capabilities =
        ts::parseWfsCapabilities(fixture(QStringLiteral("wfs/") + file),
                                 &error);
    if (!capabilities) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(capabilities->version, QStringLiteral("2.0.0"));
    QVERIFY(!capabilities->title.isEmpty());
    if (types > 0) {
      QCOMPARE(static_cast<int32_t>(capabilities->featureTypes.size()), types);
    } else {
      QVERIFY(capabilities->featureTypes.size() >= 5);
    }
    QCOMPARE(capabilities->featureTypes.front().name, firstType);
    QCOMPARE(capabilities->getFeatureUrl.host(), host);
    QCOMPARE(capabilities->resultPaging, paging);
    for (const ts::WfsFeatureType& type : capabilities->featureTypes) {
      QVERIFY(!type.title.isEmpty());
      QVERIFY(type.defaultCrs.contains(QLatin1String("4326")) ||
              type.defaultCrs.contains(QLatin1String("28992")) ||
              type.defaultCrs.contains(QLatin1String("3857")) ||
              !type.defaultCrs.isEmpty());
    }
    if (!format.isEmpty()) {
      QCOMPARE(ts::chooseWfsOutputFormat(capabilities->outputFormats), format);
    }
    const std::optional<ts::WfsSource> source =
        ts::wfsSourceFor(*capabilities, firstType, &error);
    QVERIFY2(source.has_value() || format.isEmpty(), qPrintable(error));
    if (source) {
      QCOMPARE(source->url.host(), host);
      QCOMPARE(source->typeName, firstType);
      QVERIFY(!source->attribution.isEmpty());
      QVERIFY(source->minimumLevel <= source->maximumLevel);
    }
  }

  void capabilityDetails()
  {
    QString error;
    const std::optional<ts::WfsCapabilities> pdok = ts::parseWfsCapabilities(
        fixture(QStringLiteral("wfs/pdok-200.xml")), &error);
    if (!pdok) {
      QFAIL("pdok is missing");
    }
    QCOMPARE(pdok->countDefault, int64_t{1000});
    QCOMPARE(pdok->providerName, QStringLiteral("PDOK"));
    const ts::WfsFeatureType* province = pdok->findFeatureType(
        QStringLiteral("bestuurlijkegebieden:Provinciegebied"));
    if (province == nullptr) {
      QFAIL("province is missing");
    }
    if (!province->wgs84Bounds) {
      QFAIL("no bounds");
    }
    // The Netherlands.
    QVERIFY(province->wgs84Bounds->westDeg > 2.0 &&
            province->wgs84Bounds->eastDeg < 8.0);
    QVERIFY(province->wgs84Bounds->southDeg > 50.0 &&
            province->wgs84Bounds->northDeg < 56.0);
    // Its minimum level: one tile covers the country.
    const int32_t level = ts::wfsMinimumLevel(province->wgs84Bounds);
    QVERIFY(ts::TileKey::sizeDeg(level) >=
            std::max(province->wgs84Bounds->widthDeg(),
                     province->wgs84Bounds->heightDeg()));
    QVERIFY(ts::TileKey::sizeDeg(level + 1) <
            std::max(province->wgs84Bounds->widthDeg(),
                     province->wgs84Bounds->heightDeg()));
    QCOMPARE(ts::wfsMinimumLevel(std::nullopt), 0);
    QCOMPARE(ts::wfsMinimumLevel(ts::GeoRect{-180.0, -90.0, 180.0, 90.0}), 0);
    QVERIFY(pdok->findFeatureType(QStringLiteral("nothing")) == nullptr);
  }

  void notCapabilities_data()
  {
    QTest::addColumn<QByteArray>("document");
    QTest::addColumn<QString>("message");
    QTest::newRow("web page")
        << QByteArray("<!DOCTYPE html><html></html>") << "web page";
    QTest::newRow("WMS") << fixture(QStringLiteral("wms/terrestris-130.xml"))
                         << "Add Web Map Service (WMS) Layer";
    QTest::newRow("exception")
        << QByteArray("<ows:ExceptionReport xmlns:ows=\"http://www.opengis.net/"
                      "ows/1.1\"><ows:Exception><ows:ExceptionText>Service "
                      "down</ows:ExceptionText></ows:Exception>"
                      "</ows:ExceptionReport>")
        << "Service down";
    QTest::newRow("not XML") << QByteArray("hello") << "XML";
    QTest::newRow("other XML") << QByteArray("<a/>") << "WFS";
  }

  void notCapabilities()
  {
    QFETCH(QByteArray, document);
    QFETCH(QString, message);
    QString error;
    QVERIFY(!ts::parseWfsCapabilities(document, &error));
    QVERIFY2(error.contains(message), qPrintable(error));
  }

  void capabilitiesAddress()
  {
    const QUrl url = ts::wfsCapabilitiesUrl(
        QUrl(QStringLiteral("https://example.org/ows?map=a&service=WMS")));
    const QUrlQuery query(url);
    QCOMPARE(query.queryItemValue(QStringLiteral("map")), QStringLiteral("a"));
    QCOMPARE(query.queryItemValue(QStringLiteral("SERVICE")),
             QStringLiteral("WFS"));
    QVERIFY(!query.hasQueryItem(QStringLiteral("service")));
    QCOMPARE(query.queryItemValue(QStringLiteral("REQUEST")),
             QStringLiteral("GetCapabilities"));
  }

  void outputFormats()
  {
    QCOMPARE(ts::chooseWfsOutputFormat(
                 {QStringLiteral("GML2"), QStringLiteral("application/json")}),
             QStringLiteral("application/json"));
    QCOMPARE(ts::chooseWfsOutputFormat(
                 {QStringLiteral("text/xml; subtype=gml/3.1.1"),
                  QStringLiteral("application/gml+xml; version=3.2")}),
             QStringLiteral("application/gml+xml; version=3.2"));
    QCOMPARE(ts::chooseWfsOutputFormat(
                 {QStringLiteral("SHAPE-ZIP"), QStringLiteral("GML2")}),
             QStringLiteral("GML2"));
    QCOMPARE(ts::chooseWfsOutputFormat({QStringLiteral("csv")}), QString());
  }

  void requests()
  {
    ts::WfsSource source;
    source.url = QUrl(QStringLiteral("https://example.org/wfs?map=x"));
    source.typeName = QStringLiteral("a:b");
    source.outputFormat = QStringLiteral("application/json");
    source.pageSize = 250;
    const QUrl url =
        ts::getFeatureUrl(source, ts::GeoRect{-10.0, 40.0, 5.5, 50.0}, 500);
    const QUrlQuery query(url);
    QCOMPARE(query.queryItemValue(QStringLiteral("map")), QStringLiteral("x"));
    QCOMPARE(query.queryItemValue(QStringLiteral("REQUEST")),
             QStringLiteral("GetFeature"));
    QCOMPARE(query.queryItemValue(QStringLiteral("TYPENAMES")),
             QStringLiteral("a:b"));
    // Latitude first, as urn:ogc:def:crs:EPSG::4326 has it.
    QCOMPARE(query.queryItemValue(QStringLiteral("BBOX"), QUrl::FullyDecoded),
             QStringLiteral("40,-10,50,5.5,urn:ogc:def:crs:EPSG::4326"));
    QCOMPARE(
        query.queryItemValue(QStringLiteral("SRSNAME"), QUrl::FullyDecoded),
        QStringLiteral("urn:ogc:def:crs:EPSG::4326"));
    QCOMPARE(query.queryItemValue(QStringLiteral("COUNT")),
             QStringLiteral("250"));
    QCOMPARE(query.queryItemValue(QStringLiteral("STARTINDEX")),
             QStringLiteral("500"));
    QCOMPARE(query.queryItemValue(QStringLiteral("OUTPUTFORMAT"),
                                  QUrl::FullyDecoded),
             QStringLiteral("application/json"));
    // Without paging, and in WFS 1.1.0.
    source.paging = false;
    source.version = QStringLiteral("1.1.0");
    const QUrlQuery old(ts::getFeatureUrl(source, ts::GeoRect{0, 0, 1, 1}, 0));
    QVERIFY(old.hasQueryItem(QStringLiteral("TYPENAME")));
    QVERIFY(old.hasQueryItem(QStringLiteral("MAXFEATURES")));
    QVERIFY(!old.hasQueryItem(QStringLiteral("STARTINDEX")));
  }

  void sourceJson()
  {
    ts::WfsSource source;
    source.url = QUrl(QStringLiteral("https://example.org/wfs"));
    source.service = QUrl(QStringLiteral("https://example.org/wfs?x=1"));
    source.typeName = QStringLiteral("a:b");
    source.outputFormat = QStringLiteral("application/json");
    source.pageSize = 300;
    source.maximumPages = 3;
    source.bounds = ts::GeoRect{1.0, 2.0, 3.0, 4.0};
    source.minimumLevel = 5;
    source.maximumLevel = 9;
    source.title = QStringLiteral("B");
    source.attribution = QStringLiteral("Someone");
    source.styleRules = QJsonArray{
        QJsonObject{{QStringLiteral("attribute"), QStringLiteral("class")},
                    {QStringLiteral("equals"), QStringLiteral("odd")},
                    {QStringLiteral("outline"), QStringLiteral("#ff0000ff")}}};
    QString error;
    const std::optional<ts::WfsSource> back =
        ts::WfsSource::fromJson(source.toJson(), &error);
    if (!back) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(back->toJson(), source.toJson());
    QCOMPARE(back->id(), source.id());
    // The levels: none below the minimum, the maximum's ancestor beyond it.
    QCOMPARE(back->storedLevel(4), -1);
    QCOMPARE(back->storedLevel(5), 5);
    QCOMPARE(back->storedLevel(12), 9);
    // Rules change the features, so the id.
    ts::WfsSource plain = source;
    plain.styleRules = QJsonArray();
    QVERIFY(plain.id() != source.id());
    QJsonObject broken = source.toJson();
    broken.remove(QStringLiteral("typeName"));
    QVERIFY(!ts::WfsSource::fromJson(broken, &error));
    broken = source.toJson();
    broken.insert(QStringLiteral("kind"), QStringLiteral("wms"));
    QVERIFY(!ts::WfsSource::fromJson(broken, &error));
  }

  void realFeatures()
  {
    // GeoServer's GeoJSON names urn:…EPSG::4326 yet writes longitude
    // first; its GML 3.2 writes latitude first: the same features.
    QString error;
    const std::optional<ts::WfsPage> json = ts::parseWfsFeatures(
        fixture(QStringLiteral("wfs/emodnet-features.json")),
        QStringLiteral("application/json"), &error);
    if (!json) {
      QFAIL(qPrintable(error));
    }
    const std::optional<ts::WfsPage> gml = ts::parseWfsFeatures(
        fixture(QStringLiteral("wfs/emodnet-features.xml")),
        QStringLiteral("application/gml+xml; version=3.2"), &error);
    if (!gml) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(json->features.size(), std::size_t{2});
    QCOMPARE(gml->features.size(), std::size_t{2});
    QCOMPARE(json->returned, int64_t{2});
    QCOMPARE(gml->returned, int64_t{2});
    QCOMPARE(json->matched.value_or(0), int64_t{7});
    QCOMPARE(gml->matched.value_or(0), int64_t{7});
    for (std::size_t i = 0; i < 2; ++i) {
      const ts::Feature& a = json->features.at(i);
      const ts::Feature& b = gml->features.at(i);
      QCOMPARE(a.type, ts::Feature::Type::LineStrings);
      QCOMPARE(b.type, ts::Feature::Type::LineStrings);
      QCOMPARE(a.parts.front().size(), b.parts.front().size());
      for (std::size_t p = 0; p < a.parts.front().size(); ++p) {
        QCOMPARE(a.parts.front().at(p).lonDeg, b.parts.front().at(p).lonDeg);
        QCOMPARE(a.parts.front().at(p).latDeg, b.parts.front().at(p).latDeg);
      }
      QCOMPARE(a.attributes.value(QStringLiteral("name")).toString(),
               b.attributes.value(QStringLiteral("name")).toString());
    }
    const ts::GeoPoint start = json->features.front().parts.front().front();
    QCOMPARE(start.lonDeg, 2.3312);
    QCOMPARE(start.latDeg, 52.9078);
    QCOMPARE(json->features.front().id, QStringLiteral("sea_names.36"));
    QCOMPARE(gml->features.front().id, QStringLiteral("sea_names.36"));
    QCOMPARE(json->features.front()
                 .attributes.value(QStringLiteral("name"))
                 .toString(),
             QStringLiteral("North Sea"));

    // PDOK's GML 3.2 polygon (thinned): Zuid-Holland, latitude first.
    const std::optional<ts::WfsPage> pdok = ts::parseWfsFeatures(
        fixture(QStringLiteral("wfs/pdok-features.xml")), QString(), &error);
    if (!pdok) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(pdok->features.size(), std::size_t{1});
    const ts::Feature& province = pdok->features.front();
    QCOMPARE(province.type, ts::Feature::Type::Polygon);
    QVERIFY(province.parts.front().size() > 10);
    for (const ts::GeoPoint& point : province.parts.front()) {
      QVERIFY2(
          point.latDeg > 51.0 && point.latDeg < 53.0 && point.lonDeg > 3.0 &&
              point.lonDeg < 6.0,
          qPrintable(
              QStringLiteral("%1 %2").arg(point.latDeg).arg(point.lonDeg)));
    }
    QCOMPARE(province.attributes.value(QStringLiteral("naam")).toString(),
             QStringLiteral("Zuid-Holland"));
    QVERIFY(province.id.startsWith(QLatin1String("Provinciegebied.")));
  }

  void axisOrders_data()
  {
    QTest::addColumn<QString>("srsName");
    QTest::addColumn<bool>("latitudeFirst");
    QTest::newRow("urn") << "urn:ogc:def:crs:EPSG::4326" << true;
    QTest::newRow("OGC URL")
        << "http://www.opengis.net/def/crs/EPSG/0/4326" << true;
    QTest::newRow("EPSG code") << "EPSG:4326" << false;
    QTest::newRow("epsg.xml")
        << "http://www.opengis.net/gml/srs/epsg.xml#4326" << false;
    QTest::newRow("CRS84") << "urn:ogc:def:crs:OGC:1.3:CRS84" << false;
    QTest::newRow("none (as asked)") << "" << true;
  }

  void axisOrders()
  {
    QFETCH(QString, srsName);
    QFETCH(bool, latitudeFirst);
    const QString srs = srsName.isEmpty()
                            ? QString()
                            : QStringLiteral(" srsName=\"%1\"").arg(srsName);
    const QByteArray document =
        QStringLiteral(
            "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs/"
            "2.0\" xmlns:gml=\"http://www.opengis.net/gml/3.2\" "
            "xmlns:t=\"t\"><wfs:member><t:x gml:id=\"x.1\"><t:g><gml:Point%1>"
            "<gml:pos>10 50</gml:pos></gml:Point></t:g></t:x></wfs:member>"
            "</wfs:FeatureCollection>")
            .arg(srs)
            .toUtf8();
    QString error;
    const std::optional<ts::WfsPage> page =
        ts::parseWfsFeatures(document, QStringLiteral("text/xml"), &error);
    if (!page) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(page->features.size(), std::size_t{1});
    const ts::GeoPoint point = page->features.front().parts.front().front();
    QCOMPARE(point.latDeg, latitudeFirst ? 10.0 : 50.0);
    QCOMPARE(point.lonDeg, latitudeFirst ? 50.0 : 10.0);
  }

  void otherGml()
  {
    QString error;
    // GML 2: featureMember, outerBoundaryIs, coordinates (x,y).
    const QByteArray gml2 =
        "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
        "xmlns:gml=\"http://www.opengis.net/gml\" xmlns:t=\"t\">"
        "<gml:featureMember><t:lake fid=\"lake.1\"><t:the_geom>"
        "<gml:Polygon srsName=\"EPSG:4326\"><gml:outerBoundaryIs>"
        "<gml:LinearRing><gml:coordinates>0,0 1,0 1,1 0,1 0,0"
        "</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs>"
        "<gml:innerBoundaryIs><gml:LinearRing><gml:coordinates>0.4,0.4 0.6,0.4 "
        "0.6,0.6 0.4,0.4</gml:coordinates></gml:LinearRing>"
        "</gml:innerBoundaryIs></gml:Polygon></t:the_geom><t:name>Pond"
        "</t:name></t:lake></gml:featureMember></wfs:FeatureCollection>";
    const std::optional<ts::WfsPage> page =
        ts::parseWfsFeatures(gml2, QStringLiteral("GML2"), &error);
    if (!page) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(page->features.size(), std::size_t{1});
    QCOMPARE(page->features.front().parts.size(), std::size_t{2});
    QCOMPARE(page->features.front().parts.front().at(1).lonDeg, 1.0);
    QCOMPARE(page->features.front().id, QStringLiteral("lake.1"));
    QCOMPARE(page->returned, int64_t{1});
    // A three-dimensional posList and a Curve of segments.
    const QByteArray curve =
        "<wfs:FeatureCollection xmlns:wfs=\"w\" xmlns:gml=\"g\" xmlns:t=\"t\">"
        "<wfs:member><t:r gml:id=\"r.1\"><t:g><gml:Curve srsDimension=\"3\">"
        "<gml:segments><gml:LineStringSegment><gml:posList>1 2 0 3 4 0"
        "</gml:posList></gml:LineStringSegment><gml:LineStringSegment>"
        "<gml:posList>3 4 0 5 6 0</gml:posList></gml:LineStringSegment>"
        "</gml:segments></gml:Curve></t:g></t:r></wfs:member>"
        "</wfs:FeatureCollection>";
    const std::optional<ts::WfsPage> lines =
        ts::parseWfsFeatures(curve, QStringLiteral("text/xml"), &error);
    if (!lines) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(lines->features.size(), std::size_t{1});
    QCOMPARE(lines->features.front().parts.front().size(), std::size_t{3});
    QCOMPARE(lines->features.front().parts.front().back().latDeg, 5.0);
    // Errors.
    QVERIFY(!ts::parseWfsFeatures(
        "<ows:ExceptionReport "
        "xmlns:ows=\"o\"><ows:Exception><ows:ExceptionText>"
        "Too many</ows:ExceptionText></ows:Exception></ows:ExceptionReport>",
        QString(), &error));
    QVERIFY(error.contains(QLatin1String("Too many")));
    QVERIFY(!ts::parseWfsFeatures(
        "{\"type\":", QStringLiteral("application/json"), &error));
    QVERIFY(!ts::parseWfsFeatures("<a><b></a>", QStringLiteral("text/xml"),
                                  &error));
  }

  void styleRules()
  {
    const std::vector<ts::StyleRule> rules = ts::styleRulesFromJson(QJsonArray{
        QJsonObject{{QStringLiteral("attribute"), QStringLiteral("class")},
                    {QStringLiteral("equals"), QStringLiteral("motorway")},
                    {QStringLiteral("outline"), QStringLiteral("#ff0000ff")},
                    {QStringLiteral("outlineWidthPx"), 3}},
        QJsonObject{{QStringLiteral("attribute"), QStringLiteral("depth")},
                    {QStringLiteral("min"), 100},
                    {QStringLiteral("max"), 1000},
                    {QStringLiteral("fill"), QStringLiteral("#0000ff80")}},
        QJsonObject{{QStringLiteral("attribute"), QStringLiteral("x")}},
        QJsonObject{{QStringLiteral("pointSizePx"), 9}}});
    // The rule without a style is dropped; the last matches everything.
    QCOMPARE(rules.size(), std::size_t{3});
    std::vector<ts::Feature> features(5);
    features[0].attributes.insert(QStringLiteral("class"),
                                  QStringLiteral("motorway"));
    features[1].attributes.insert(QStringLiteral("depth"), 500);
    features[2].attributes.insert(QStringLiteral("depth"), 1000);
    features[3].type = ts::Feature::Type::LineStrings;
    features[3].attributes.insert(QStringLiteral("stroke"),
                                  QStringLiteral("#00ff00"));
    features[4].type = ts::Feature::Type::Polygon;
    features[4].attributes.insert(QStringLiteral("fill"),
                                  QStringLiteral("#123456"));
    features[4].attributes.insert(QStringLiteral("fill-opacity"), 0.5);
    ts::applyStyleRules(features, rules);
    QCOMPARE(features[0]
                 .style.value_or(ts::FeatureStyle())
                 .outline.value_or(QColor()),
             QColor(255, 0, 0));
    QCOMPARE(features[0]
                 .style.value_or(ts::FeatureStyle())
                 .outlineWidthPx.value_or(0.0),
             3.0);
    QCOMPARE(
        features[1].style.value_or(ts::FeatureStyle()).fill.value_or(QColor()),
        QColor(0, 0, 255, 128));
    // Beyond the range: the catch-all rule.
    QCOMPARE(features[2]
                 .style.value_or(ts::FeatureStyle())
                 .pointSizePx.value_or(0.0),
             9.0);
    QVERIFY(!features[2].style.value_or(ts::FeatureStyle()).fill.has_value());
    QCOMPARE(features[3]
                 .style.value_or(ts::FeatureStyle())
                 .pointSizePx.value_or(0.0),
             9.0);
    // Without rules: their own simplestyle colours.
    ts::applyStyleRules(features, {});
    QVERIFY(!features[0].style.has_value());
    QCOMPARE(features[3]
                 .style.value_or(ts::FeatureStyle())
                 .outline.value_or(QColor()),
             QColor(0, 255, 0));
    QCOMPARE(features[4]
                 .style.value_or(ts::FeatureStyle())
                 .fill.value_or(QColor())
                 .alpha(),
             128);
    // JSON round trip.
    const std::optional<ts::StyleRule> back =
        ts::StyleRule::fromJson(rules.at(1).toJson());
    if (!back) {
      QFAIL("back is missing");
    }
    QVERIFY(back->style == rules.at(1).style);
    QCOMPARE(back->minimum.value_or(0.0), 100.0);
  }

  void meshRunsAndPoints()
  {
    // Features with their own styles are drawn in runs of one style each;
    // points become dots (segments of no length) after the lines.
    ts::FeatureTile tile;
    tile.key = ts::TileKey{3, 8, 2};
    const ts::GeoRect rect = tile.key.bounds();
    const double lon = rect.centreLonDeg();
    const double lat = rect.centreLatDeg();
    ts::Feature line;
    line.type = ts::Feature::Type::LineStrings;
    line.parts = {{ts::GeoPoint{lat, lon - 5.0}, ts::GeoPoint{lat, lon + 5.0}}};
    ts::Feature red = line;
    red.parts = {{ts::GeoPoint{lat + 1.0, lon - 5.0},
                  ts::GeoPoint{lat + 1.0, lon + 5.0}}};
    red.style =
        ts::FeatureStyle{std::nullopt, QColor(Qt::red), 3.0, std::nullopt};
    ts::Feature points;
    points.type = ts::Feature::Type::Points;
    points.parts = {{ts::GeoPoint{lat, lon}},
                    {ts::GeoPoint{lat + 2.0, lon}},
                    {ts::GeoPoint{lat + 80.0, lon}}}; // outside the tile
    tile.features = {red, line, points};
    ts::VectorMesher mesher;
    const std::optional<ts::VectorTileMesh> mesh =
        mesher.build(tile, notCancelled);
    if (!mesh) {
      QFAIL("mesh is missing");
    }
    QCOMPARE(mesh->styles.size(), std::size_t{1});
    // Layer-style lines, red lines, then the points.
    QCOMPARE(mesh->segmentRuns.size(), std::size_t{3});
    QCOMPARE(mesh->segmentRuns.at(0).style, 0);
    QCOMPARE(mesh->segmentRuns.at(1).style, 1);
    QVERIFY(mesh->segmentRuns.at(2).points);
    QCOMPARE(mesh->segmentRuns.at(2).count, 2);
    int32_t covered = 0;
    for (const ts::VectorRun& run : mesh->segmentRuns) {
      QCOMPARE(run.first, covered);
      covered += run.count;
    }
    QCOMPARE(covered, static_cast<int32_t>(mesh->segments.size()));
    const ts::VectorSegment& dot = mesh->segments.at(
        static_cast<std::size_t>(mesh->segmentRuns.at(2).first));
    QCOMPARE(dot.a.offsetM, dot.b.offsetM);
    QVERIFY(mesh->fillRuns.empty());
  }

  void providerLoadsAndPages()
  {
    // The whole grid in one stored tile: 400 features, 100 a page, so
    // four requests and a fifth that comes back short (STARTINDEX 400).
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    ts::WfsSource source = mockSource(server, QStringLiteral("test:points"));
    QCOMPARE(source.pageSize, 100);
    QVERIFY(source.paging);
    source.minimumLevel = 2;
    source.maximumLevel = 2;
    ts::WfsFeatureProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    // Zoomed out beyond the minimum: nothing, nothing asked.
    QVERIFY(!provider.loadFeatures(ts::TileKey{1, 1, 0}, notCancelled));
    QCOMPARE(server.requestCount(), 0);
    // Quadtree tile (2, 3, 1) is 0..45° E, 0..45° N; (2, 4, 2) the
    // south-east of the grid's centre.
    const ts::TileKey key =
        ts::TileKey::containing(4, ts::Geodetic{5.0, 5.0, 0.0});
    QCOMPARE(provider.storedKey(key).level, 2);
    const std::optional<ts::FeatureTile> tile =
        provider.loadFeatures(key, notCancelled);
    if (!tile) {
      QFAIL("tile is missing");
    }
    QCOMPARE(tile->key, provider.storedKey(key));
    // The grid's north-east quarter: 10 × 10 cells.
    QCOMPARE(static_cast<int32_t>(tile->features.size()), 100);
    std::vector<ts::test::MockWfsRequest> asked =
        ts::test::mockWfsRequests(server);
    // A full page asks for the next, which is empty.
    QCOMPARE(static_cast<int32_t>(asked.size()), 2);
    QCOMPARE(asked.at(1).start, int64_t{100});
    QCOMPARE(asked.front().south, 0.0);
    QCOMPARE(asked.front().west, 0.0);
    QCOMPARE(asked.front().north, 45.0);
    QCOMPARE(asked.front().east, 45.0);
    for (const ts::Feature& feature : tile->features) {
      QCOMPARE(feature.type, ts::Feature::Type::Points);
      QVERIFY(feature.parts.front().front().lonDeg >= 0.0);
    }

    // The whole grid at level 0: four full pages, then a short one.
    source.minimumLevel = 0;
    source.maximumLevel = 0;
    ts::WfsFeatureProvider world(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const int32_t before = server.requestCount();
    const std::optional<ts::FeatureTile> west =
        world.loadFeatures(ts::TileKey{0, 0, 0}, notCancelled);
    const std::optional<ts::FeatureTile> east =
        world.loadFeatures(ts::TileKey{0, 1, 0}, notCancelled);
    if (!west || !east) {
      QFAIL("a half is missing");
    }
    QCOMPARE(
        static_cast<int32_t>(west->features.size() + east->features.size()),
        400);
    QCOMPARE(world.status().state, ts::ProviderStatus::State::Ok);
    QVERIFY(server.requestCount() - before >= 5);

    // Pages run out: the status says so.
    source.maximumPages = 2;
    ts::WfsFeatureProvider capped(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const std::optional<ts::FeatureTile> part =
        capped.loadFeatures(ts::TileKey{0, 1, 0}, notCancelled);
    if (!part) {
      QFAIL("part is missing");
    }
    QCOMPARE(static_cast<int32_t>(part->features.size()), 200);
    QVERIFY2(capped.status().message.contains(QLatin1String("zoom in")),
             qPrintable(capped.status().message));
  }

  void jsonAndGmlAgree_data()
  {
    QTest::addColumn<QString>("type");
    QTest::newRow("points") << "test:points";
    QTest::newRow("roads") << "test:roads";
    QTest::newRow("areas") << "test:areas";
  }

  void jsonAndGmlAgree()
  {
    QFETCH(QString, type);
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    ts::WfsSource json = mockSource(server, type);
    ts::WfsSource gml = mockSource(
        server, type, QStringLiteral("application/gml+xml; version=3.2"));
    for (ts::WfsSource* source : {&json, &gml}) {
      source->minimumLevel = 3;
      source->maximumLevel = 3;
    }
    const ts::TileKey key =
        ts::TileKey::containing(3, ts::Geodetic{-5.0, -5.0, 0.0});
    ts::WfsFeatureProvider a(
        json, std::make_shared<ts::NetworkClient>(testNetwork()));
    ts::WfsFeatureProvider b(
        gml, std::make_shared<ts::NetworkClient>(testNetwork()));
    const std::optional<ts::FeatureTile> fromJson =
        a.loadFeatures(key, notCancelled);
    const std::optional<ts::FeatureTile> fromGml =
        b.loadFeatures(key, notCancelled);
    if (!fromJson || !fromGml) {
      QFAIL("features are missing");
    }
    const std::vector<ts::Feature>& jsonFeatures = fromJson->features;
    const std::vector<ts::Feature>& gmlFeatures = fromGml->features;
    QVERIFY(!jsonFeatures.empty());
    QCOMPARE(jsonFeatures.size(), gmlFeatures.size());
    QCOMPARE(countParts(jsonFeatures), countParts(gmlFeatures));
    for (std::size_t i = 0; i < jsonFeatures.size(); ++i) {
      const ts::Feature& x = jsonFeatures.at(i);
      const ts::Feature& y = gmlFeatures.at(i);
      QCOMPARE(x.id, y.id);
      QCOMPARE(x.type, y.type);
      QCOMPARE(x.parts.front().front().lonDeg, y.parts.front().front().lonDeg);
      QCOMPARE(x.parts.front().front().latDeg, y.parts.front().front().latDeg);
      QCOMPARE(x.attributes.value(QStringLiteral("name")).toString(),
               y.attributes.value(QStringLiteral("name")).toString());
    }
  }

  void providerCachesAndGoesOffline()
  {
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    ts::WfsSource source;
    const ts::TileKey key =
        ts::TileKey::containing(3, ts::Geodetic{-5.0, -5.0, 0.0});
    {
      std::shared_ptr<MockWmsBehaviour> behaviour =
          std::make_shared<MockWmsBehaviour>();
      behaviour->maxAgeS = 3600;
      MockHttpServer server;
      server.setHandler(ts::test::mockWfsHandler(
          behaviour, server.url(QStringLiteral("/wfs"))));
      source = mockSource(server, QStringLiteral("test:areas"));
      source.minimumLevel = 3;
      source.maximumLevel = 3;
      ts::WfsFeatureProvider provider(
          source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
      QVERIFY(provider.loadFeatures(key, notCancelled).has_value());
      const int32_t asked = server.requestCount();
      ts::WfsFeatureProvider again(
          source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
      QVERIFY(again.loadFeatures(key, notCancelled).has_value());
      QCOMPARE(server.requestCount(), asked);
      QCOMPARE(again.statistics().requests, 0);
      // A server error: failed, not stored.
      behaviour->failStatus = 500;
      ts::WfsSource other = source;
      other.typeName = QStringLiteral("test:roads");
      ts::WfsFeatureProvider failing(
          other, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
      QVERIFY(!failing.loadFeatures(key, notCancelled).has_value());
      QCOMPARE(failing.status().state, ts::ProviderStatus::State::Error);
    }
    // The server is gone: the features come from the disk.
    ts::WfsFeatureProvider offline(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    const std::optional<ts::FeatureTile> tile =
        offline.loadFeatures(key, notCancelled);
    if (!tile) {
      QFAIL("tile is missing");
    }
    QVERIFY(!tile->features.empty());
  }

  void picking()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    const ts::TileKey key =
        ts::TileKey::containing(3, ts::Geodetic{-5.0, -5.0, 0.0});
    // Areas: inside the 0.5° square of cell (−6, −6), not beside it.
    ts::WfsSource areas = mockSource(server, QStringLiteral("test:areas"));
    areas.minimumLevel = 3;
    areas.maximumLevel = 3;
    ts::WfsFeatureProvider provider(
        areas, std::make_shared<ts::NetworkClient>(testNetwork()));
    QVERIFY(provider.featuresAt(ts::GeoPoint{-5.5, -5.5}, 0.01).empty());
    QVERIFY(provider.loadFeatures(key, notCancelled).has_value());
    const std::vector<ts::Feature> inside =
        provider.featuresAt(ts::GeoPoint{-5.5, -5.5}, 0.01);
    QCOMPARE(inside.size(), std::size_t{1});
    QCOMPARE(inside.front().attributes.value(QStringLiteral("name")).toString(),
             QStringLiteral("cell -6 -6"));
    QVERIFY(provider.featuresAt(ts::GeoPoint{-5.9, -5.9}, 0.01).empty());
    // Lines and points within the tolerance.
    ts::WfsSource roads = areas;
    roads.typeName = QStringLiteral("test:roads");
    ts::WfsFeatureProvider lines(
        roads, std::make_shared<ts::NetworkClient>(testNetwork()));
    QVERIFY(lines.loadFeatures(key, notCancelled).has_value());
    QCOMPARE(lines.featuresAt(ts::GeoPoint{-5.9 + 0.005, -5.5}, 0.01).size(),
             std::size_t{1});
    QVERIFY(lines.featuresAt(ts::GeoPoint{-5.5, -5.5}, 0.01).empty());
    ts::WfsSource points = areas;
    points.typeName = QStringLiteral("test:points");
    ts::WfsFeatureProvider dots(
        points, std::make_shared<ts::NetworkClient>(testNetwork()));
    QVERIFY(dots.loadFeatures(key, notCancelled).has_value());
    QCOMPARE(dots.featuresAt(ts::GeoPoint{-6.0, -6.004}, 0.01).size(),
             std::size_t{1});
  }

  void styleRulesOnLoad()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    ts::WfsSource source = mockSource(server, QStringLiteral("test:areas"));
    source.minimumLevel = 3;
    source.maximumLevel = 3;
    source.styleRules = QJsonArray{
        QJsonObject{{QStringLiteral("attribute"), QStringLiteral("class")},
                    {QStringLiteral("equals"), QStringLiteral("odd")},
                    {QStringLiteral("fill"), QStringLiteral("#ff000080")}}};
    ts::WfsFeatureProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const std::optional<ts::FeatureTile> tile = provider.loadFeatures(
        ts::TileKey::containing(3, ts::Geodetic{-5.0, -5.0, 0.0}),
        notCancelled);
    if (!tile) {
      QFAIL("tile is missing");
    }
    int32_t styled = 0;
    for (const ts::Feature& feature : tile->features) {
      const bool odd =
          feature.attributes.value(QStringLiteral("class")).toString() ==
          QLatin1String("odd");
      QCOMPARE(feature.style.has_value(), odd);
      styled += odd ? 1 : 0;
    }
    QVERIFY(styled > 0);
    // Meshed in two runs: the layer's style and the rule's.
    ts::VectorMesher mesher;
    const std::optional<ts::VectorTileMesh> mesh =
        mesher.build(*tile, notCancelled);
    if (!mesh) {
      QFAIL("mesh is missing");
    }
    QCOMPARE(mesh->styles.size(), std::size_t{1});
    QCOMPARE(mesh->fillRuns.size(), std::size_t{2});
  }

  void regionFetch()
  {
    QTemporaryDir cache;
    ts::WebTileSettings settings;
    settings.cacheDirectory = cache.path();
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    behaviour->maxAgeS = 3600;
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    ts::WfsSource source = mockSource(server, QStringLiteral("test:points"));
    source.minimumLevel = 0;
    source.maximumLevel = 0;
    ts::WfsFeatureProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()), settings);
    std::atomic<bool> cancelled{false};
    const ts::RegionTileResult first =
        provider.fetchForRegion(ts::TileKey{0, 1, 0}, 7, cancelled);
    QCOMPARE(first.outcome, ts::RegionTileResult::Outcome::Downloaded);
    QVERIFY(first.bytes > 0);
    const int32_t asked = server.requestCount();
    // Every page: the east half's 200 features, two full pages and the
    // short one after them.
    QCOMPARE(asked, 3);
    const ts::RegionTileResult again =
        provider.fetchForRegion(ts::TileKey{2, 5, 1}, 7, cancelled);
    QCOMPARE(again.outcome, ts::RegionTileResult::Outcome::AlreadyCached);
    QCOMPARE(server.requestCount(), asked);
    QVERIFY(provider.diskCache()->regionUsage(7).tiles >= 3);
    QCOMPARE(provider.serviceUrl().host(), source.url.host());
  }

  void registryCreatesWfs()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    const ts::WfsSource source =
        mockSource(server, QStringLiteral("test:points"));
    ts::ProviderRegistry registry;
    registry.setWmsNetwork(std::make_shared<ts::NetworkClient>(testNetwork()),
                           ts::WebTileSettings{});
    QString error;
    const std::shared_ptr<ts::FeatureProvider> provider =
        registry.features(source.toJson(), &error);
    if (!provider) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(std::dynamic_pointer_cast<ts::WfsFeatureProvider>(provider));
    QVERIFY(std::dynamic_pointer_cast<ts::RegionTileSource>(provider));
    QCOMPARE(registry.features(source.toJson(), &error), provider);
    QCOMPARE(registry.attribution(source.toJson()),
             QStringLiteral("Mock Survey"));
    QJsonObject broken = source.toJson();
    broken.remove(QStringLiteral("url"));
    QVERIFY(!registry.features(broken, &error));
  }

  void concurrentOwnInstances()
  {
    // Parsing is reentrant: each thread its own answers.
    const QByteArray json =
        fixture(QStringLiteral("wfs/emodnet-features.json"));
    const QByteArray gml = fixture(QStringLiteral("wfs/emodnet-features.xml"));
    const int32_t failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&json, &gml](int32_t) {
          for (int32_t i = 0; i < 50; ++i) {
            QString error;
            const std::optional<ts::WfsPage> a = ts::parseWfsFeatures(
                json, QStringLiteral("application/json"), &error);
            const std::optional<ts::WfsPage> b =
                ts::parseWfsFeatures(gml, QString(), &error);
            if (!a || !b || a->features.size() != b->features.size()) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void concurrentSharedProvider()
  {
    // The provider is thread-safe: loader threads share it.
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    MockHttpServer server;
    server.setHandler(ts::test::mockWfsHandler(
        behaviour, server.url(QStringLiteral("/wfs"))));
    ts::WfsSource source = mockSource(server, QStringLiteral("test:roads"));
    source.minimumLevel = 4;
    source.maximumLevel = 4;
    ts::WfsFeatureProvider provider(
        source, std::make_shared<ts::NetworkClient>(testNetwork()));
    const int32_t failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&provider](int32_t thread) {
          const ts::TileKey key = ts::TileKey::containing(
              4, ts::Geodetic{-9.0 + thread * 2.0, -9.0 + thread * 2.0, 0.0});
          const std::optional<ts::FeatureTile> tile =
              provider.loadFeatures(key, notCancelled);
          provider.featuresAt(ts::GeoPoint{0.0, 0.0}, 0.1);
          return tile.has_value() && !tile->features.empty();
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkParseGeoJson()
  {
    std::shared_ptr<MockWmsBehaviour> behaviour =
        std::make_shared<MockWmsBehaviour>();
    QStringList parts;
    for (const ts::test::MockWfsFeature& feature :
         ts::test::mockWfsFeatures(QStringLiteral("test:areas"))) {
      parts.append(
          ts::test::mockWfsFeature(QStringLiteral("test:areas"), feature).json);
    }
    const QByteArray body =
        (QStringLiteral("{\"type\": \"FeatureCollection\", \"features\": [") +
         parts.join(QStringLiteral(", ")) + QStringLiteral("]}"))
            .toUtf8();
    QBENCHMARK
    {
      QString error;
      QVERIFY(
          ts::parseWfsFeatures(body, QStringLiteral("application/json"), &error)
              .has_value());
    }
  }
};

QTEST_GUILESS_MAIN(TestWfs)
#include "tst_wfs.moc"
