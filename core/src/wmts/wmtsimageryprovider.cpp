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
#include "ts/wmts/wmtsimageryprovider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QXmlStreamReader>

#include "ts/network/networkclient.h"
#include "ts/wms/wmssource.h"
#include "ts/wmts/wmtscrs.h"

namespace ts
{

namespace
{

/** The message of an OWS ExceptionReport, or empty if it isn't one. */
QString exceptionMessage(const QByteArray& body)
{
  QXmlStreamReader reader(body);
  if (!reader.readNextStartElement() ||
      (reader.name() != QLatin1String("ExceptionReport") &&
       reader.name() != QLatin1String("ServiceExceptionReport"))) {
    return QString();
  }
  QStringList messages;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement() &&
        (reader.name() == QLatin1String("ExceptionText") ||
         reader.name() == QLatin1String("ServiceException"))) {
      messages.append(
          reader.readElementText(QXmlStreamReader::IncludeChildElements)
              .simplified());
    }
  }
  return messages.join(QLatin1Char(' ')).simplified();
}

QString statusErrorMessage(const HttpResponse& response)
{
  const QString exception = exceptionMessage(response.body);
  if (exception.isEmpty()) {
    return QCoreApplication::translate("ts::WmtsImageryProvider",
                                       "The server answered %1.")
        .arg(response.errorString);
  }
  return QCoreApplication::translate("ts::WmtsImageryProvider",
                                     "The server reported an error: %1")
      .arg(exception);
}

QString notAnImageMessage(const HttpResponse& response)
{
  const QString exception = exceptionMessage(response.body);
  if (exception.isEmpty()) {
    return QCoreApplication::translate("ts::WmtsImageryProvider",
                                       "The server sent something that "
                                       "isn't an image (%1).")
        .arg(QString::fromLatin1(response.header("content-type")));
  }
  return QCoreApplication::translate("ts::WmtsImageryProvider",
                                     "The server reported an error: %1")
      .arg(exception);
}

QString hostOf(const WmtsSource& source)
{
  if (!source.urlTemplate.isEmpty()) {
    return QUrl(source.urlTemplate).host();
  }
  return source.url.host();
}

/** Returns pixel (@p x, @p y) of @p image, transparent outside it. */
QRgb pixelAt(const QImage& image, int32_t x, int32_t y, int32_t width,
             int32_t height)
{
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return 0;
  }
  return reinterpret_cast<const QRgb*>(image.constScanLine(y))[x];
}

/**
 * Returns @p image's colour at (@p x, @p y) in pixel coordinates (pixel
 * centres at whole numbers), bilinear between the four pixels around it;
 * transparent outside. @p image is premultiplied ARGB32.
 */
QRgb sampleBilinear(const QImage& image, double x, double y, int32_t width,
                    int32_t height)
{
  if (!(x > -1.0) || !(y > -1.0) || !(x < width) || !(y < height)) {
    return 0;
  }
  const int32_t x0 = static_cast<int32_t>(std::floor(x));
  const int32_t y0 = static_cast<int32_t>(std::floor(y));
  const double fx = x - x0;
  const double fy = y - y0;
  const std::array<QRgb, 4> corners = {
      pixelAt(image, x0, y0, width, height),
      pixelAt(image, x0 + 1, y0, width, height),
      pixelAt(image, x0, y0 + 1, width, height),
      pixelAt(image, x0 + 1, y0 + 1, width, height)};
  const std::array<double, 4> weights = {(1 - fx) * (1 - fy), fx * (1 - fy),
                                         (1 - fx) * fy, fx * fy};
  std::array<double, 4> channels = {0.0, 0.0, 0.0, 0.0};
  for (std::size_t k = 0; k < 4; ++k) {
    channels[0] += weights.at(k) * qAlpha(corners.at(k));
    channels[1] += weights.at(k) * qRed(corners.at(k));
    channels[2] += weights.at(k) * qGreen(corners.at(k));
    channels[3] += weights.at(k) * qBlue(corners.at(k));
  }
  return qRgba(static_cast<int32_t>(std::lround(channels[1])),
               static_cast<int32_t>(std::lround(channels[2])),
               static_cast<int32_t>(std::lround(channels[3])),
               static_cast<int32_t>(std::lround(channels[0])));
}

/** Largest mosaic, pixels across: bigger blocks are drawn smaller. */
constexpr double maximumMosaicPx = 2048.0;

