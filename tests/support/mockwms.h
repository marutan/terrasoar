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
 * A mock WMS server for tests: capabilities and GetMap on 127.0.0.1.
 */
#ifndef TS_TESTS_MOCKWMS_H
#define TS_TESTS_MOCKWMS_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <QBuffer>
#include <QImage>
#include <QStringList>
#include <QUrlQuery>

#include "mockhttpserver.h"

namespace ts::test
{

/** What the mock WMS does; change it from the test at any time. */
struct MockWmsBehaviour {
  /** Delay before each GetMap answer, ms. */
  std::atomic<int32_t> delayMs{0};
  /**
   * Send half of each tile, then the rest this much later, ms (XYZ only,
   * MockHttpResponse::restDelayMs).
   */
  std::atomic<int32_t> restDelayMs{0};
  /**
   * Largest GetMap image drawn, pixels a side (0: any up to 4096); larger
   * ones get a ServiceException, as servers answer past their MaxWidth.
   */
  std::atomic<int32_t> maximumImagePx{0};
  /** Answer GetMap with this HTTP status instead (0: normally). */
  std::atomic<int32_t> failStatus{0};
  /** Answer GetMap with a ServiceExceptionReport (status 200). */
  std::atomic<bool> serviceException{false};
  /** Answer GetMap with an HTML page (status 200). */
  std::atomic<bool> html{false};
  /** Answer GetMap with 3 MB of bytes. */
  std::atomic<bool> oversized{false};
  /** Send Cache-Control: max-age=N (−1: no header). */
  std::atomic<int32_t> maxAgeS{-1};
  /** Send Cache-Control: no-store. */
  std::atomic<bool> noStore{false};
  /** Send an ETag and answer If-None-Match with 304. */
  std::atomic<bool> etag{false};
  /** Answer GetCapabilities for 1.3.0 with an exception (an old server). */
  std::atomic<bool> refuse130{false};
  /**
   * Offer a third layer, "clouds", with a TIME dimension: 1 to 5 November
   * 2026, daily (default the 5th).
   */
  std::atomic<bool> timeLayer{false};
  /**
   * Make "pattern" queryable and answer GetFeatureInfo (GeoJSON, HTML or
   * plain text) with the asked point's longitude and latitude (from BBOX,
   * I and J) and TIME.
   */
  std::atomic<bool> featureInfo{false};
};

/** Longitude and latitude to colour: the picture every GetMap draws. */
inline QRgb mockWmsColour(double lonDeg, double latDeg, bool transparent)
{
  // Red discs 8° across at every 30° intersection; yellow bands 3° wide
  // at 45° N and 60° E; elsewhere nothing (or white when not
  // transparent).
  const double nearestLat = 30.0 * std::round(latDeg / 30.0);
  const double nearestLon = 30.0 * std::round(lonDeg / 30.0);
  const double dLon =
      (lonDeg - nearestLon) * std::cos(latDeg * 3.14159265358979 / 180.0);
  const double dLat = latDeg - nearestLat;
  if (dLon * dLon + dLat * dLat < 4.0 * 4.0) {
    return qRgba(220, 30, 30, 230);
  }
  if (std::abs(latDeg - 45.0) < 1.5 || std::abs(lonDeg - 60.0) < 1.5) {
    return qRgba(250, 220, 40, 200);
  }
  return transparent ? qRgba(0, 0, 0, 0) : qRgba(255, 255, 255, 255);
}

/**
 * Draws the GetMap image for @p query (BBOX, CRS/SRS, VERSION, WIDTH,
 * HEIGHT, TRANSPARENT) pixel by pixel, honouring the axis order of the
 * version and CRS: a correct client gets the same picture of the Earth in
 * every CRS.
 */
inline QImage mockWmsImage(const QUrlQuery& query)
{
  const QString version = query.queryItemValue(QStringLiteral("VERSION"));
  QString crs = query.queryItemValue(QStringLiteral("CRS"));
  if (crs.isEmpty()) {
    crs = query.queryItemValue(QStringLiteral("SRS"));
  }
  const QStringList bbox =
      query.queryItemValue(QStringLiteral("BBOX")).split(QLatin1Char(','));
  const int32_t width = query.queryItemValue(QStringLiteral("WIDTH")).toInt();
  const int32_t height = query.queryItemValue(QStringLiteral("HEIGHT")).toInt();
  const bool transparent =
      query.queryItemValue(QStringLiteral("TRANSPARENT")).toUpper() ==
      QLatin1String("TRUE");
  if (bbox.size() != 4 || width <= 0 || height <= 0 || width > 4096 ||
      height > 4096) {
    return QImage();
  }
  double minX = bbox.at(0).toDouble();
  double minY = bbox.at(1).toDouble();
  double maxX = bbox.at(2).toDouble();
  double maxY = bbox.at(3).toDouble();
  const bool mercator = crs == QLatin1String("EPSG:3857");
  if (version.startsWith(QLatin1String("1.3")) &&
      crs == QLatin1String("EPSG:4326")) {
    // Latitude first: x is latitude.
    std::swap(minX, minY);
    std::swap(maxX, maxY);
  }
  QImage image(width, height, QImage::Format_ARGB32);
  const double a = 6378137.0;
  const double degrees = 180.0 / 3.14159265358979323846;
  for (int32_t row = 0; row < height; ++row) {
    const double y = maxY - (row + 0.5) * (maxY - minY) / height;
    const double lat = mercator ? std::atan(std::sinh(y / a)) * degrees : y;
    for (int32_t column = 0; column < width; ++column) {
      const double x = minX + (column + 0.5) * (maxX - minX) / width;
      const double lon = mercator ? x / a * degrees : x;
      image.setPixel(column, row, mockWmsColour(lon, lat, transparent));
    }
  }
  return image;
}

/** Returns capabilities of the mock WMS, @p version, served at @p url. */
inline QByteArray mockWmsCapabilities(const QString& version, const QUrl& url,
                                      const QString& crs,
                                      bool timeLayer = false,
                                      bool featureInfo = false)
{
  const bool v13 = version.startsWith(QLatin1String("1.3"));
  const QString crsElement =
      v13 ? QStringLiteral("CRS") : QStringLiteral("SRS");
  const QString href = url.toString(QUrl::FullyEncoded).toHtmlEscaped();
  QString xml;
  xml += v13 ? QStringLiteral("<WMS_Capabilities version=\"1.3.0\" "
                              "xmlns=\"http://www.opengis.net/wms\" "
                              "xmlns:xlink=\"http://www.w3.org/1999/xlink\">")
             : QStringLiteral("<WMT_MS_Capabilities version=\"1.1.1\">");
  xml += QStringLiteral("<Service><Name>WMS</Name><Title>Mock WMS</Title>"
                        "<Abstract>Test pattern</Abstract><Fees>none</Fees>"
                        "</Service>");
  xml += QStringLiteral(
             "<Capability><Request><GetMap><Format>image/png</Format>"
             "<Format>image/jpeg</Format><DCPType><HTTP><Get><OnlineResource "
             "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
             "xlink:href=\"%1\"/></Get></HTTP></DCPType></GetMap>")
             .arg(href);
  if (featureInfo) {
    xml += QStringLiteral(
               "<GetFeatureInfo><Format>text/plain</Format><Format>text/html"
               "</Format><Format>application/json</Format><DCPType><HTTP>"
               "<Get><OnlineResource "
               "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
               "xlink:href=\"%1\"/></Get></HTTP></DCPType></GetFeatureInfo>")
               .arg(href);
  }
  xml += QStringLiteral("</Request>");
  xml += QStringLiteral("<Layer><Title>Mock</Title><%1>%2</%1>")
             .arg(crsElement, crs);
  xml +=
      QStringLiteral("<Attribution><Title>Mock Agency</Title></Attribution>");
  xml += QStringLiteral("<Layer queryable=\"%1\"><Name>pattern</Name>")
             .arg(featureInfo ? 1 : 0);
  xml += QStringLiteral(""
                        "<Title>Test pattern</Title>"
                        "<Style><Name>plain</Name><Title>Plain</Title></Style>"
                        "</Layer>");
  xml += QStringLiteral("<Layer opaque=\"1\"><Name>background</Name>"
                        "<Title>Opaque background</Title></Layer>");
  if (timeLayer) {
    xml += v13 ? QStringLiteral(
                     "<Layer><Name>clouds</Name><Title>Daily clouds</Title>"
                     "<Dimension name=\"time\" units=\"ISO8601\" "
                     "default=\"2026-11-05\">2026-11-01/2026-11-05/P1D"
                     "</Dimension></Layer>")
               : QStringLiteral(
                     "<Layer><Name>clouds</Name><Title>Daily clouds</Title>"
                     "<Dimension name=\"time\" units=\"ISO8601\"/>"
                     "<Extent name=\"time\" default=\"2026-11-05\">"
                     "2026-11-01/2026-11-05/P1D</Extent></Layer>");
  }
  xml += QStringLiteral("</Layer></Capability>");
  xml += v13 ? QStringLiteral("</WMS_Capabilities>")
             : QStringLiteral("</WMT_MS_Capabilities>");
  return xml.toUtf8();
}

/** The capabilities document in the version @p query asks for. */
inline MockHttpResponse mockWmsCapabilitiesResponse(
    const MockHttpRequest& request, const QUrlQuery& query,
    const MockWmsBehaviour& behaviour, const QString& crs)
{
  QString version = query.queryItemValue(QStringLiteral("VERSION"));
  if (behaviour.refuse130.load() && version == QLatin1String("1.3.0")) {
    return mockResponse(
        200,
        "<?xml version=\"1.0\"?><ServiceExceptionReport version=\"1.1.1\">"
        "<ServiceException>Unsupported VERSION 1.3.0</ServiceException>"
        "</ServiceExceptionReport>",
        "application/vnd.ogc.se_xml");
  }
  if (version != QLatin1String("1.1.1")) {
    version = QStringLiteral("1.3.0");
  }
  QUrl self = request.url;
  self.setQuery(QString());
  self.setScheme(QStringLiteral("http"));
  self.setHost(QStringLiteral("127.0.0.1"));
  const QByteArray host = request.header("host");
  self.setPort(host.mid(host.lastIndexOf(':') + 1).toInt());
  return mockResponse(200,
                      mockWmsCapabilities(version, self, crs,
                                          behaviour.timeLayer.load(),
                                          behaviour.featureInfo.load()),
                      "text/xml");
}

/** The failure @p behaviour asks for instead of a GetMap image, if any. */
inline std::optional<MockHttpResponse>
mockWmsFailure(const MockWmsBehaviour& behaviour)
{
  constexpr int32_t threeMiB = 3 * 1024 * 1024;
  MockHttpResponse response;
  response.delayMs = behaviour.delayMs.load();
  if (behaviour.failStatus.load() != 0) {
    response.status = behaviour.failStatus.load();
    response.body = "failed";
    return response;
  }
  if (behaviour.serviceException.load()) {
    response.status = 200;
    response.headers.emplace_back("Content-Type", "application/vnd.ogc.se_xml");
    response.body =
        "<?xml version=\"1.0\"?><ServiceExceptionReport version=\"1.3.0\">"
        "<ServiceException code=\"LayerNotDefined\">Layer 'x' is not "
        "defined</ServiceException></ServiceExceptionReport>";
    return response;
  }
  if (behaviour.html.load()) {
    return mockResponse(200, "<html><body>Maintenance</body></html>",
                        "text/html");
  }
  if (behaviour.oversized.load()) {
    return mockResponse(200, QByteArray(threeMiB, 'x'), "image/png");
  }
  return std::nullopt;
}

/** The Cache-Control header @p behaviour asks for, if any. */
inline QByteArray mockWmsCacheControl(const MockWmsBehaviour& behaviour)
{
  if (behaviour.noStore.load()) {
    return QByteArrayLiteral("no-store");
  }
  if (behaviour.maxAgeS.load() >= 0) {
    return "max-age=" + QByteArray::number(behaviour.maxAgeS.load());
  }
  return QByteArray();
}

/** A GetMap answer: the image, or 304 if the client has it already. */
inline MockHttpResponse mockWmsGetMap(const MockHttpRequest& request,
                                      const QUrlQuery& query,
                                      const MockWmsBehaviour& behaviour)
{
  MockHttpResponse response;
  response.delayMs = behaviour.delayMs.load();
  const QByteArray cacheControl = mockWmsCacheControl(behaviour);
  const QByteArray etag = QStringLiteral("\"%1\"")
                              .arg(qHash(request.url.query(QUrl::FullyEncoded)))
                              .toLatin1();
  if (behaviour.etag.load() && request.header("if-none-match") == etag) {
    response.status = 304;
    // A 304 carries max-age only, never no-store.
    if (behaviour.maxAgeS.load() >= 0) {
      response.headers.emplace_back(
          "Cache-Control",
          "max-age=" + QByteArray::number(behaviour.maxAgeS.load()));
    }
    return response;
  }
  const QImage image = mockWmsImage(query);
  const bool jpeg = query.queryItemValue(QStringLiteral("FORMAT")) ==
                    QLatin1String("image/jpeg");
  QBuffer buffer(&response.body);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, jpeg ? "JPG" : "PNG");
  response.status = 200;
  response.headers.emplace_back("Content-Type",
                                jpeg ? "image/jpeg" : "image/png");
  if (!cacheControl.isEmpty()) {
    response.headers.emplace_back("Cache-Control", cacheControl);
  }
  if (behaviour.etag.load()) {
    response.headers.emplace_back("ETag", etag);
  }
  return response;
}

