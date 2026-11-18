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
#include "ts/wmts/wmtssource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <QCoreApplication>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QUrlQuery>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/wms/wmssource.h"
#include "ts/wmts/wmtscapabilities.h"
#include "ts/wmts/wmtscrs.h"

namespace ts
{

namespace
{

/**
 * Most WMTS tiles fitted to one quadtree tile. A national grid's coarsest
 * matrix may still be much finer than a coarse quadtree tile: then its
 * tiles are drawn smaller (WmtsImageryProvider).
 */
constexpr int32_t maximumTilesPerQuadtreeTile = 64;

/**
 * Most matrices a source keeps: the disk cache keys tiles by matrix index
 * in TileKey's level, which has six bits (TileKey::packed()).
 */
constexpr std::size_t maximumMatrices = 60;

/** WMTS parameters a base URL may carry that requests set themselves. */
const std::array<const char*, 11> wmtsParameters = {
    "service", "request", "version",       "layer",
    "style",   "format",  "tilematrixset", "tilematrix",
    "tilerow", "tilecol", "acceptversions"};

bool isWmtsParameter(const QString& key)
{
  return std::any_of(
      wmtsParameters.begin(), wmtsParameters.end(), [&key](const char* name) {
        return key.compare(QLatin1String(name), Qt::CaseInsensitive) == 0;
      });
}

const WebMercatorProjection& webMercator()
{
  static const WebMercatorProjection projection;
  return projection;
}

bool isWebMercator(const QString& crs)
{
  return crs == QLatin1String("EPSG:3857");
}

/** True for CRS:84, EPSG:4326 and other geographic CRSs PROJ knows. */
bool isGeographic(const QString& crs)
{
  if (crs == QLatin1String("CRS:84") || crs == QLatin1String("EPSG:4326")) {
    return true;
  }
  if (isWebMercator(crs)) {
    return false;
  }
  const std::optional<WmtsCrsInfo> info = wmtsCrsInfo(crs);
  return info && info->geographic;
}

/** Metres per unit of @p crs (WMTS's degree for geographic ones). */
double metresPerUnit(const QString& crs)
{
  if (isGeographic(crs)) {
    return wmtsMetresPerDegree;
  }
  if (isWebMercator(crs)) {
    return 1.0;
  }
  const std::optional<WmtsCrsInfo> info = wmtsCrsInfo(crs);
  return info && info->metresPerUnit > 0.0 ? info->metresPerUnit : 1.0;
}

/**
 * Width of a level-0 quadtree tile in @p crs units: 180°, or that along a
 * meridian (π a metres) for Web Mercator and projected CRSs, whose scale is
 * about true near where they are used.
 */
double levelZeroWidth(const QString& crs)
{
  return isGeographic(crs) ? 180.0
                           : wgs84::semiMajorAxisM * pi / metresPerUnit(crs);
}

/** A pixel this size in metres, for comparing CRSs. */
double pixelSizeM(double pixelSize, const QString& crs)
{
  return pixelSize * metresPerUnit(crs);
}

/**
 * How @p crs is drawn, best first: 0 geographic (no reprojection), 1 Web
 * Mercator (reprojected in the shader), 2 others (reprojected through
 * PROJ, on the CPU).
 */
int32_t drawingRank(const QString& crs)
{
  if (isGeographic(crs)) {
    return 0;
  }
  return isWebMercator(crs) ? 1 : 2;
}

/** The overlap of @p a and @p b (nothing if they don't overlap). */
std::optional<GeoRect> intersection(const GeoRect& a, const GeoRect& b)
{
  const GeoRect overlap{
      std::max(a.westDeg, b.westDeg), std::max(a.southDeg, b.southDeg),
      std::min(a.eastDeg, b.eastDeg), std::min(a.northDeg, b.northDeg)};
  if (!(overlap.westDeg < overlap.eastDeg) ||
      !(overlap.southDeg < overlap.northDeg)) {
    return GeoRect{0.0, 0.0, 0.0, 0.0};
  }
  return overlap;
}

/** The deepest quadtree level that gains detail from @p finest. */
int32_t deepestLevel(double finest, const QString& crs, int32_t tileSizePx)
{
  const double ratio =
      levelZeroWidth(crs) / (static_cast<double>(tileSizePx) * finest);
  const double level = std::ceil(std::log2(std::max(ratio, 1.0)) - 0.01);
  return std::clamp(static_cast<int32_t>(level), 0, TileKey::maximumLevel);
}

std::optional<WmtsSource> fail(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return std::nullopt;
}

/**
 * The matrices of @p set that @p link lets the layer use, ready for
 * arithmetic, coarsest first.
 */
std::vector<WmtsMatrix> usableMatrices(const WmtsTileMatrixSet& set,
                                       const WmtsTileMatrixSetLink& link,
                                       const QString& crs)
{
  std::vector<WmtsMatrix> matrices;
  for (const WmtsTileMatrix& matrix : set.matrices) {
    WmtsMatrix usable = wmtsMatrix(matrix, crs);
    if (!link.limits.empty()) {
      // With limits, only the matrices listed have tiles.
      const std::vector<WmtsTileMatrixLimits>::const_iterator limits =
          std::find_if(link.limits.begin(), link.limits.end(),
                       [&matrix](const WmtsTileMatrixLimits& candidate) {
                         return candidate.tileMatrix == matrix.identifier;
                       });
      if (limits == link.limits.end()) {
        continue;
      }
      usable.minRow = std::max(usable.minRow, limits->minRow);
      usable.maxRow = std::min(usable.maxRow, limits->maxRow);
      usable.minColumn = std::max(usable.minColumn, limits->minColumn);
      usable.maxColumn = std::min(usable.maxColumn, limits->maxColumn);
      if (usable.minRow > usable.maxRow ||
          usable.minColumn > usable.maxColumn) {
        continue;
      }
    }
    if (usable.pixelSize > 0.0 && std::isfinite(usable.pixelSize)) {
      matrices.push_back(usable);
    }
  }
  std::stable_sort(matrices.begin(), matrices.end(),
                   [](const WmtsMatrix& a, const WmtsMatrix& b) {
                     return a.pixelSize > b.pixelSize;
                   });
  if (matrices.size() > maximumMatrices) {
    // Keep the finest; coarse tiles are drawn from them less sharply.
    matrices.erase(matrices.begin(),
                   matrices.end() -
                       static_cast<std::ptrdiff_t>(maximumMatrices));
  }
  return matrices;
}

QJsonObject matrixJson(const WmtsMatrix& matrix)
{
  return QJsonObject{
      {QStringLiteral("id"), matrix.identifier},
      {QStringLiteral("pixelSize"), matrix.pixelSize},
      {QStringLiteral("x"), matrix.originX},
      {QStringLiteral("y"), matrix.originY},
      {QStringLiteral("tileWidth"), matrix.tileWidth},
      {QStringLiteral("tileHeight"), matrix.tileHeight},
      {QStringLiteral("matrixWidth"), static_cast<double>(matrix.matrixWidth)},
      {QStringLiteral("matrixHeight"),
       static_cast<double>(matrix.matrixHeight)},
      {QStringLiteral("minRow"), static_cast<double>(matrix.minRow)},
      {QStringLiteral("maxRow"), static_cast<double>(matrix.maxRow)},
      {QStringLiteral("minColumn"), static_cast<double>(matrix.minColumn)},
      {QStringLiteral("maxColumn"), static_cast<double>(matrix.maxColumn)}};
}

std::optional<WmtsMatrix> matrixFromJson(const QJsonObject& json)
{
  WmtsMatrix matrix;
  matrix.identifier = json.value(QStringLiteral("id")).toString();
  matrix.pixelSize = json.value(QStringLiteral("pixelSize")).toDouble();
  matrix.originX = json.value(QStringLiteral("x")).toDouble();
  matrix.originY = json.value(QStringLiteral("y")).toDouble();
  matrix.tileWidth = json.value(QStringLiteral("tileWidth")).toInt();
  matrix.tileHeight = json.value(QStringLiteral("tileHeight")).toInt();
  matrix.matrixWidth = static_cast<int64_t>(
      json.value(QStringLiteral("matrixWidth")).toDouble());
  matrix.matrixHeight = static_cast<int64_t>(
      json.value(QStringLiteral("matrixHeight")).toDouble());
  matrix.minRow =
      static_cast<int64_t>(json.value(QStringLiteral("minRow")).toDouble());
  matrix.maxRow = static_cast<int64_t>(
      json.value(QStringLiteral("maxRow"))
          .toDouble(static_cast<double>(matrix.matrixHeight - 1)));
  matrix.minColumn =
      static_cast<int64_t>(json.value(QStringLiteral("minColumn")).toDouble());
  matrix.maxColumn = static_cast<int64_t>(
      json.value(QStringLiteral("maxColumn"))
          .toDouble(static_cast<double>(matrix.matrixWidth - 1)));
  const bool sane =
      !matrix.identifier.isEmpty() && matrix.pixelSize > 0.0 &&
      std::isfinite(matrix.pixelSize) && std::isfinite(matrix.originX) &&
      std::isfinite(matrix.originY) && matrix.tileWidth > 0 &&
      matrix.tileWidth <= 4096 && matrix.tileHeight > 0 &&
      matrix.tileHeight <= 4096 && matrix.matrixWidth > 0 &&
      matrix.matrixHeight > 0 && matrix.minRow >= 0 &&
      matrix.minRow <= matrix.maxRow && matrix.maxRow < matrix.matrixHeight &&
      matrix.minColumn >= 0 && matrix.minColumn <= matrix.maxColumn &&
      matrix.maxColumn < matrix.matrixWidth;
  return sane ? std::optional<WmtsMatrix>(matrix) : std::nullopt;
}

/** The link of @p layer to a set in @p capabilities that can be drawn. */
struct UsableSet {
  const WmtsTileMatrixSet* set = nullptr;
  const WmtsTileMatrixSetLink* link = nullptr;
  QString crs;
  std::vector<WmtsMatrix> matrices;
};

std::vector<UsableSet> usableSets(const WmtsCapabilities& capabilities,
                                  const WmtsLayer& layer)
{
  std::vector<UsableSet> sets;
  for (const WmtsTileMatrixSetLink& link : layer.tileMatrixSetLinks) {
    const WmtsTileMatrixSet* set =
        capabilities.findTileMatrixSet(link.tileMatrixSet);
    if (set == nullptr) {
      continue;
    }
    const QString crs = normalisedWmtsCrs(set->supportedCrs);
    if (!isSupportedWmtsCrs(crs)) {
      continue;
    }
    std::vector<WmtsMatrix> matrices = usableMatrices(*set, link, crs);
    if (!matrices.empty()) {
      sets.push_back(UsableSet{set, &link, crs, std::move(matrices)});
    }
  }
  std::stable_sort(sets.begin(), sets.end(),
                   [](const UsableSet& a, const UsableSet& b) {
                     if (drawingRank(a.crs) != drawingRank(b.crs)) {
                       return drawingRank(a.crs) < drawingRank(b.crs);
                     }
                     return pixelSizeM(a.matrices.back().pixelSize, a.crs) <
                            pixelSizeM(b.matrices.back().pixelSize, b.crs);
                   });
  return sets;
}

QString noUsableSetMessage(const WmtsCapabilities& capabilities,
                           const WmtsLayer& layer)
{
  QStringList offered;
  for (const WmtsTileMatrixSetLink& link : layer.tileMatrixSetLinks) {
    const WmtsTileMatrixSet* set =
        capabilities.findTileMatrixSet(link.tileMatrixSet);
    if (set != nullptr) {
      offered.append(set->supportedCrs);
    }
  }
  offered.removeDuplicates();
  return QCoreApplication::translate(
             "ts::WmtsSource",
             "Layer %1 is only offered in coordinate systems that can't be "
             "drawn (%2): they aren't EPSG codes PROJ knows.")
      .arg(layer.identifier, offered.join(QStringLiteral(", ")));
}

bool failed(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return false;
}

/** Sets @p source's style: @p wanted, or the layer's default. */
bool chooseStyle(const WmtsLayer& layer, const QString& wanted,
                 WmtsSource& source, QString* errorString)
{
  source.style = wanted.isEmpty() ? layer.defaultStyle() : wanted;
  if (source.style.isEmpty()) {
    source.style = QStringLiteral("default");
  }
  const bool known = layer.styles.empty() ||
                     std::any_of(layer.styles.begin(), layer.styles.end(),
                                 [&source](const WmtsStyle& candidate) {
                                   return candidate.identifier == source.style;
                                 });
  return known ||
         failed(errorString, QCoreApplication::translate(
                                 "ts::WmtsSource", "Layer %1 has no style %2.")
                                 .arg(layer.identifier, source.style));
}

/**
 * Of the layer's templates for @p format, the one with a variable for
 * every dimension (NASA GIBS has one with {Time} and some without), so
 * their values are in each address and so in the cache's keys; else the
 * first; null if none.
 */
const WmtsResourceUrl* templateFor(const WmtsLayer& layer,
                                   const QString& format)
{
  const WmtsResourceUrl* found = nullptr;
  for (const WmtsResourceUrl& candidate : layer.resourceUrls) {
    if (candidate.format != format) {
      continue;
    }
    const bool hasEveryDimension = std::all_of(
        layer.dimensions.begin(), layer.dimensions.end(),
        [&candidate](const WmtsDimension& dimension) {
          return candidate.urlTemplate.contains(
              QLatin1Char('{') + dimension.identifier + QLatin1Char('}'),
              Qt::CaseInsensitive);
        });
    if (hasEveryDimension) {
      return &candidate;
    }
    if (found == nullptr) {
      found = &candidate;
    }
  }
  return found;
}

/**
 * Sets @p source's format (@p wanted, or chosen) and its address: the
 * RESTful template for the format, else the KVP address.
 */
bool chooseAddress(const WmtsCapabilities& capabilities, const WmtsLayer& layer,
                   const QString& wanted, WmtsSource& source,
                   QString* errorString)
{
  QStringList formats = layer.formats;
  for (const WmtsResourceUrl& resource : layer.resourceUrls) {
    if (!formats.contains(resource.format)) {
      formats.append(resource.format);
    }
  }
  source.format = wanted.isEmpty() ? chooseFormat(formats, false) : wanted;
  if (source.format.isEmpty() && wanted.isEmpty() && !formats.isEmpty()) {
    // None of the usual ones (ArcGIS offers "image/jpgpng"): the first
    // offered; the image's own bytes say what it is when it arrives.
    source.format = formats.first();
  }
  if (source.format.isEmpty() || !formats.contains(source.format)) {
    return failed(
        errorString,
        QCoreApplication::translate("ts::WmtsSource",
                                    "Layer %1 can't be requested as %2.")
            .arg(layer.identifier, source.format.isEmpty()
                                       ? formats.join(QStringLiteral(", "))
                                       : source.format));
  }
  const WmtsResourceUrl* resource = templateFor(layer, source.format);
  if (resource != nullptr) {
    source.urlTemplate = resource->urlTemplate;
    return true;
  }
  if (!capabilities.getTileKvpUrl.isEmpty()) {
    source.url = capabilities.getTileKvpUrl;
    return true;
  }
  return failed(errorString,
                QCoreApplication::translate(
                    "ts::WmtsSource",
                    "The service gives no address for layer %1's tiles in %2.")
                    .arg(layer.identifier, source.format));
}

/** Sets a value for each of the layer's dimensions. */
bool chooseDimensions(const WmtsLayer& layer,
                      const std::vector<std::pair<QString, QString>>& given,
                      WmtsSource& source, QString* errorString)
{
  for (const WmtsDimension& dimension : layer.dimensions) {
    const std::vector<std::pair<QString, QString>>::const_iterator value =
        std::find_if(
            given.begin(), given.end(),
            [&dimension](const std::pair<QString, QString>& candidate) {
              return candidate.first.compare(dimension.identifier,
                                             Qt::CaseInsensitive) == 0;
            });
    QString chosen =
        value != given.end() ? value->second : dimension.defaultValue;
    if (chosen.isEmpty() && !dimension.values.isEmpty()) {
      chosen = dimension.values.first();
    }
    if (chosen.isEmpty()) {
      return failed(errorString,
                    QCoreApplication::translate(
                        "ts::WmtsSource",
                        "Layer %1 needs a value for %2, and the service "
                        "gives no default.")
                        .arg(layer.identifier, dimension.identifier));
    }
    source.dimensions.emplace_back(dimension.identifier, chosen);
  }
  return true;
}

} // namespace

// --- WmtsSource
// ----------------------------------------------------------------

QString WmtsSource::id() const
{
  QStringList dimensionValues;
  for (const std::pair<QString, QString>& dimension : dimensions) {
    dimensionValues.append(dimension.first + QLatin1Char('=') +
                           dimension.second);
  }
  return QStringLiteral("wmts:%1|%2|%3|%4|%5|%6|%7")
      .arg(urlTemplate.isEmpty() ? url.toString(QUrl::FullyEncoded)
                                 : urlTemplate,
           layer, style, format, tileMatrixSet,
           dimensionValues.join(QLatin1Char(';')))
      .arg(tileSizePx);
}

TileImageProjection WmtsSource::imageProjection() const
{
  return crs == QLatin1String("EPSG:3857") ? TileImageProjection::WebMercator
                                           : TileImageProjection::Geographic;
}

QJsonObject WmtsSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), QStringLiteral("wmts"));
  if (urlTemplate.isEmpty()) {
    json.insert(QStringLiteral("url"), url.toString(QUrl::FullyEncoded));
  } else {
    json.insert(QStringLiteral("template"), urlTemplate);
  }
  if (!service.isEmpty()) {
    json.insert(QStringLiteral("service"),
                service.toString(QUrl::FullyEncoded));
  }
  json.insert(QStringLiteral("layer"), layer);
  json.insert(QStringLiteral("style"), style);
  json.insert(QStringLiteral("format"), format);
  json.insert(QStringLiteral("tileMatrixSet"), tileMatrixSet);
  json.insert(QStringLiteral("crs"), crs);
  QJsonArray matrixArray;
  for (const WmtsMatrix& matrix : matrices) {
    matrixArray.append(matrixJson(matrix));
  }
  json.insert(QStringLiteral("matrices"), matrixArray);
  if (!dimensions.empty()) {
    QJsonArray dimensionArray;
    for (const std::pair<QString, QString>& dimension : dimensions) {
      dimensionArray.append(QJsonArray{dimension.first, dimension.second});
    }
    json.insert(QStringLiteral("dimensions"), dimensionArray);
  }
  json.insert(QStringLiteral("tileSize"), tileSizePx);
  json.insert(QStringLiteral("maximumLevel"), maximumLevel);
  if (bounds) {
    json.insert(QStringLiteral("bounds"),
                QJsonArray{bounds->westDeg, bounds->southDeg, bounds->eastDeg,
                           bounds->northDeg});
  }
  if (!title.isEmpty()) {
    json.insert(QStringLiteral("title"), title);
  }
  if (!attribution.isEmpty()) {
    json.insert(QStringLiteral("attribution"), attribution);
  }
  return json;
}