ImageLoadResult outcome(ImageLoadResult::Outcome kind)
{
  ImageLoadResult result;
  result.outcome = kind;
  return result;
}

} // namespace

WmtsImageryProvider::WmtsImageryProvider(WmtsSource source,
                                         std::shared_ptr<NetworkClient> network,
                                         WebTileSettings settings)
    : WmtsImageryProvider(source, source.id(), hostOf(source),
                          std::move(network), std::move(settings))
{
}

WmtsImageryProvider::WmtsImageryProvider(WmtsSource source, QString id,
                                         QString host,
                                         std::shared_ptr<NetworkClient> network,
                                         WebTileSettings settings)
    : m_source(std::move(source)), m_id(std::move(id)),
      m_fetcher(std::make_unique<CachedTileFetcher>(
          m_id, std::move(host), std::move(network), std::move(settings),
          CachedTileFetcher::Messages{statusErrorMessage, notAnImageMessage,
                                      true},
          [this](const ProviderStatus& status) { reportStatus(status); }))
{
}

WmtsImageryProvider::~WmtsImageryProvider() = default;

QString WmtsImageryProvider::id() const
{
  return m_id;
}

QUrl WmtsImageryProvider::tileUrl(const WmtsTileIndex& tile) const
{
  return getTileUrl(m_source, tile);
}

QString WmtsImageryProvider::name() const
{
  return m_source.title.isEmpty() ? m_source.layer : m_source.title;
}

int32_t WmtsImageryProvider::maximumLevel() const
{
  return m_source.maximumLevel;
}

int32_t WmtsImageryProvider::tileSizePx() const
{
  return m_source.tileSizePx;
}

int32_t WmtsImageryProvider::preferredLoaderThreads() const
{
  return m_fetcher->settings().loaderThreads;
}

TileImageProjection WmtsImageryProvider::imageProjection() const
{
  return m_source.imageProjection();
}

WebTileStatistics WmtsImageryProvider::statistics() const
{
  return m_fetcher->statistics();
}

QImage WmtsImageryProvider::loadImage(TileKey key,
                                      const std::atomic<bool>& cancelled)
{
  return loadTileImage(key, cancelled).image;
}

WmtsImageryProvider::Mosaic
WmtsImageryProvider::fetchMosaic(const WmtsTileWindow& window,
                                 const std::atomic<bool>& cancelled) const
{
  const WmtsMatrix& m =
      m_source.matrices.at(static_cast<std::size_t>(window.matrix));
  Mosaic mosaic;
  // The block of WMTS tiles, side by side; missing ones stay transparent.
  // A big block (a national grid's coarsest tiles under a coarse quadtree
  // tile) is drawn smaller: the quadtree tile can't show more anyway.
  const int32_t fullWidth = window.columns * m.tileWidth;
  const int32_t fullHeight = window.rows * m.tileHeight;
  mosaic.scale =
      std::min(1.0, maximumMosaicPx /
                        static_cast<double>(std::max(fullWidth, fullHeight)));
  mosaic.image = QImage(
      std::max(1, static_cast<int32_t>(std::ceil(fullWidth * mosaic.scale))),
      std::max(1, static_cast<int32_t>(std::ceil(fullHeight * mosaic.scale))),
      QImage::Format_ARGB32_Premultiplied);
  mosaic.image.fill(Qt::transparent);
  bool anyData = false;
  bool anyFailed = false;
  QPainter painter(&mosaic.image);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, mosaic.scale < 1.0);
  for (int32_t r = 0; r < window.rows; ++r) {
    for (int32_t c = 0; c < window.columns; ++c) {
      const WmtsTileIndex tileIndex{window.matrix, window.firstColumn + c,
                                    window.firstRow + r};
      if (!wmtsTileExists(m_source, tileIndex)) {
        continue;
      }
      QImage tile;
      // The disk cache keys WMTS tiles by (matrix, column, row).
      const TileKey cacheKey{window.matrix,
                             static_cast<int32_t>(tileIndex.column),
                             static_cast<int32_t>(tileIndex.row)};
      const FetchOutcome fetched = m_fetcher->fetch(
          cacheKey, tileUrl(tileIndex),
          [&tile](const QByteArray& data) {
            tile.loadFromData(data);
            return !tile.isNull();
          },
          cancelled);
      switch (fetched) {
      case FetchOutcome::Data: {
        // Its place in whole pixels, so neighbours meet without a seam; a
        // server sending the wrong size is scaled to fit it.
        const QRect place(
            QPoint(static_cast<int32_t>(
                       std::lround(c * m.tileWidth * mosaic.scale)),
                   static_cast<int32_t>(
                       std::lround(r * m.tileHeight * mosaic.scale))),
            QPoint(static_cast<int32_t>(
                       std::lround((c + 1) * m.tileWidth * mosaic.scale)) -
                       1,
                   static_cast<int32_t>(
                       std::lround((r + 1) * m.tileHeight * mosaic.scale)) -
                       1));
        if (place.size() == tile.size()) {
          painter.drawImage(place.topLeft(), tile);
        } else {
          painter.drawImage(place,
                            tile.scaled(place.size(), Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation));
        }
        anyData = true;
        break;
      }
      case FetchOutcome::NoData:
        break;
      case FetchOutcome::Cancelled:
        mosaic.outcome = ImageLoadResult::Outcome::Cancelled;
        return mosaic;
      case FetchOutcome::Failed:
        anyFailed = true;
        break;
      }
    }
  }
  if (anyFailed) {
    mosaic.outcome = ImageLoadResult::Outcome::Failed;
  } else if (anyData) {
    mosaic.outcome = ImageLoadResult::Outcome::Image;
  }
  return mosaic;
}

