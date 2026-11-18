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
 * A mock WMTS server for tests: capabilities and tiles on 127.0.0.1, KVP
 * and RESTful, in three tile matrix sets.
 */
#ifndef TS_TESTS_MOCKWMTS_H
#define TS_TESTS_MOCKWMTS_H

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

#include <QBuffer>
#include <QImage>
#include <QStringList>
#include <QUrlQuery>

#include "mockhttpserver.h"
#include "mockwms.h"

namespace ts::test
{

/**
 * What the mock WMTS does besides MockWmsBehaviour's failures (which
 * apply to tiles); change it from the test at any time.
 */
struct MockWmtsBehaviour {
  /** The WMS behaviour: delay, failures, caching headers. */
  MockWmsBehaviour wms;
  /** Answer tiles with 404 Not Found (an area without tiles). */
  std::atomic<bool> notFound{false};
};

/** One of the mock's tile matrix sets. */
struct MockWmtsSet {
  /** Identifier. */
  const char* identifier;
  /** SupportedCRS as written in the capabilities. */
  const char* crs;
  /** True for Web Mercator (metres), false for degrees. */
  bool mercator;
  /** Tile size, pixels. */
  int32_t tileSize;
  /** Columns of matrix 0 (doubling at each level). */
  int32_t columns0;
  /** Rows of matrix 0 (doubling at each level). */
  int32_t rows0;
  /** Size of a pixel of matrix 0, CRS units. */
  double pixel0;
  /** Top-left corner, easting or longitude. */
  double originX;
  /** Top-left corner, northing or latitude. */
  double originY;
};

/** Number of matrices in each set (0 to 9). */
constexpr int32_t mockWmtsMatrices = 10;

/** The mock's sets: aligned EPSG:4326, Web Mercator, unaligned EPSG:4326. */
inline const std::array<MockWmtsSet, 3>& mockWmtsSets()
{
  constexpr double a = 6378137.0;
  constexpr double pi = 3.14159265358979323846;
  static const std::array<MockWmtsSet, 3> sets = {{
      // Lines up with the quadtree: tile (z, x, y) is quadtree tile
      // (z, x, y).
      {"WorldCRS84Quad", "urn:ogc:def:crs:OGC:1.3:CRS84", false, 256, 2, 1,
       180.0 / 256.0, -180.0, 90.0},
      // Zoom z + 1 lines up with quadtree level z, as Web Mercator images.
      {"GoogleMapsCompatible", "urn:ogc:def:crs:EPSG::3857", true, 256, 1, 1,
       2.0 * pi * a / 256.0, -pi * a, pi * a},
      // 200-pixel tiles from a corner off the quadtree's meridians: every
      // quadtree tile takes pieces of several. Latitude first, as
      // EPSG:4326 is.
      {"Offset4326", "urn:ogc:def:crs:EPSG::4326", false, 200, 3, 2,
       180.0 / 256.0, -183.5, 90.0},
  }};
  return sets;
}

/** Scale denominator of a pixel of @p size in @p set's units. */
inline double mockWmtsScale(const MockWmtsSet& set, double size)
{
  constexpr double metresPerDegree = 111319.49079327357;
  return size * (set.mercator ? 1.0 : metresPerDegree) / 0.00028;
}

/** The capabilities document, with tile addresses on @p server. */
inline QByteArray mockWmtsCapabilities(const QUrl& server)
{
  const QString base = server.toString(QUrl::RemovePath | QUrl::RemoveQuery);
  QString xml =
      QStringLiteral(
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<Capabilities xmlns=\"http://www.opengis.net/wmts/1.0\" "
          "xmlns:ows=\"http://www.opengis.net/ows/1.1\" "
          "xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.0.0\">\n"
          "<ows:ServiceIdentification><ows:Title>Mock WMTS</ows:Title>"
          "<ows:Abstract>A test pattern</ows:Abstract>"
          "<ows:ServiceType>OGC WMTS</ows:ServiceType>"
          "<ows:ServiceTypeVersion>1.0.0</ows:ServiceTypeVersion>"
          "<ows:Fees>none</ows:Fees>"
          "<ows:AccessConstraints>Tests only</ows:AccessConstraints>"
          "</ows:ServiceIdentification>\n"
          "<ows:ServiceProvider><ows:ProviderName>Terra Soar tests"
          "</ows:ProviderName></ows:ServiceProvider>\n"
          "<ows:OperationsMetadata><ows:Operation name=\"GetTile\"><ows:DCP>"
          "<ows:HTTP><ows:Get xlink:href=\"%1/kvp?\"><ows:Constraint "
          "name=\"GetEncoding\"><ows:AllowedValues><ows:Value>KVP</ows:Value>"
          "</ows:AllowedValues></ows:Constraint></ows:Get></ows:HTTP></ows:DCP>"
          "</ows:Operation></ows:OperationsMetadata>\n<Contents>\n"
          "<Layer><ows:Title>Test pattern</ows:Title>"
          "<ows:WGS84BoundingBox><ows:LowerCorner>-180 -90</ows:LowerCorner>"
          "<ows:UpperCorner>180 90</ows:UpperCorner></ows:WGS84BoundingBox>"
          "<ows:Identifier>pattern</ows:Identifier>"
          "<Style isDefault=\"true\"><ows:Identifier>default</ows:Identifier>"
          "</Style><Format>image/png</Format><Format>image/jpeg</Format>"
          "<Dimension><ows:Identifier>Time</ows:Identifier>"
          "<Default>2026-01-01</Default><Value>2026-01-01</Value>"
          "<Value>2026-02-01</Value></Dimension>")
          .arg(base);
  for (const MockWmtsSet& set : mockWmtsSets()) {
    xml += QStringLiteral("<TileMatrixSetLink><TileMatrixSet>%1"
                          "</TileMatrixSet></TileMatrixSetLink>")
               .arg(QLatin1String(set.identifier));
  }
  // RESTful for PNG; JPEG only through KVP.
  xml += QStringLiteral(
             "<ResourceURL format=\"image/png\" resourceType=\"tile\" "
             "template=\"%1/rest/pattern/{Style}/{Time}/{TileMatrixSet}/"
             "{TileMatrix}/{TileRow}/{TileCol}.png\"/></Layer>\n")
             .arg(base);
  for (const MockWmtsSet& set : mockWmtsSets()) {
    xml += QStringLiteral("<TileMatrixSet><ows:Identifier>%1</ows:Identifier>"
                          "<ows:SupportedCRS>%2</ows:SupportedCRS>")
               .arg(QLatin1String(set.identifier), QLatin1String(set.crs));
    for (int32_t z = 0; z < mockWmtsMatrices; ++z) {
      const double factor = std::ldexp(1.0, z);
      const QString corner =
          QString::fromLatin1(set.crs).endsWith(QLatin1String("::4326"))
              ? QStringLiteral("%1 %2")
                    .arg(set.originY, 0, 'g', 17)
                    .arg(set.originX, 0, 'g', 17)
              : QStringLiteral("%1 %2")
                    .arg(set.originX, 0, 'g', 17)
                    .arg(set.originY, 0, 'g', 17);
      xml += QStringLiteral(
                 "<TileMatrix><ows:Identifier>%1</ows:Identifier>"
                 "<ScaleDenominator>%2</ScaleDenominator>"
                 "<TopLeftCorner>%3</TopLeftCorner><TileWidth>%4</TileWidth>"
                 "<TileHeight>%4</TileHeight><MatrixWidth>%5</MatrixWidth>"
                 "<MatrixHeight>%6</MatrixHeight></TileMatrix>")
                 .arg(z)
                 .arg(mockWmtsScale(set, set.pixel0 / factor), 0, 'g', 17)
                 .arg(corner)
                 .arg(set.tileSize)
                 .arg(static_cast<int64_t>(set.columns0) << z)
                 .arg(static_cast<int64_t>(set.rows0) << z);
    }
    xml += QStringLiteral("</TileMatrixSet>\n");
  }
  xml += QStringLiteral("</Contents>\n</Capabilities>\n");
  return xml.toUtf8();
}

/**
 * Draws tile (@p column, @p row) of matrix @p z of @p set: the mock WMS's
 * picture of the Earth (mockWmsColour()), from each pixel's position, so
 * a correct client draws the same Earth from every set; on white if
 * @p opaque (for JPEG, which has no transparency).
 */
// Matrix, column and row, as WMTS names them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline QImage mockWmtsTile(const MockWmtsSet& set, int32_t z, int64_t column,
                           int64_t row, bool opaque = false)
{
  constexpr double a = 6378137.0;
  constexpr double degrees = 180.0 / 3.14159265358979323846;
  const double pixel = set.pixel0 / std::ldexp(1.0, z);
  QImage image(set.tileSize, set.tileSize, QImage::Format_ARGB32);
  for (int32_t y = 0; y < set.tileSize; ++y) {
    const double northing =
        set.originY -
        (static_cast<double>(row * set.tileSize + y) + 0.5) * pixel;
    const double lat =
        set.mercator ? std::atan(std::sinh(northing / a)) * degrees : northing;
    for (int32_t x = 0; x < set.tileSize; ++x) {
      const double easting =
          set.originX +
          (static_cast<double>(column * set.tileSize + x) + 0.5) * pixel;
      const double lon = set.mercator ? easting / a * degrees : easting;
      image.setPixel(x, y,
                     std::abs(lat) > 90.0 || std::abs(lon) > 180.0
                         ? qRgba(0, 0, 0, 0)
                         : mockWmsColour(lon, lat, !opaque));
    }
  }
  return image;
}