std::optional<WmtsSource> WmtsSource::fromJson(const QJsonObject& json,
                                               QString* errorString)
{
  if (json.value(QStringLiteral("kind")).toString() != QLatin1String("wmts")) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmtsSource",
                                            "This is not a WMTS source."));
  }
  WmtsSource source;
  source.urlTemplate = json.value(QStringLiteral("template")).toString();
  source.url = QUrl(json.value(QStringLiteral("url")).toString());
  const bool kvp =
      source.url.isValid() && (source.url.scheme() == QLatin1String("http") ||
                               source.url.scheme() == QLatin1String("https"));
  const bool rest = source.urlTemplate.startsWith(QLatin1String("http://")) ||
                    source.urlTemplate.startsWith(QLatin1String("https://"));
  if (!kvp && !rest) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WmtsSource",
                                 "The WMTS source has no usable address."));
  }
  if (rest) {
    source.url = QUrl();
  }
  source.service = QUrl(json.value(QStringLiteral("service")).toString());
  source.layer = json.value(QStringLiteral("layer")).toString();
  source.style = json.value(QStringLiteral("style")).toString();
  source.format = json.value(QStringLiteral("format")).toString(source.format);
  source.tileMatrixSet = json.value(QStringLiteral("tileMatrixSet")).toString();
  source.crs = normalisedWmtsCrs(json.value(QStringLiteral("crs")).toString());
  if (source.layer.isEmpty() || source.tileMatrixSet.isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmtsSource",
                    "The WMTS source names no layer or tile matrix "
                    "set."));
  }
  if (!isSupportedWmtsCrs(source.crs)) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmtsSource",
                    "The WMTS source's coordinate system (%1) can't "
                    "be drawn.")
                    .arg(json.value(QStringLiteral("crs")).toString()));
  }
  const QJsonArray matrixArray =
      json.value(QStringLiteral("matrices")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert): the same on Qt 5 and Qt 6
  for (int32_t i = 0; i < matrixArray.size(); ++i) {
    const std::optional<WmtsMatrix> matrix =
        matrixFromJson(matrixArray.at(i).toObject());
    if (!matrix) {
      return fail(errorString,
                  QCoreApplication::translate(
                      "ts::WmtsSource",
                      "The WMTS source has a tile matrix that makes "
                      "no sense."));
    }
    source.matrices.push_back(*matrix);
  }
  if (source.matrices.empty() || source.matrices.size() > maximumMatrices) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WmtsSource",
                                 "The WMTS source has no tile matrices, or too "
                                 "many."));
  }
  std::stable_sort(source.matrices.begin(), source.matrices.end(),
                   [](const WmtsMatrix& a, const WmtsMatrix& b) {
                     return a.pixelSize > b.pixelSize;
                   });
  const QJsonArray dimensionArray =
      json.value(QStringLiteral("dimensions")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert): the same on Qt 5 and Qt 6
  for (int32_t i = 0; i < dimensionArray.size(); ++i) {
    const QJsonArray pair = dimensionArray.at(i).toArray();
    if (pair.size() == 2 && !pair.at(0).toString().isEmpty()) {
      source.dimensions.emplace_back(pair.at(0).toString(),
                                     pair.at(1).toString());
    }
  }
  source.tileSizePx = std::clamp(
      json.value(QStringLiteral("tileSize")).toInt(source.tileSizePx), 64,
      1024);
  source.maximumLevel =
      std::clamp(json.value(QStringLiteral("maximumLevel"))
                     .toInt(deepestLevel(source.matrices.back().pixelSize,
                                         source.crs, source.tileSizePx)),
                 0, TileKey::maximumLevel);
  const QJsonArray boundsArray = json.value(QStringLiteral("bounds")).toArray();
  if (boundsArray.size() == 4) {
    const GeoRect rect{
        boundsArray.at(0).toDouble(), boundsArray.at(1).toDouble(),
        boundsArray.at(2).toDouble(), boundsArray.at(3).toDouble()};
    if (rect.westDeg <= rect.eastDeg && rect.southDeg <= rect.northDeg) {
      source.bounds = rect;
    }
  }
  source.title = json.value(QStringLiteral("title")).toString();
  source.attribution = json.value(QStringLiteral("attribution")).toString();
  return source;
}