ImageLoadResult
WmtsImageryProvider::loadTileImage(TileKey key,
                                   const std::atomic<bool>& cancelled)
{
  if (isProjectedWmtsCrs(m_source.crs)) {
    return loadReprojected(key, cancelled);
  }
  const int32_t matrix = wmtsMatrixFor(m_source, key);
  const std::optional<WmtsTileWindow> window =
      wmtsTileWindow(m_source, key, matrix);
  if (!window) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  const Mosaic mosaic = fetchMosaic(*window, cancelled);
  if (mosaic.outcome != ImageLoadResult::Outcome::Image) {
    return outcome(mosaic.outcome);
  }
  // The quadtree tile's part of the block, scaled to the tile's image.
  const int32_t size = m_source.tileSizePx;
  QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(
        QRectF(0.0, 0.0, size, size), mosaic.image,
        QRectF(window->left * mosaic.scale, window->top * mosaic.scale,
               window->width * mosaic.scale, window->height * mosaic.scale));
  }
  ImageLoadResult result;
  result.outcome = ImageLoadResult::Outcome::Image;
  result.image = std::move(image);
  return result;
}

std::optional<WmtsImageryProvider::ProjectedGrid>
WmtsImageryProvider::projectedGrid(TileKey key) const
{
  // The part of the tile inside the source's bounds (its layer's extent
  // and the CRS's area of use, see wmtsSourceFor()).
  GeoRect clip = key.bounds();
  if (m_source.bounds) {
    const GeoRect& bounds = *m_source.bounds;
    clip = GeoRect{std::max(clip.westDeg, bounds.westDeg),
                   std::max(clip.southDeg, bounds.southDeg),
                   std::min(clip.eastDeg, bounds.eastDeg),
                   std::min(clip.northDeg, bounds.northDeg)};
    if (!(clip.westDeg < clip.eastDeg) || !(clip.southDeg < clip.northDeg)) {
      return std::nullopt;
    }
  }
  ProjectedGrid grid;
  grid.clip = clip;
  const int32_t n = ProjectedGrid::cells;
  const std::size_t side = static_cast<std::size_t>(n) + 1;
  grid.points.reserve(side * side);
  for (int32_t j = 0; j <= n; ++j) {
    const double lat = clip.northDeg - clip.heightDeg() * j / n;
    for (int32_t i = 0; i <= n; ++i) {
      grid.points.emplace_back(clip.westDeg + clip.widthDeg() * i / n, lat);
    }
  }
  std::unique_ptr<WmtsCrsTransform> transform = takeTransform();
  transform->forward(grid.points);
  returnTransform(std::move(transform));
  bool any = false;
  GeoRect extent{std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::lowest(),
                 std::numeric_limits<double>::lowest()};
  for (const std::pair<double, double>& point : grid.points) {
    if (std::isnan(point.first)) {
      continue;
    }
    any = true;
    extent.westDeg = std::min(extent.westDeg, point.first);
    extent.eastDeg = std::max(extent.eastDeg, point.first);
    extent.southDeg = std::min(extent.southDeg, point.second);
    extent.northDeg = std::max(extent.northDeg, point.second);
  }
  if (!any) {
    return std::nullopt;
  }
  grid.extent = extent;
  return grid;
}

