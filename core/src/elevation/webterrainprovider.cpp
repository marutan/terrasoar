// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/webterrainprovider.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <utility>

#include <QCoreApplication>
#include <QImage>

#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/heightgrid.h"
#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

constexpr double noData = std::numeric_limits<double>::quiet_NaN();

/** Latitude where Web Mercator ends, degrees. */
constexpr double mercatorLimitDeg = 85.0511287798066;

/** Decoded web tiles kept (bytes): 64 tiles of 256². */
constexpr std::size_t tileCacheBytes = std::size_t{64} * 256 * 256 * 4;

constexpr double pi = 3.14159265358979323846;

/** Index of (@p row, @p column) in a grid @p width wide. */
// Row then column, as everywhere in a grid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::size_t gridIndex(int row, int column, int width)
{
  return static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(column);
}

QString encodingName(TerrainEncoding encoding)
{
  return encoding == TerrainEncoding::TerrainRgb ? QStringLiteral("terrain-rgb")
                                                 : QStringLiteral("terrarium");
}

QString statusError(const HttpResponse& response)
{
  return QCoreApplication::translate("ts::WebTerrainProvider",
                                     "The terrain server answered %1.")
      .arg(response.errorString);
}

QString notATile(const HttpResponse& response)
{
  return QCoreApplication::translate(
             "ts::WebTerrainProvider",
             "The terrain server sent something that isn't a terrain tile "
             "(%1).")
      .arg(QString::fromLatin1(response.header("content-type")));
}

/**
 * The value at (@p x0 + @p fx, @p y0 + @p fy) between the pixels around
 * it (@p pixel gives them), from those that have data; NaN if none has.
 */
// The pixel's corner, then the fractions: as the position reads.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
double bilinear(const std::function<float(int, int)>& pixel, int x0, int y0,
                double fx, double fy)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  double sum = 0.0;
  double weights = 0.0;
  for (int dy = 0; dy <= 1; ++dy) {
    for (int dx = 0; dx <= 1; ++dx) {
      const double weight =
          (dx == 1 ? fx : 1.0 - fx) * (dy == 1 ? fy : 1.0 - fy);
      const float value = weight > 0.0
                              ? pixel(x0 + dx, y0 + dy)
                              : std::numeric_limits<float>::quiet_NaN();
      if (!std::isnan(value)) {
        sum += weight * static_cast<double>(value);
        weights += weight;
      }
    }
  }
  return weights > 1e-9 ? sum / weights : noData;
}

} // namespace

double decodeTerrainPixel(QRgb pixel, TerrainEncoding encoding)
{
  if (qAlpha(pixel) == 0) {
    return noData;
  }
  const double r = qRed(pixel);
  const double g = qGreen(pixel);
  const double b = qBlue(pixel);
  if (encoding == TerrainEncoding::TerrainRgb) {
    return -10000.0 + (r * 65536.0 + g * 256.0 + b) * 0.1;
  }
  return r * 256.0 + g + b / 256.0 - 32768.0;
}

std::optional<WebTerrainSource>
WebTerrainSource::fromJson(const QJsonObject& json, QString* errorString)
{
  const std::function<std::optional<WebTerrainSource>(const QString&)> fail =
      [errorString](const QString& message) {
        if (errorString != nullptr) {
          *errorString = message;
        }
        return std::optional<WebTerrainSource>();
      };
  WebTerrainSource source;
  source.urlTemplate = json.value(QStringLiteral("url")).toString();
  const QUrl url(source.urlTemplate);
  if ((url.scheme() != QLatin1String("http") &&
       url.scheme() != QLatin1String("https")) ||
      url.host().isEmpty() ||
      !source.urlTemplate.contains(QLatin1String("{z}")) ||
      !source.urlTemplate.contains(QLatin1String("{x}")) ||
      !source.urlTemplate.contains(QLatin1String("{y}"))) {
    return fail(QCoreApplication::translate(
        "ts::WebTerrainProvider",
        "A terrain tile address must be an http or https address with "
        "{z}, {x} and {y} in it."));
  }
  const QString encoding = json.value(QStringLiteral("encoding")).toString();
  if (encoding == QLatin1String("terrarium")) {
    source.encoding = TerrainEncoding::Terrarium;
  } else if (encoding == QLatin1String("terrain-rgb")) {
    source.encoding = TerrainEncoding::TerrainRgb;
  } else {
    return fail(QCoreApplication::translate("ts::WebTerrainProvider",
                                            "Unknown terrain encoding \"%1\".")
                    .arg(encoding));
  }
  source.maximumZoom = json.value(QStringLiteral("maximumZoom")).toInt(15);
  source.tileSizePx = json.value(QStringLiteral("tileSize")).toInt(256);
  if (source.maximumZoom < 0 || source.maximumZoom > 22 ||
      (source.tileSizePx != 256 && source.tileSizePx != 512)) {
    return fail(QCoreApplication::translate(
        "ts::WebTerrainProvider",
        "The terrain tiles' zoom must be 0–22 and their size 256 or 512."));
  }
  source.verticalDatum = json.value(QStringLiteral("verticalDatum"))
                             .toString(source.verticalDatum);
  source.name = json.value(QStringLiteral("name")).toString();
  source.attribution = json.value(QStringLiteral("attribution")).toString();
  return source;
}