/**
 * Answers a GetFeatureInfo request: the point the pixel (I, J) or (X, Y)
 * of the BBOX stands for, in the INFO_FORMAT asked for (geographic CRSs).
 */
inline MockHttpResponse mockWmsFeatureInfo(const QUrlQuery& query)
{
  const QStringList box =
      query.queryItemValue(QStringLiteral("BBOX"), QUrl::FullyDecoded)
          .split(QLatin1Char(','));
  if (box.size() != 4) {
    return mockResponse(400, "bad BBOX", "text/plain");
  }
  const bool v13 =
      query.queryItemValue(QStringLiteral("VERSION")) == QLatin1String("1.3.0");
  const QString crs =
      query.queryItemValue(v13 ? QStringLiteral("CRS") : QStringLiteral("SRS"));
  const bool latFirst = v13 && crs == QLatin1String("EPSG:4326");
  const double west = box.at(latFirst ? 1 : 0).toDouble();
  const double south = box.at(latFirst ? 0 : 1).toDouble();
  const double east = box.at(latFirst ? 3 : 2).toDouble();
  const double north = box.at(latFirst ? 2 : 3).toDouble();
  const double width = query.queryItemValue(QStringLiteral("WIDTH")).toDouble();
  const double height =
      query.queryItemValue(QStringLiteral("HEIGHT")).toDouble();
  const double i =
      query.queryItemValue(v13 ? QStringLiteral("I") : QStringLiteral("X"))
          .toDouble();
  const double j =
      query.queryItemValue(v13 ? QStringLiteral("J") : QStringLiteral("Y"))
          .toDouble();
  const double lon = west + (i + 0.5) / width * (east - west);
  const double lat = north - (j + 0.5) / height * (north - south);
  const QString time = query.queryItemValue(QStringLiteral("TIME"));
  const QString format =
      query.queryItemValue(QStringLiteral("INFO_FORMAT"), QUrl::FullyDecoded);
  const QString lonText = QString::number(lon, 'f', 4);
  const QString latText = QString::number(lat, 'f', 4);
  if (format == QLatin1String("text/html")) {
    return mockResponse(
        200,
        QStringLiteral("<html><body><table><tr><th>lon</th><th>lat</th></tr>"
                       "<tr><td>%1</td><td>%2</td></tr></table></body></html>")
            .arg(lonText, latText)
            .toUtf8(),
        "text/html");
  }
  if (format == QLatin1String("text/plain")) {
    return mockResponse(200,
                        QStringLiteral("Layer 'pattern'\n  Feature 1:\n    "
                                       "lon = '%1'\n    lat = '%2'\n")
                            .arg(lonText, latText)
                            .toUtf8(),
                        "text/plain");
  }
  return mockResponse(
      200,
      QStringLiteral("{\"type\": \"FeatureCollection\", \"features\": "
                     "[{\"type\": \"Feature\", \"id\": \"pattern.1\", "
                     "\"properties\": {\"lon\": %1, \"lat\": %2, "
                     "\"time\": \"%3\"}}]}")
          .arg(lonText, latText, time)
          .toUtf8(),
      "application/json");
}

