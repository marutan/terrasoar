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
 * A mock XYZ/TMS tile server for tests: the mock WMS's picture of the
 * Earth in Web Mercator tiles, at /xyz/{z}/{x}/{y}.png (rows from the
 * north), /tms/{z}/{x}/{y}.png (rows from the south) and
 * /{s}/xyz/{z}/{x}/{y}.png (any subdomain name in the path).
 */
#ifndef TS_TESTS_MOCKXYZ_H
#define TS_TESTS_MOCKXYZ_H

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QImage>
#include <QStringList>

#include "mockhttpserver.h"
#include "mockwmts.h"

namespace ts::test
{

/** Finest zoom the mock serves. */
constexpr int32_t mockXyzMaximumZoom = 9;

/** A tile request the mock XYZ server understood. */
struct MockXyzTileRequest {
  /** Zoom. */
  int32_t z = 0;
  /** Column. */
  int64_t x = 0;
  /** Row from the north, whatever the address's row order. */
  int64_t row = 0;
  /** True if it came by the TMS address. */
  bool tms = false;
  /** The subdomain in the path, or empty. */
  QString subdomain;
};

/** Reads a tile request; nothing if it isn't one. */
inline std::optional<MockXyzTileRequest>
mockXyzTileRequest(const MockHttpRequest& request)
{
  QStringList parts = request.url.path().split(QLatin1Char('/'));
  // "", [subdomain,] kind, z, x, y.png
  MockXyzTileRequest tile;
  if (parts.size() == 6) {
    tile.subdomain = parts.at(1);
    parts.removeAt(1);
  }
  if (parts.size() != 5 || !parts.at(4).endsWith(QLatin1String(".png"))) {
    return std::nullopt;
  }
  tile.tms = parts.at(1) == QLatin1String("tms");
  if (!tile.tms && parts.at(1) != QLatin1String("xyz")) {
    return std::nullopt;
  }
  bool zOk = false;
  bool xOk = false;
  bool yOk = false;
  QString y = parts.at(4);
  y.chop(4);
  tile.z = parts.at(2).toInt(&zOk);
  tile.x = parts.at(3).toLongLong(&xOk);
  const int64_t row = y.toLongLong(&yOk);
  if (!zOk || !xOk || !yOk || tile.z < 0 || tile.z > mockXyzMaximumZoom) {
    return std::nullopt;
  }
  const int64_t tiles = int64_t{1} << tile.z;
  if (tile.x < 0 || tile.x >= tiles || row < 0 || row >= tiles) {
    return std::nullopt;
  }
  tile.row = tile.tms ? tiles - 1 - row : row;
  return tile;
}

/**
 * Returns a handler serving the mock XYZ tiles, behaving as @p behaviour
 * says (404s, failures, delays, caching headers).
 */
inline MockHttpServer::Handler
mockXyzHandler(const std::shared_ptr<MockWmtsBehaviour>& behaviour)
{
  return [behaviour](const MockHttpRequest& request) {
    const std::optional<MockXyzTileRequest> tile = mockXyzTileRequest(request);
    if (!tile || behaviour->notFound.load()) {
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
    // The mock WMTS's GoogleMapsCompatible set: its matrix z is zoom z.
    const QImage image =
        mockWmtsTile(mockWmtsSets().at(1), tile->z, tile->x, tile->row);
    QBuffer buffer(&response.body);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    response.status = 200;
    response.headers.emplace_back("Content-Type", "image/png");
    const QByteArray cacheControl = mockWmsCacheControl(behaviour->wms);
    if (!cacheControl.isEmpty()) {
      response.headers.emplace_back("Cache-Control", cacheControl);
    }
    return response;
  };
}

/** Returns the tile requests @p server received. */
inline std::vector<MockXyzTileRequest>
mockXyzTileRequests(const MockHttpServer& server)
{
  std::vector<MockXyzTileRequest> tiles;
  for (const MockHttpRequest& request : server.requests()) {
    const std::optional<MockXyzTileRequest> tile = mockXyzTileRequest(request);
    if (tile) {
      tiles.push_back(*tile);
    }
  }
  return tiles;
}

} // namespace ts::test

#endif // TS_TESTS_MOCKXYZ_H
