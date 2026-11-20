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
/**
 * @file
 * A mock WFS 2.0 for tests: three feature types on a grid (points, lines
 * and squares), GetFeature by BBOX with paging, as GeoJSON or GML 3.2.
 */
#ifndef TS_TESTS_MOCKWFS_H
#define TS_TESTS_MOCKWFS_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QStringList>
#include <QUrlQuery>

#include "mockhttpserver.h"
#include "mockwms.h"

namespace ts::test
{

/** One mock feature. */
struct MockWfsFeature {
  /** "points.12" and so on. */
  QString id;
  /** Its grid cell's longitude and latitude (south-west corner). */
  int32_t lon = 0;
  /** As lon. */
  int32_t lat = 0;
};

/**
 * The mock's features of @p type ("test:points", "test:roads",
 * "test:areas"): one per whole degree from −10 to 9 in both directions
 * (400), sorted by id. A point at the cell's corner, a road along its
 * southern edge to the next degree east, an area a 0.5° square in it.
 */
inline std::vector<MockWfsFeature> mockWfsFeatures(const QString& type)
{
  std::vector<MockWfsFeature> features;
  const QString prefix = type.section(QLatin1Char(':'), 1);
  int32_t n = 0;
  for (int32_t lat = -10; lat < 10; ++lat) {
    for (int32_t lon = -10; lon < 10; ++lon) {
      features.push_back(MockWfsFeature{
          QStringLiteral("%1.%2").arg(prefix).arg(n++), lon, lat});
    }
  }
  return features;
}

/** The capabilities document, GetFeature at @p url. */
inline QByteArray mockWfsCapabilities(const QUrl& url)
{
  const QString href = url.toString(QUrl::FullyEncoded).toHtmlEscaped();
  QString xml =
      QStringLiteral(
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          "<wfs:WFS_Capabilities version=\"2.0.0\" "
          "xmlns:wfs=\"http://www.opengis.net/wfs/2.0\" "
          "xmlns:ows=\"http://www.opengis.net/ows/1.1\" "
          "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
          "<ows:ServiceIdentification><ows:Title>Mock WFS</ows:Title>"
          "<ows:Abstract>A grid of features</ows:Abstract>"
          "<ows:ServiceType>WFS</ows:ServiceType>"
          "<ows:ServiceTypeVersion>2.0.0</ows:ServiceTypeVersion>"
          "<ows:Fees>NONE</ows:Fees><ows:AccessConstraints>Tests only"
          "</ows:AccessConstraints></ows:ServiceIdentification>"
          "<ows:ServiceProvider><ows:ProviderName>Mock "
          "Survey</ows:ProviderName>"
          "</ows:ServiceProvider><ows:OperationsMetadata>"
          "<ows:Operation name=\"GetFeature\"><ows:DCP><ows:HTTP><ows:Get "
          "xlink:href=\"%1\"/></ows:HTTP></ows:DCP>"
          "<ows:Parameter name=\"outputFormat\"><ows:AllowedValues>"
          "<ows:Value>application/gml+xml; version=3.2</ows:Value>"
          "<ows:Value>application/json</ows:Value></ows:AllowedValues>"
          "</ows:Parameter></ows:Operation>"
          "<ows:Constraint name=\"ImplementsResultPaging\"><ows:NoValues/>"
          "<ows:DefaultValue>TRUE</ows:DefaultValue></ows:Constraint>"
          "<ows:Constraint name=\"CountDefault\"><ows:NoValues/>"
          "<ows:DefaultValue>100</ows:DefaultValue></ows:Constraint>"
          "</ows:OperationsMetadata><wfs:FeatureTypeList>")
          .arg(href);
  const QStringList types{QStringLiteral("points"), QStringLiteral("roads"),
                          QStringLiteral("areas")};
  for (const QString& type : types) {
    xml += QStringLiteral(
               "<wfs:FeatureType><wfs:Name>test:%1</wfs:Name>"
               "<wfs:Title>Grid %1</wfs:Title><wfs:DefaultCRS>"
               "urn:ogc:def:crs:EPSG::4326</wfs:DefaultCRS>"
               "<ows:WGS84BoundingBox><ows:LowerCorner>-10 -10"
               "</ows:LowerCorner><ows:UpperCorner>10 10</ows:UpperCorner>"
               "</ows:WGS84BoundingBox></wfs:FeatureType>")
               .arg(type);
  }
  xml += QStringLiteral("</wfs:FeatureTypeList></wfs:WFS_Capabilities>");
  return xml.toUtf8();
}

/** The parts of one feature in a GetFeature answer. */
struct MockWfsAnswer {
  QString json;
  QString gml;
};

inline MockWfsAnswer mockWfsFeature(const QString& type,
                                    const MockWfsFeature& feature)
{
  const double x = feature.lon;
  const double y = feature.lat;
  const QString properties =
      QStringLiteral("\"name\": \"cell %1 %2\", \"class\": \"%3\"")
          .arg(feature.lon)
          .arg(feature.lat)
          .arg((feature.lon + feature.lat) % 2 == 0 ? QStringLiteral("even")
                                                    : QStringLiteral("odd"));
  QString jsonGeometry;
  QString gmlGeometry;
  if (type == QLatin1String("test:points")) {
    jsonGeometry =
        QStringLiteral("{\"type\": \"Point\", \"coordinates\": [%1, %2]}")
            .arg(x)
            .arg(y);
    gmlGeometry = QStringLiteral("<gml:Point><gml:pos>%2 %1</gml:pos>"
                                 "</gml:Point>")
                      .arg(x)
                      .arg(y);
  } else if (type == QLatin1String("test:roads")) {
    jsonGeometry = QStringLiteral("{\"type\": \"LineString\", \"coordinates\": "
                                  "[[%1, %2], [%3, %2]]}")
                       .arg(x + 0.1)
                       .arg(y + 0.1)
                       .arg(x + 0.9);
    gmlGeometry = QStringLiteral("<gml:LineString><gml:posList>%2 %1 %2 %3"
                                 "</gml:posList></gml:LineString>")
                      .arg(x + 0.1)
                      .arg(y + 0.1)
                      .arg(x + 0.9);
  } else {
    const QString ring = QStringLiteral("[[%1, %2], [%3, %2], [%3, %4], "
                                        "[%1, %4], [%1, %2]]")
                             .arg(x + 0.25)
                             .arg(y + 0.25)
                             .arg(x + 0.75)
                             .arg(y + 0.75);
    jsonGeometry =
        QStringLiteral("{\"type\": \"MultiPolygon\", \"coordinates\": [[%1]]}")
            .arg(ring);
    gmlGeometry =
        QStringLiteral(
            "<gml:MultiSurface><gml:surfaceMember><gml:Polygon><gml:exterior>"
            "<gml:LinearRing><gml:posList>%2 %1 %2 %3 %4 %3 %4 %1 %2 %1"
            "</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon>"
            "</gml:surfaceMember></gml:MultiSurface>")
            .arg(x + 0.25)
            .arg(y + 0.25)
            .arg(x + 0.75)
            .arg(y + 0.75);
  }
  MockWfsAnswer answer;
  answer.json = QStringLiteral("{\"type\": \"Feature\", \"id\": \"%1\", "
                               "\"geometry\": %2, \"properties\": {%3}}")
                    .arg(feature.id, jsonGeometry, properties);
  answer.gml =
      QStringLiteral(
          "<wfs:member><test:%1 gml:id=\"%2\"><test:geom>%3</test:geom>"
          "<test:name>cell %4 %5</test:name><test:class>%6</test:class>"
          "</test:%1></wfs:member>")
          .arg(type.section(QLatin1Char(':'), 1), feature.id, gmlGeometry)
          .arg(feature.lon)
          .arg(feature.lat)
          .arg((feature.lon + feature.lat) % 2 == 0 ? QStringLiteral("even")
                                                    : QStringLiteral("odd"));
  return answer;
}

/** A GetFeature request the mock understood. */
struct MockWfsRequest {
  QString type;
  double south = 0.0;
  double west = 0.0;
  double north = 0.0;
  double east = 0.0;
  int64_t count = 0;
  int64_t start = 0;
  QString format;
};

inline std::optional<MockWfsRequest> mockWfsRequest(const MockHttpRequest& r)
{
  const QUrlQuery query(r.url);
  if (query.queryItemValue(QStringLiteral("REQUEST")) !=
      QLatin1String("GetFeature")) {
    return std::nullopt;
  }
  MockWfsRequest request;
  request.type = query.queryItemValue(QStringLiteral("TYPENAMES"));
  const QStringList box =
      query.queryItemValue(QStringLiteral("BBOX"), QUrl::FullyDecoded)
          .split(QLatin1Char(','));
  if (box.size() < 4) {
    return std::nullopt;
  }
  // urn:ogc:def:crs:EPSG::4326: latitude first.
  request.south = box.at(0).toDouble();
  request.west = box.at(1).toDouble();
  request.north = box.at(2).toDouble();
  request.east = box.at(3).toDouble();
  request.count = query.queryItemValue(QStringLiteral("COUNT")).toLongLong();
  request.start =
      query.queryItemValue(QStringLiteral("STARTINDEX")).toLongLong();
  request.format =
      query.queryItemValue(QStringLiteral("OUTPUTFORMAT"), QUrl::FullyDecoded);
  return request;
}

/**
 * Returns a handler serving the mock WFS: capabilities, and GetFeature
 * answers (at most 100 a page), behaving as @p behaviour says (failures,
 * caching headers, delays).
 */
inline MockHttpServer::Handler
mockWfsHandler(const std::shared_ptr<MockWmsBehaviour>& behaviour,
               const QUrl& url)
{
  return [behaviour, url](const MockHttpRequest& request) {
    const QUrlQuery query(request.url);
    if (query.queryItemValue(QStringLiteral("REQUEST")) ==
        QLatin1String("GetCapabilities")) {
      return mockResponse(200, mockWfsCapabilities(url), "text/xml");
    }
    const std::optional<MockWfsRequest> asked = mockWfsRequest(request);
    if (!asked) {
      return mockResponse(400,
                          "<ows:ExceptionReport xmlns:ows=\"http://www.opengis."
                          "net/ows/1.1\"><ows:Exception><ows:ExceptionText>Bad "
                          "request</ows:ExceptionText></ows:Exception>"
                          "</ows:ExceptionReport>",
                          "text/xml");
    }
    if (std::optional<MockHttpResponse> failure = mockWmsFailure(*behaviour)) {
      return std::move(*failure);
    }
    std::vector<MockWfsFeature> inside;
    for (const MockWfsFeature& feature : mockWfsFeatures(asked->type)) {
      // By the feature's own extent (its cell).
      if (feature.lon + 1 > asked->west && feature.lon < asked->east &&
          feature.lat + 1 > asked->south && feature.lat < asked->north) {
        inside.push_back(feature);
      }
    }
    const int64_t count =
        std::min<int64_t>(asked->count > 0 ? asked->count : 100, 100);
    const int64_t first =
        std::min<int64_t>(asked->start, static_cast<int64_t>(inside.size()));
    const int64_t last =
        std::min<int64_t>(first + count, static_cast<int64_t>(inside.size()));
    const bool json = asked->format.contains(QLatin1String("json"));
    QStringList parts;
    for (int64_t i = first; i < last; ++i) {
      const MockWfsAnswer answer =
          mockWfsFeature(asked->type, inside.at(static_cast<std::size_t>(i)));
      parts.append(json ? answer.json : answer.gml);
    }
    MockHttpResponse response;
    response.status = 200;
    response.delayMs = behaviour->delayMs.load();
    if (json) {
      response.body =
          QStringLiteral("{\"type\": \"FeatureCollection\", \"numberMatched\": "
                         "%1, \"numberReturned\": %2, \"features\": [%3]}")
              .arg(inside.size())
              .arg(last - first)
              .arg(parts.join(QStringLiteral(", ")))
              .toUtf8();
      response.headers.emplace_back("Content-Type", "application/json");
    } else {
      response.body =
          QStringLiteral(
              "<?xml version=\"1.0\"?><wfs:FeatureCollection "
              "xmlns:wfs=\"http://www.opengis.net/wfs/2.0\" "
              "xmlns:gml=\"http://www.opengis.net/gml/3.2\" "
              "xmlns:test=\"http://example.invalid/test\" numberMatched=\"%1\" "
              "numberReturned=\"%2\">%3</wfs:FeatureCollection>")
              .arg(inside.size())
              .arg(last - first)
              .arg(parts.join(QString()))
              .toUtf8();
      response.headers.emplace_back("Content-Type",
                                    "application/gml+xml; version=3.2");
    }
    const QByteArray cacheControl = mockWmsCacheControl(*behaviour);
    if (!cacheControl.isEmpty()) {
      response.headers.emplace_back("Cache-Control", cacheControl);
    }
    return response;
  };
}

/** Returns the GetFeature requests @p server received. */
inline std::vector<MockWfsRequest> mockWfsRequests(const MockHttpServer& server)
{
  std::vector<MockWfsRequest> found;
  for (const MockHttpRequest& request : server.requests()) {
    const std::optional<MockWfsRequest> asked = mockWfsRequest(request);
    if (asked) {
      found.push_back(*asked);
    }
  }
  return found;
}

} // namespace ts::test

#endif // TS_TESTS_MOCKWFS_H