/**
 * Returns a handler that serves the mock WMS: GetCapabilities (in the
 * version asked for, offering @p crs) and GetMap (mockWmsImage(), as PNG or
 * JPEG), behaving as @p behaviour says.
 */
inline MockHttpServer::Handler
mockWmsHandler(const std::shared_ptr<MockWmsBehaviour>& behaviour,
               const QString& crs = QStringLiteral("EPSG:4326"))
{
  return [behaviour, crs](const MockHttpRequest& request) {
    const QUrlQuery query(request.url);
    if (query.queryItemValue(QStringLiteral("REQUEST")) ==
        QLatin1String("GetCapabilities")) {
      return mockWmsCapabilitiesResponse(request, query, *behaviour, crs);
    }
    if (std::optional<MockHttpResponse> failure = mockWmsFailure(*behaviour)) {
      return std::move(*failure);
    }
    if (query.queryItemValue(QStringLiteral("REQUEST")) ==
        QLatin1String("GetFeatureInfo")) {
      return mockWmsFeatureInfo(query);
    }
    const int32_t largest = behaviour->maximumImagePx.load();
    if (largest > 0 &&
        std::max(query.queryItemValue(QStringLiteral("WIDTH")).toInt(),
                 query.queryItemValue(QStringLiteral("HEIGHT")).toInt()) >
            largest) {
      MockHttpResponse response;
      response.status = 200;
      response.headers.emplace_back("Content-Type",
                                    "application/vnd.ogc.se_xml");
      response.body =
          "<?xml version=\"1.0\"?><ServiceExceptionReport version=\"1.3.0\">"
          "<ServiceException>Image too large</ServiceException>"
          "</ServiceExceptionReport>";
      return response;
    }
    return mockWmsGetMap(request, query, *behaviour);
  };
}

/** Returns how many GetMap requests @p server received. */
inline int32_t mockGetMapCount(const MockHttpServer& server)
{
  const std::vector<MockHttpRequest> requests = server.requests();
  return static_cast<int32_t>(std::count_if(
      requests.begin(), requests.end(), [](const MockHttpRequest& request) {
        return QUrlQuery(request.url)
                   .queryItemValue(QStringLiteral("REQUEST")) ==
               QLatin1String("GetMap");
      }));
}

} // namespace ts::test

#endif // TS_TESTS_MOCKWMS_H