// --- CRSs and matrices
// ---------------------------------------------------------

QString normalisedWmtsCrs(const QString& supportedCrs)
{
  const QString crs = supportedCrs.trimmed();
  const QString lower = crs.toLower();
  if (lower.endsWith(QLatin1String("crs84")) ||
      lower == QLatin1String("crs:84")) {
    return QStringLiteral("CRS:84");
  }
  // urn:ogc:def:crs:EPSG:6.18.3:3857, urn:ogc:def:crs:EPSG::4326,
  // http://www.opengis.net/def/crs/EPSG/0/3857, EPSG:3857.
  static const QRegularExpression code(
      QStringLiteral("epsg[:/](?:.*[:/])?(\\d+)$"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = code.match(crs);
  if (!match.hasMatch()) {
    return QString();
  }
  const QString epsg = QStringLiteral("EPSG:") + match.captured(1);
  return isWebMercatorCrs(epsg) ? QStringLiteral("EPSG:3857") : epsg;
}

bool isSupportedWmtsCrs(const QString& crs)
{
  if (crs.isEmpty()) {
    return false;
  }
  return crs == QLatin1String("CRS:84") || crs == QLatin1String("EPSG:4326") ||
         isWebMercator(crs) || wmtsCrsInfo(crs).has_value();
}

bool isProjectedWmtsCrs(const QString& crs)
{
  return isSupportedWmtsCrs(crs) && !isGeographic(crs) && !isWebMercator(crs);
}

double wmtsPixelSize(const WmtsTileMatrix& matrix, const QString& crs)
{
  return matrix.scaleDenominator * wmtsPixelSizeM / metresPerUnit(crs);
}

WmtsMatrix wmtsMatrix(const WmtsTileMatrix& matrix, const QString& crs)
{
  WmtsMatrix result;
  result.identifier = matrix.identifier;
  result.pixelSize = wmtsPixelSize(matrix, crs);
  result.originX = matrix.topLeftFirst;
  result.originY = matrix.topLeftSecond;
  // EPSG:4326 is latitude first (as other EPSG geographic CRSs, and some
  // projected ones such as EPSG:3035), but a geographic corner's first
  // value beyond ±90 must be a longitude: the server wrote it the other
  // way round.
  const std::optional<WmtsCrsInfo> info =
      crs == QLatin1String("CRS:84") || isWebMercator(crs) ? std::nullopt
                                                           : wmtsCrsInfo(crs);
  const bool northingFirst =
      crs == QLatin1String("EPSG:4326") || (info && info->northingFirst);
  const bool writtenEastingFirst =
      isGeographic(crs) && std::abs(matrix.topLeftFirst) > 90.0;
  if (northingFirst && !writtenEastingFirst) {
    result.originX = matrix.topLeftSecond;
    result.originY = matrix.topLeftFirst;
  }
  result.tileWidth = matrix.tileWidth;
  result.tileHeight = matrix.tileHeight;
  result.matrixWidth = matrix.matrixWidth;
  result.matrixHeight = matrix.matrixHeight;
  result.maxRow = matrix.matrixHeight - 1;
  result.maxColumn = matrix.matrixWidth - 1;
  return result;
}

std::optional<GeoRect> wmtsExtent(const WmtsSource& source, TileKey key)
{
  const GeoRect rect = key.bounds();
  if (isProjectedWmtsCrs(source.crs)) {
    // Not a rectangle there: see WmtsImageryProvider.
    return std::nullopt;
  }
  if (!isWebMercator(source.crs)) {
    return rect;
  }
  const std::optional<GeoRect> clipped = webMercatorCoverage(rect);
  if (!clipped) {
    return std::nullopt;
  }
  const double a = wgs84::semiMajorAxisM;
  return GeoRect{a * degToRad(clipped->westDeg),
                 webMercator().northingM(clipped->southDeg),
                 a * degToRad(clipped->eastDeg),
                 webMercator().northingM(clipped->northDeg)};
}

int32_t wmtsMatrixFor(const WmtsSource& source, TileKey key)
{
  if (source.matrices.empty()) {
    return 0;
  }
  // The quadtree tile's pixel size: level n tiles are 180° / 2^n wide.
  const double target = levelZeroWidth(source.crs) /
                        std::ldexp(1.0, key.level) /
                        static_cast<double>(std::max(source.tileSizePx, 1));
  for (std::size_t i = 0; i < source.matrices.size(); ++i) {
    if (source.matrices[i].pixelSize <= target * 1.01) {
      return static_cast<int32_t>(i);
    }
  }
  return static_cast<int32_t>(source.matrices.size() - 1);
}

std::optional<WmtsTileWindow> wmtsTileWindow(const WmtsSource& source,
                                             TileKey key, int32_t matrix)
{
  if (matrix < 0 ||
      static_cast<std::size_t>(matrix) >= source.matrices.size()) {
    return std::nullopt;
  }
  if (source.bounds) {
    const GeoRect rect = key.bounds();
    const GeoRect& bounds = *source.bounds;
    if (rect.eastDeg <= bounds.westDeg || rect.westDeg >= bounds.eastDeg ||
        rect.northDeg <= bounds.southDeg || rect.southDeg >= bounds.northDeg) {
      return std::nullopt;
    }
  }
  const std::optional<GeoRect> extent = wmtsExtent(source, key);
  if (!extent) {
    return std::nullopt;
  }
  return wmtsTileWindowForExtent(source, *extent, matrix);
}

std::optional<WmtsTileWindow> wmtsTileWindowForExtent(const WmtsSource& source,
                                                      const GeoRect& extent,
                                                      int32_t matrix)
{
  if (matrix < 0 ||
      static_cast<std::size_t>(matrix) >= source.matrices.size() ||
      !(extent.westDeg <= extent.eastDeg) ||
      !(extent.southDeg <= extent.northDeg)) {
    return std::nullopt;
  }
  const WmtsMatrix& m = source.matrices[static_cast<std::size_t>(matrix)];
  // The extent in the matrix's pixels, from its top-left corner.
  const double left = (extent.westDeg - m.originX) / m.pixelSize;
  const double right = (extent.eastDeg - m.originX) / m.pixelSize;
  const double top = (m.originY - extent.northDeg) / m.pixelSize;
  const double bottom = (m.originY - extent.southDeg) / m.pixelSize;
  const double tileWidth = m.tileWidth;
  const double tileHeight = m.tileHeight;
  // A sliver of a pixel past a tile edge doesn't need the next tile.
  const double edge = 1e-6;
  const int64_t firstColumn =
      static_cast<int64_t>(std::floor(left / tileWidth + edge));
  const int64_t lastColumn =
      static_cast<int64_t>(std::ceil(right / tileWidth - edge)) - 1;
  const int64_t firstRow =
      static_cast<int64_t>(std::floor(top / tileHeight + edge));
  const int64_t lastRow =
      static_cast<int64_t>(std::ceil(bottom / tileHeight - edge)) - 1;
  // Some of the block must exist.
  if (std::max(firstColumn, m.minColumn) > std::min(lastColumn, m.maxColumn) ||
      std::max(firstRow, m.minRow) > std::min(lastRow, m.maxRow)) {
    return std::nullopt;
  }
  const int64_t columns = lastColumn - firstColumn + 1;
  const int64_t rows = lastRow - firstRow + 1;
  if (columns < 1 || rows < 1 || columns * rows > maximumTilesPerQuadtreeTile) {
    return std::nullopt;
  }
  WmtsTileWindow window;
  window.matrix = matrix;
  window.firstColumn = firstColumn;
  window.firstRow = firstRow;
  window.columns = static_cast<int32_t>(columns);
  window.rows = static_cast<int32_t>(rows);
  window.left = left - static_cast<double>(firstColumn) * tileWidth;
  window.top = top - static_cast<double>(firstRow) * tileHeight;
  window.width = right - left;
  window.height = bottom - top;
  return window;
}

bool wmtsTileExists(const WmtsSource& source, const WmtsTileIndex& tile)
{
  if (tile.matrix < 0 ||
      static_cast<std::size_t>(tile.matrix) >= source.matrices.size()) {
    return false;
  }
  const WmtsMatrix& m = source.matrices[static_cast<std::size_t>(tile.matrix)];
  return tile.column >= m.minColumn && tile.column <= m.maxColumn &&
         tile.row >= m.minRow && tile.row <= m.maxRow;
}

QUrl getTileUrl(const WmtsSource& source, const WmtsTileIndex& tile)
{
  const QString matrixId =
      source.matrices.at(static_cast<std::size_t>(tile.matrix)).identifier;
  if (!source.urlTemplate.isEmpty()) {
    static const QRegularExpression variable(QStringLiteral("\\{([^}]+)\\}"));
    QString filled;
    int32_t from = 0;
    QRegularExpressionMatchIterator matches =
        variable.globalMatch(source.urlTemplate);
    while (matches.hasNext()) {
      const QRegularExpressionMatch match = matches.next();
      const QString name = match.captured(1);
      QString value;
      bool known = true;
      if (name.compare(QLatin1String("TileMatrixSet"), Qt::CaseInsensitive) ==
          0) {
        value = source.tileMatrixSet;
      } else if (name.compare(QLatin1String("TileMatrix"),
                              Qt::CaseInsensitive) == 0) {
        value = matrixId;
      } else if (name.compare(QLatin1String("TileRow"), Qt::CaseInsensitive) ==
                 0) {
        value = QString::number(tile.row);
      } else if (name.compare(QLatin1String("TileCol"), Qt::CaseInsensitive) ==
                 0) {
        value = QString::number(tile.column);
      } else if (name.compare(QLatin1String("Style"), Qt::CaseInsensitive) ==
                 0) {
        value = source.style;
      } else {
        const std::vector<std::pair<QString, QString>>::const_iterator
            dimension = std::find_if(
                source.dimensions.begin(), source.dimensions.end(),
                [&name](const std::pair<QString, QString>& candidate) {
                  return candidate.first.compare(name, Qt::CaseInsensitive) ==
                         0;
                });
        known = dimension != source.dimensions.end();
        if (known) {
          value = dimension->second;
        }
      }
      filled += source.urlTemplate.mid(
          from, static_cast<int32_t>(match.capturedStart()) - from);
      filled += known ? QString::fromLatin1(QUrl::toPercentEncoding(value))
                      : match.captured(0);
      from = static_cast<int32_t>(match.capturedEnd());
    }
    filled += source.urlTemplate.mid(from);
    // Tolerant: braces of a variable left as it is get percent-encoded.
    return QUrl(filled, QUrl::TolerantMode);
  }
  QUrl url = source.url;
  const QUrlQuery original(url);
  QUrlQuery query;
  const QList<QPair<QString, QString>> items =
      original.queryItems(QUrl::FullyDecoded);
  for (const QPair<QString, QString>& item : items) {
    const bool isDimension = std::any_of(
        source.dimensions.begin(), source.dimensions.end(),
        [&item](const std::pair<QString, QString>& dimension) {
          return dimension.first.compare(item.first, Qt::CaseInsensitive) == 0;
        });
    if (!isWmtsParameter(item.first) && !isDimension) {
      query.addQueryItem(item.first, item.second);
    }
  }
  query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMTS"));
  query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetTile"));
  query.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("1.0.0"));
  query.addQueryItem(QStringLiteral("LAYER"), source.layer);
  query.addQueryItem(QStringLiteral("STYLE"), source.style);
  query.addQueryItem(QStringLiteral("FORMAT"), source.format);
  query.addQueryItem(QStringLiteral("TILEMATRIXSET"), source.tileMatrixSet);
  query.addQueryItem(QStringLiteral("TILEMATRIX"), matrixId);
  query.addQueryItem(QStringLiteral("TILEROW"), QString::number(tile.row));
  query.addQueryItem(QStringLiteral("TILECOL"), QString::number(tile.column));
  for (const std::pair<QString, QString>& dimension : source.dimensions) {
    query.addQueryItem(dimension.first, dimension.second);
  }
  // Percent-encode what QUrlQuery leaves alone (+ would read as a space).
  QString encoded = query.toString(QUrl::FullyEncoded);
  encoded.replace(QLatin1Char('+'), QLatin1String("%2B"));
  url.setQuery(encoded, QUrl::StrictMode);
  return url;
}

QUrl wmtsCapabilitiesUrl(const QUrl& serviceUrl)
{
  const QUrlQuery original(serviceUrl);
  const bool asksForCapabilities =
      original.queryItemValue(QStringLiteral("REQUEST"))
              .compare(QLatin1String("GetCapabilities"), Qt::CaseInsensitive) ==
          0 ||
      original.queryItemValue(QStringLiteral("request"))
              .compare(QLatin1String("GetCapabilities"), Qt::CaseInsensitive) ==
          0;
  if (serviceUrl.path().endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) ||
      asksForCapabilities) {
    return serviceUrl;
  }
  QUrl url = serviceUrl;
  QUrlQuery query;
  const QList<QPair<QString, QString>> items =
      original.queryItems(QUrl::FullyDecoded);
  for (const QPair<QString, QString>& item : items) {
    if (!isWmtsParameter(item.first)) {
      query.addQueryItem(item.first, item.second);
    }
  }
  query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMTS"));
  query.addQueryItem(QStringLiteral("REQUEST"),
                     QStringLiteral("GetCapabilities"));
  query.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("1.0.0"));
  url.setQuery(query);
  return url;
}