/** A tile request, from either kind of address. */
struct MockWmtsTileRequest {
  /** Index into mockWmtsSets(). */
  int32_t set = 0;
  /** Matrix. */
  int32_t z = 0;
  /** Column. */
  int64_t column = 0;
  /** Row. */
  int64_t row = 0;
  /** "image/png" or "image/jpeg". */
  QString format;
  /** The Time dimension's value. */
  QString time;
};

/** Reads a KVP or RESTful tile request; nothing if it isn't one. */
inline std::optional<MockWmtsTileRequest>
mockWmtsTileRequest(const MockHttpRequest& request)
{
  MockWmtsTileRequest tile;
  QString setName;
  bool zOk = false;
  bool columnOk = false;
  bool rowOk = false;
  const QString path = request.url.path();
  if (path == QLatin1String("/kvp")) {
    const QUrlQuery query(request.url);
    if (query.queryItemValue(QStringLiteral("REQUEST")) !=
        QLatin1String("GetTile")) {
      return std::nullopt;
    }
    setName = query.queryItemValue(QStringLiteral("TILEMATRIXSET"));
    tile.z = query.queryItemValue(QStringLiteral("TILEMATRIX")).toInt(&zOk);
    tile.column =
        query.queryItemValue(QStringLiteral("TILECOL")).toLongLong(&columnOk);
    tile.row =
        query.queryItemValue(QStringLiteral("TILEROW")).toLongLong(&rowOk);
    tile.format =
        query.queryItemValue(QStringLiteral("FORMAT"), QUrl::FullyDecoded);
    tile.time = query.queryItemValue(QStringLiteral("Time"));
  } else if (path.startsWith(QLatin1String("/rest/pattern/"))) {
    // /rest/pattern/{Style}/{Time}/{Set}/{z}/{row}/{col}.png
    const QStringList parts = path.split(QLatin1Char('/'));
    if (parts.size() != 9) {
      return std::nullopt;
    }
    tile.time = parts.at(4);
    setName = parts.at(5);
    tile.z = parts.at(6).toInt(&zOk);
    tile.row = parts.at(7).toLongLong(&rowOk);
    QString column = parts.at(8);
    column.chop(4);
    tile.column = column.toLongLong(&columnOk);
    tile.format = QStringLiteral("image/png");
  } else {
    return std::nullopt;
  }
  const std::array<MockWmtsSet, 3>& sets = mockWmtsSets();
  tile.set = -1;
  for (std::size_t i = 0; i < sets.size(); ++i) {
    if (setName == QLatin1String(sets.at(i).identifier)) {
      tile.set = static_cast<int32_t>(i);
    }
  }
  if (tile.set < 0 || !zOk || !columnOk || !rowOk || tile.z < 0 ||
      tile.z >= mockWmtsMatrices) {
    return std::nullopt;
  }
  const MockWmtsSet& set = sets.at(static_cast<std::size_t>(tile.set));
  if (tile.column < 0 || tile.row < 0 ||
      tile.column >= (static_cast<int64_t>(set.columns0) << tile.z) ||
      tile.row >= (static_cast<int64_t>(set.rows0) << tile.z)) {
    return std::nullopt;
  }
  return tile;
}