QJsonObject WebTerrainSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), QStringLiteral("web"));
  json.insert(QStringLiteral("url"), urlTemplate);
  json.insert(QStringLiteral("encoding"), encodingName(encoding));
  json.insert(QStringLiteral("maximumZoom"), maximumZoom);
  json.insert(QStringLiteral("tileSize"), tileSizePx);
  json.insert(QStringLiteral("verticalDatum"), verticalDatum);
  if (!name.isEmpty()) {
    json.insert(QStringLiteral("name"), name);
  }
  if (!attribution.isEmpty()) {
    json.insert(QStringLiteral("attribution"), attribution);
  }
  return json;
}

QString WebTerrainSource::id() const
{
  // ts-notr: identifier
  return QStringLiteral("webheights:%1|%2|%3|%4")
      .arg(urlTemplate, encodingName(encoding))
      .arg(tileSizePx)
      .arg(verticalDatum);
}

QUrl WebTerrainSource::tileUrl(int zoom, int x, int y) const
{
  QString url = urlTemplate;
  url.replace(QLatin1String("{z}"), QString::number(zoom));
  url.replace(QLatin1String("{x}"), QString::number(x));
  url.replace(QLatin1String("{y}"), QString::number(y));
  return QUrl(url);
}

WebTerrainProvider::WebTerrainProvider(
    WebTerrainSource source, std::shared_ptr<NetworkClient> network,
    WebTileSettings settings, std::shared_ptr<const GeoidModel> datumGeoid,
    std::shared_ptr<const GeoidModel> seaLevelGeoid)
    : m_source(std::move(source)), m_datumGeoid(std::move(datumGeoid)),
      m_seaLevelGeoid(std::move(seaLevelGeoid)), m_tiles(tileCacheBytes)
{
  CachedTileFetcher::Messages messages{statusError, notATile};
  messages.notFoundIsNoData = true;
  m_fetcher = std::make_unique<CachedTileFetcher>(
      m_source.id(), QUrl(m_source.urlTemplate).host(), std::move(network),
      std::move(settings), std::move(messages),
      [this](const ProviderStatus& status) { reportStatus(status); });
}

WebTerrainProvider::~WebTerrainProvider() = default;

QString WebTerrainProvider::id() const
{
  // The tiles' id, and which grid turns them into ellipsoidal heights.
  return m_source.id() + QLatin1Char('|') +
         (m_datumGeoid ? m_datumGeoid->id() : QString());
}

QString WebTerrainProvider::name() const
{
  return m_source.name.isEmpty() ? QUrl(m_source.urlTemplate).host()
                                 : m_source.name;
}

int WebTerrainProvider::zoomForLevel(int level) const
{
  // Pixels as far apart as the samples at the equator: zoom z has
  // 360° / (size · 2^z) per pixel, level L 180° / (64 · 2^L) per sample.
  const int sizeSteps = m_source.tileSizePx == 512 ? 1 : 0;
  return std::clamp(level - 1 - sizeSteps, 0, m_source.maximumZoom);
}

int WebTerrainProvider::maximumLevel() const
{
  const int sizeSteps = m_source.tileSizePx == 512 ? 1 : 0;
  return std::min(m_source.maximumZoom + 1 + sizeSteps, TileKey::maximumLevel);
}

int WebTerrainProvider::preferredLoaderThreads() const
{
  return m_fetcher->settings().loaderThreads;
}