QStringList usableTileMatrixSets(const WmtsCapabilities& capabilities,
                                 const QString& layerId)
{
  QStringList identifiers;
  const WmtsLayer* layer = capabilities.findLayer(layerId);
  if (layer == nullptr) {
    return identifiers;
  }
  for (const UsableSet& set : usableSets(capabilities, *layer)) {
    identifiers.append(set.set->identifier);
  }
  return identifiers;
}

std::optional<WmtsSource> wmtsSourceFor(const WmtsCapabilities& capabilities,
                                        const QString& layerId,
                                        const WmtsChoices& choices,
                                        QString* errorString)
{
  const WmtsLayer* layer = capabilities.findLayer(layerId);
  if (layer == nullptr) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmtsSource",
                                            "The service has no layer %1.")
                    .arg(layerId));
  }
  const std::vector<UsableSet> sets = usableSets(capabilities, *layer);
  if (sets.empty()) {
    return fail(errorString, noUsableSetMessage(capabilities, *layer));
  }
  const std::vector<UsableSet>::const_iterator chosen =
      choices.tileMatrixSet.isEmpty()
          ? sets.begin()
          : std::find_if(sets.begin(), sets.end(),
                         [&choices](const UsableSet& set) {
                           return set.set->identifier == choices.tileMatrixSet;
                         });
  if (chosen == sets.end()) {
    return fail(
        errorString,
        QCoreApplication::translate(
            "ts::WmtsSource", "Layer %1 can't be drawn in tile matrix set %2.")
            .arg(layerId, choices.tileMatrixSet));
  }
  WmtsSource source;
  source.layer = layerId;
  source.tileMatrixSet = chosen->set->identifier;
  source.crs = chosen->crs;
  source.matrices = chosen->matrices;
  if (!chooseStyle(*layer, choices.style, source, errorString) ||
      !chooseAddress(capabilities, *layer, choices.format, source,
                     errorString) ||
      !chooseDimensions(*layer, choices.dimensions, source, errorString)) {
    return std::nullopt;
  }
  source.bounds = layer->wgs84Bounds;
  if (isProjectedWmtsCrs(source.crs)) {
    // Only where the CRS is meant to be used: far from it a projection
    // runs wild, or can't be computed at all.
    const std::optional<WmtsCrsInfo> info = wmtsCrsInfo(source.crs);
    if (info && info->areaOfUse) {
      source.bounds = source.bounds
                          ? intersection(*source.bounds, *info->areaOfUse)
                          : info->areaOfUse;
    }
  }
  source.maximumLevel = deepestLevel(source.matrices.back().pixelSize,
                                     source.crs, source.tileSizePx);
  source.title = layer->title.isEmpty() ? layerId : layer->title;
  source.attribution = capabilities.providerName.isEmpty()
                           ? capabilities.title
                           : capabilities.providerName;
  return source;
}

} // namespace ts