/**
 * Returns a handler that serves the mock WMTS: capabilities at
 * /WMTSCapabilities.xml and by KVP at /kvp, tiles by KVP (/kvp) and
 * RESTful (/rest/…), behaving as @p behaviour says.
 */
inline MockHttpServer::Handler
mockWmtsHandler(const std::shared_ptr<MockWmtsBehaviour>& behaviour,
                const QUrl& server)
{
  return [behaviour, server](const MockHttpRequest& request) {
    const QUrlQuery query(request.url);
    if (request.url.path() == QLatin1String("/WMTSCapabilities.xml") ||
        query.queryItemValue(QStringLiteral("REQUEST")) ==
            QLatin1String("GetCapabilities")) {
      return mockResponse(200, mockWmtsCapabilities(server), "text/xml");
    }
    const std::optional<MockWmtsTileRequest> tile =
        mockWmtsTileRequest(request);
    if (!tile) {
      return mockResponse(
          400,
          "<?xml version=\"1.0\"?><ows:ExceptionReport "
          "xmlns:ows=\"http://www.opengis.net/ows/1.1\"><ows:Exception "
          "exceptionCode=\"TileOutOfRange\"><ows:ExceptionText>No such "
          "tile</ows:ExceptionText></ows:Exception></ows:ExceptionReport>",
          "text/xml");
    }
    if (behaviour->notFound.load()) {
      MockHttpResponse response = mockResponse(404, "Not Found", "text/plain");
      response.delayMs = behaviour->wms.delayMs.load();
      return response;
    }
    if (std::optional<MockHttpResponse> failure =
            mockWmsFailure(behaviour->wms)) {
      return std::move(*failure);
    }
    MockHttpResponse response;
    response.delayMs = behaviour->wms.delayMs.load();
    const QByteArray etag =
        QStringLiteral("\"%1\"")
            .arg(qHash(request.url.toString(QUrl::FullyEncoded)))
            .toLatin1();
    if (behaviour->wms.etag.load() && request.header("if-none-match") == etag) {
      response.status = 304;
      return response;
    }
    const bool jpeg = tile->format == QLatin1String("image/jpeg");
    const QImage image =
        mockWmtsTile(mockWmtsSets().at(static_cast<std::size_t>(tile->set)),
                     tile->z, tile->column, tile->row, jpeg);
    QBuffer buffer(&response.body);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, jpeg ? "JPG" : "PNG");
    response.status = 200;
    response.headers.emplace_back("Content-Type",
                                  jpeg ? "image/jpeg" : "image/png");
    const QByteArray cacheControl = mockWmsCacheControl(behaviour->wms);
    if (!cacheControl.isEmpty()) {
      response.headers.emplace_back("Cache-Control", cacheControl);
    }
    if (behaviour->wms.etag.load()) {
      response.headers.emplace_back("ETag", etag);
    }
    return response;
  };
}

/** Returns the tile requests @p server received. */
inline std::vector<MockWmtsTileRequest>
mockWmtsTileRequests(const MockHttpServer& server)
{
  std::vector<MockWmtsTileRequest> tiles;
  for (const MockHttpRequest& request : server.requests()) {
    const std::optional<MockWmtsTileRequest> tile =
        mockWmtsTileRequest(request);
    if (tile) {
      tiles.push_back(*tile);
    }
  }
  return tiles;
}

} // namespace ts::test

#endif // TS_TESTS_MOCKWMTS_H