WebTerrainProvider::Fetched
WebTerrainProvider::webTile(int zoom, int x, int y,
                            const std::atomic<bool>& cancelled)
{
  // A TileKey stands for the web tile in the caches.
  const TileKey key{zoom, x, y};
  if (const std::optional<std::shared_ptr<const Heights>> kept =
          m_tiles.find(key)) {
    return Fetched{FetchOutcome::Data, *kept};
  }
  const int size = m_source.tileSizePx;
  std::shared_ptr<Heights> heights;
  const FetchOutcome outcome = m_fetcher->fetch(
      key, m_source.tileUrl(zoom, x, y),
      [&](const QByteArray& data) {
        QImage image;
        if (!image.loadFromData(data) || image.width() != size ||
            image.height() != size) {
          return false;
        }
        image = image.convertToFormat(QImage::Format_ARGB32);
        heights = std::make_shared<Heights>(gridIndex(size, 0, size));
        for (int row = 0; row < size; ++row) {
          const QRgb* line =
              reinterpret_cast<const QRgb*>(image.constScanLine(row));
          for (int column = 0; column < size; ++column) {
            (*heights)[gridIndex(row, column, size)] = static_cast<float>(
                decodeTerrainPixel(line[column], m_source.encoding));
          }
        }
        return true;
      },
      cancelled);
  if (outcome == FetchOutcome::Data && heights) {
    m_tiles.insert(key, heights, heights->size() * sizeof(float));
    return Fetched{outcome, heights};
  }
  return Fetched{outcome, nullptr};
}

std::optional<HeightTile>
WebTerrainProvider::loadHeights(TileKey key, const std::atomic<bool>& cancelled)
{
  if (!key.isValid()) {
    return std::nullopt;
  }
  const int zoom = zoomForLevel(key.level);
  const int size = m_source.tileSizePx;
  const int tiles = 1 << zoom;
  const double world = static_cast<double>(size) * tiles;

  HeightTile tile;
  tile.key = key;
  tile.samples = samples;
  const std::size_t count = gridIndex(samples, 0, samples);
  tile.heightsM.assign(count, static_cast<float>(noData));
  std::vector<GeoPoint> points(count);
  // The web tiles this tile needs, fetched once each.
  std::map<std::pair<int, int>, Fetched> fetched;
  const std::function<float(int, int)> pixel = [&](int gx, int gy) -> float {
    const int wrapped = ((gx % (tiles * size)) + tiles * size) % (tiles * size);
    const int clamped = std::clamp(gy, 0, tiles * size - 1);
    const std::pair<int, int> at{wrapped / size, clamped / size};
    std::map<std::pair<int, int>, Fetched>::iterator it = fetched.find(at);
    if (it == fetched.end()) {
      it = fetched.emplace(at, webTile(zoom, at.first, at.second, cancelled))
               .first;
    }
    const Fetched& web = it->second;
    if (web.outcome == FetchOutcome::Failed) {
      tile.incomplete = true;
    }
    if (!web.heights) {
      return static_cast<float>(noData);
    }
    return (*web.heights)[gridIndex(clamped % size, wrapped % size, size)];
  };

  bool found = false;
  for (int row = 0; row < samples; ++row) {
    for (int column = 0; column < samples; ++column) {
      const std::size_t i = gridIndex(row, column, samples);
      points[i] = heightSamplePoint(key, samples, row, column);
      const double lat = points[i].latDeg;
      if (std::abs(lat) > mercatorLimitDeg) {
        continue;
      }
      // Global pixel coordinates; pixel centres at whole numbers.
      const double latRad = lat * pi / 180.0;
      const double u = (points[i].lonDeg + 180.0) / 360.0 * world - 0.5;
      const double v =
          (1.0 - std::asinh(std::tan(latRad)) / pi) / 2.0 * world - 0.5;
      const int x0 = static_cast<int>(std::floor(u));
      const int y0 = static_cast<int>(std::floor(v));
      const double height = bilinear(pixel, x0, y0, u - x0, v - y0);
      if (!std::isnan(height)) {
        tile.heightsM[i] = static_cast<float>(height);
        found = true;
      }
    }
  }
  if (cancelled.load() ||
      std::any_of(fetched.begin(), fetched.end(),
                  [](const std::pair<const std::pair<int, int>, Fetched>& web) {
                    return web.second.outcome == FetchOutcome::Cancelled;
                  })) {
    return std::nullopt;
  }
  if (!found && !tile.incomplete) {
    return std::nullopt;
  }
  applyVerticalDatum(tile, points, m_datumGeoid.get(), m_seaLevelGeoid.get());
  return tile;
}

} // namespace ts