ImageLoadResult
WmtsImageryProvider::loadReprojected(TileKey key,
                                     const std::atomic<bool>& cancelled)
{
  const std::optional<ProjectedGrid> grid = projectedGrid(key);
  if (!grid) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  const int32_t matrix = wmtsMatrixFor(m_source, key);
  const std::optional<WmtsTileWindow> window =
      wmtsTileWindowForExtent(m_source, grid->extent, matrix);
  if (!window) {
    return outcome(ImageLoadResult::Outcome::NoData);
  }
  const Mosaic mosaic = fetchMosaic(*window, cancelled);
  if (mosaic.outcome != ImageLoadResult::Outcome::Image) {
    return outcome(mosaic.outcome);
  }
  ImageLoadResult result;
  result.outcome = ImageLoadResult::Outcome::Image;
  result.image = resample(key, *grid, *window, mosaic);
  return result;
}

QImage WmtsImageryProvider::resample(TileKey key, const ProjectedGrid& grid,
                                     const WmtsTileWindow& window,
                                     const Mosaic& mosaic) const
{
  // Each pixel of the (geographic) tile image: its place in the CRS from
  // the grid of PROJ's points around it (bilinear between them; a cell of
  // 1/16 of the tile bends too little to see), then its colour from the
  // mosaic (bilinear between its pixels).
  const WmtsMatrix& m =
      m_source.matrices.at(static_cast<std::size_t>(window.matrix));
  const double blockLeft = m.originX + static_cast<double>(window.firstColumn) *
                                           m.tileWidth * m.pixelSize;
  const double blockTop = m.originY - static_cast<double>(window.firstRow) *
                                          m.tileHeight * m.pixelSize;
  const GeoRect rect = key.bounds();
  const GeoRect& clip = grid.clip;
  const int32_t size = m_source.tileSizePx;
  const int32_t n = ProjectedGrid::cells;
  const int32_t width = mosaic.image.width();
  const int32_t height = mosaic.image.height();
  // Mosaic pixels per CRS unit.
  const double perUnit = mosaic.scale / m.pixelSize;
  QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  const std::vector<std::pair<double, double>>& points = grid.points;
  for (int32_t v = 0; v < size; ++v) {
    const double lat = rect.northDeg - (v + 0.5) * rect.heightDeg() / size;
    if (lat > clip.northDeg || lat < clip.southDeg) {
      continue;
    }
    const double gy = (clip.northDeg - lat) / clip.heightDeg() * n;
    const int32_t j = std::min(static_cast<int32_t>(gy), n - 1);
    const double fy = gy - j;
    QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(v));
    for (int32_t u = 0; u < size; ++u) {
      const double lon = rect.westDeg + (u + 0.5) * rect.widthDeg() / size;
      if (lon < clip.westDeg || lon > clip.eastDeg) {
        continue;
      }
      const double gx = (lon - clip.westDeg) / clip.widthDeg() * n;
      const int32_t i = std::min(static_cast<int32_t>(gx), n - 1);
      const double fx = gx - i;
      const std::size_t corner =
          static_cast<std::size_t>(j) * (static_cast<std::size_t>(n) + 1) +
          static_cast<std::size_t>(i);
      const std::pair<double, double>& p00 = points[corner];
      const std::pair<double, double>& p10 = points[corner + 1];
      const std::pair<double, double>& p01 =
          points[corner + static_cast<std::size_t>(n + 1)];
      const std::pair<double, double>& p11 =
          points[corner + static_cast<std::size_t>(n + 2)];
      if (std::isnan(p00.first) || std::isnan(p10.first) ||
          std::isnan(p01.first) || std::isnan(p11.first)) {
        continue;
      }
      const double x = (1.0 - fy) * ((1.0 - fx) * p00.first + fx * p10.first) +
                       fy * ((1.0 - fx) * p01.first + fx * p11.first);
      const double y =
          (1.0 - fy) * ((1.0 - fx) * p00.second + fx * p10.second) +
          fy * ((1.0 - fx) * p01.second + fx * p11.second);
      line[u] = sampleBilinear(mosaic.image, (x - blockLeft) * perUnit - 0.5,
                               (blockTop - y) * perUnit - 0.5, width, height);
    }
  }
  return image;
}

std::unique_ptr<WmtsCrsTransform> WmtsImageryProvider::takeTransform() const
{
  {
    const std::lock_guard<std::mutex> lock(m_transformMutex);
    if (!m_transforms.empty()) {
      std::unique_ptr<WmtsCrsTransform> transform =
          std::move(m_transforms.back());
      m_transforms.pop_back();
      return transform;
    }
  }
  return std::make_unique<WmtsCrsTransform>(m_source.crs);
}

void WmtsImageryProvider::returnTransform(
    std::unique_ptr<WmtsCrsTransform> transform) const
{
  const std::lock_guard<std::mutex> lock(m_transformMutex);
  m_transforms.push_back(std::move(transform));
}

QUrl WmtsImageryProvider::serviceUrl() const
{
  if (!m_source.service.isEmpty()) {
    return m_source.service;
  }
  if (!m_source.url.isEmpty()) {
    return m_source.url;
  }
  QUrl host(m_source.urlTemplate);
  host.setPath(QString());
  host.setQuery(QString());
  return host;
}

int64_t WmtsImageryProvider::typicalTileBytes() const
{
  const WmtsMatrix& finest = m_source.matrices.back();
  return typicalImageBytes(m_source.format,
                           std::max(finest.tileWidth, finest.tileHeight));
}

RegionTileResult
WmtsImageryProvider::fetchForRegion(TileKey key, int64_t region,
                                    const std::atomic<bool>& cancelled)
{
  using Outcome = RegionTileResult::Outcome;
  const int32_t matrix = wmtsMatrixFor(m_source, key);
  std::optional<WmtsTileWindow> window;
  if (isProjectedWmtsCrs(m_source.crs)) {
    // The same tiles as drawing it needs.
    const std::optional<ProjectedGrid> grid = projectedGrid(key);
    if (grid) {
      window = wmtsTileWindowForExtent(m_source, grid->extent, matrix);
    }
  } else {
    window = wmtsTileWindow(m_source, key, matrix);
  }
  RegionTileResult total;
  total.outcome = Outcome::NoData;
  if (!window) {
    return total;
  }
  bool anyDownloaded = false;
  bool anyRevalidated = false;
  bool anyStored = false;
  for (int32_t r = 0; r < window->rows; ++r) {
    for (int32_t c = 0; c < window->columns; ++c) {
      const WmtsTileIndex tile{matrix, window->firstColumn + c,
                               window->firstRow + r};
      if (!wmtsTileExists(m_source, tile)) {
        continue;
      }
      const RegionTileResult result = m_fetcher->fetchForRegion(
          TileKey{matrix, static_cast<int32_t>(tile.column),
                  static_cast<int32_t>(tile.row)},
          tileUrl(tile), region,
          [](const QByteArray& data) {
            QImage image;
            return image.loadFromData(data);
          },
          cancelled);
      switch (result.outcome) {
      case Outcome::Cancelled:
      case Outcome::Failed:
        return result;
      case Outcome::NoData:
        break;
      case Outcome::Downloaded:
        anyDownloaded = true;
        break;
      case Outcome::Revalidated:
        anyRevalidated = true;
        break;
      case Outcome::AlreadyCached:
        break;
      }
      if (result.outcome != Outcome::NoData) {
        anyStored = true;
        total.bytes += result.bytes;
      }
    }
  }
  if (anyDownloaded) {
    total.outcome = Outcome::Downloaded;
  } else if (anyRevalidated) {
    total.outcome = Outcome::Revalidated;
  } else if (anyStored) {
    total.outcome = Outcome::AlreadyCached;
  }
  return total;
}

std::optional<GeoRect> WmtsImageryProvider::dataBounds() const
{
  std::optional<GeoRect> bounds = m_source.bounds;
  if (m_source.imageProjection() == TileImageProjection::WebMercator) {
    bounds = webMercatorCoverage(
        bounds.value_or(GeoRect{-180.0, -90.0, 180.0, 90.0}));
    if (!bounds) {
      // Nothing at all: an empty rectangle no tile overlaps.
      return GeoRect{0.0, 0.0, 0.0, 0.0};
    }
  }
  return bounds;
}

} // namespace ts
