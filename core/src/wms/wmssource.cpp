// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/wms/wmssource.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QCoreApplication>
#include <QJsonArray>
#include <QLocale>
#include <QUrlQuery>

#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/wms/wmscapabilities.h"

namespace ts
{

namespace
{

/** WMS parameters a base URL may carry that requests set themselves. */
const std::array<const char*, 14> wmsParameters = {
    "service", "request",     "version",    "layers", "styles",
    "crs",     "srs",         "bbox",       "width",  "height",
    "format",  "transparent", "exceptions", "bgcolor"};

bool isWmsParameter(const QString& key)
{
  return std::any_of(
      wmsParameters.begin(), wmsParameters.end(), [&key](const char* name) {
        return key.compare(QLatin1String(name), Qt::CaseInsensitive) == 0;
      });
}

/** The query items of @p url that aren't WMS parameters. */
QUrlQuery foreignItems(const QUrl& url)
{
  const QUrlQuery original(url);
  QUrlQuery kept;
  const QList<QPair<QString, QString>> items =
      original.queryItems(QUrl::FullyDecoded);
  for (const QPair<QString, QString>& item : items) {
    if (!isWmsParameter(item.first)) {
      kept.addQueryItem(item.first, item.second);
    }
  }
  return kept;
}

QString shortest(double value)
{
  return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

bool sameCode(const QString& a, const char* b)
{
  return a.compare(QLatin1String(b), Qt::CaseInsensitive) == 0;
}

bool isVersion13(const QString& version)
{
  return version.startsWith(QLatin1String("1.3"));
}

const WebMercatorProjection& webMercator()
{
  static const WebMercatorProjection projection;
  return projection;
}

std::optional<WmsSource> fail(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return std::nullopt;
}

} // namespace

// --- WmsSource
// ----------------------------------------------------------------

QString WmsSource::id() const
{
  return QStringLiteral("wms:%1|%2|%3|%4|%5|%6|%7|%8")
      .arg(url.toString(QUrl::FullyEncoded), version,
           layers.join(QLatin1Char(',')), styles.join(QLatin1Char(',')), format,
           crs, transparent ? QStringLiteral("t") : QStringLiteral("o"))
      .arg(tileSizePx);
}

TileImageProjection WmsSource::imageProjection() const
{
  return isWebMercatorCrs(crs) ? TileImageProjection::WebMercator
                               : TileImageProjection::Geographic;
}

QJsonObject WmsSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), QStringLiteral("wms"));
  json.insert(QStringLiteral("url"), url.toString(QUrl::FullyEncoded));
  json.insert(QStringLiteral("version"), version);
  json.insert(QStringLiteral("layers"), QJsonArray::fromStringList(layers));
  json.insert(QStringLiteral("styles"), QJsonArray::fromStringList(styles));
  json.insert(QStringLiteral("format"), format);
  json.insert(QStringLiteral("transparent"), transparent);
  json.insert(QStringLiteral("crs"), crs);
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

std::optional<WmsSource> WmsSource::fromJson(const QJsonObject& json,
                                             QString* errorString)
{
  WmsSource source;
  source.url = QUrl(json.value(QStringLiteral("url")).toString());
  if (!source.url.isValid() ||
      (source.url.scheme() != QLatin1String("http") &&
       source.url.scheme() != QLatin1String("https"))) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsSource",
                    "The WMS layer has no valid http or https address."));
  }
  source.version =
      json.value(QStringLiteral("version")).toString(source.version);
  const QJsonArray layers = json.value(QStringLiteral("layers")).toArray();
  const QJsonArray styles = json.value(QStringLiteral("styles")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < layers.size(); ++i) {
    const QString name = layers.at(i).toString();
    if (!name.isEmpty()) {
      source.layers.append(name);
      source.styles.append(styles.at(i).toString());
    }
  }
  if (source.layers.isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmsSource",
                                            "The WMS layer names no layers."));
  }
  source.format = json.value(QStringLiteral("format")).toString(source.format);
  source.transparent = json.value(QStringLiteral("transparent"))
                           .toBool(isTransparentFormat(source.format));
  source.crs = json.value(QStringLiteral("crs")).toString(source.crs);
  if (!isGeographicCrs(source.crs) && !isWebMercatorCrs(source.crs)) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsSource",
                    "The WMS layer asks for %1, which Terra Soar can't "
                    "draw.")
                    .arg(source.crs));
  }
  source.tileSizePx = std::clamp(
      json.value(QStringLiteral("tileSize")).toInt(source.tileSizePx), 16,
      4096);
  source.maximumLevel = std::clamp(
      json.value(QStringLiteral("maximumLevel")).toInt(source.maximumLevel), 0,
      TileKey::maximumLevel);
  const QJsonArray bounds = json.value(QStringLiteral("bounds")).toArray();
  if (bounds.size() == 4) {
    const GeoRect rect{bounds.at(0).toDouble(), bounds.at(1).toDouble(),
                       bounds.at(2).toDouble(), bounds.at(3).toDouble()};
    if (rect.westDeg <= rect.eastDeg && rect.southDeg <= rect.northDeg) {
      source.bounds = rect;
    }
  }
  source.title = json.value(QStringLiteral("title")).toString();
  source.attribution = json.value(QStringLiteral("attribution")).toString();
  return source;
}

// --- Choices
// ------------------------------------------------------------------

bool isGeographicCrs(const QString& crs)
{
  return sameCode(crs, "EPSG:4326") || sameCode(crs, "CRS:84");
}

bool isWebMercatorCrs(const QString& crs)
{
  return sameCode(crs, "EPSG:3857") || sameCode(crs, "EPSG:900913") ||
         sameCode(crs, "EPSG:3785") || sameCode(crs, "EPSG:102100") ||
         sameCode(crs, "EPSG:102113");
}

QString chooseCrs(const QStringList& offered, const QString& version)
{
  std::vector<const char*> preference;
  if (isVersion13(version)) {
    preference = {"CRS:84", "EPSG:4326"};
  } else {
    preference = {"EPSG:4326"};
  }
  for (const char* code : {"EPSG:3857", "EPSG:900913", "EPSG:102100",
                           "EPSG:3785", "EPSG:102113"}) {
    preference.push_back(code);
  }
  for (const char* wanted : preference) {
    for (const QString& code : offered) {
      if (sameCode(code, wanted)) {
        return QString::fromLatin1(wanted); // in its usual spelling
      }
    }
  }
  return QString();
}

QString chooseFormat(const QStringList& offered, bool opaque)
{
  const std::vector<const char*> preference =
      opaque ? std::vector<const char*>{"image/jpeg", "image/png", "image/png8",
                                        "image/png; mode=8bit", "image/gif"}
             : std::vector<const char*>{"image/png", "image/png8",
                                        "image/png; mode=8bit", "image/gif",
                                        "image/jpeg"};
  for (const char* wanted : preference) {
    for (const QString& format : offered) {
      if (sameCode(format.trimmed(), wanted)) {
        return format.trimmed();
      }
    }
  }
  return QString();
}

bool isTransparentFormat(const QString& format)
{
  return format.contains(QLatin1String("png"), Qt::CaseInsensitive) ||
         format.contains(QLatin1String("gif"), Qt::CaseInsensitive);
}

// --- Requests
// -----------------------------------------------------------------

std::optional<GeoRect> webMercatorCoverage(const GeoRect& rect)
{
  const double limit = webMercator().maximumLatDeg();
  const GeoRect clipped{rect.westDeg, std::max(rect.southDeg, -limit),
                        rect.eastDeg, std::min(rect.northDeg, limit)};
  if (!(clipped.southDeg < clipped.northDeg)) {
    return std::nullopt;
  }
  return clipped;
}

QString bboxParameter(const QString& version, const QString& crs,
                      const GeoRect& rect)
{
  const QString comma = QStringLiteral(",");
  if (isWebMercatorCrs(crs)) {
    const std::optional<GeoRect> covered = webMercatorCoverage(rect);
    if (!covered) {
      return QString();
    }
    // x without wrapping: the east edge of the world is +πa, not −πa.
    const double a = wgs84::semiMajorAxisM;
    return shortest(a * degToRad(covered->westDeg)) + comma +
           shortest(webMercator().northingM(covered->southDeg)) + comma +
           shortest(a * degToRad(covered->eastDeg)) + comma +
           shortest(webMercator().northingM(covered->northDeg));
  }
  if (!isGeographicCrs(crs)) {
    return QString();
  }
  if (isVersion13(version) && sameCode(crs, "EPSG:4326")) {
    return shortest(rect.southDeg) + comma + shortest(rect.westDeg) + comma +
           shortest(rect.northDeg) + comma + shortest(rect.eastDeg);
  }
  return shortest(rect.westDeg) + comma + shortest(rect.southDeg) + comma +
         shortest(rect.eastDeg) + comma + shortest(rect.northDeg);
}

QUrl getMapUrl(const WmsSource& source, TileKey key)
{
  const GeoRect rect = key.bounds();
  if (source.bounds && (rect.eastDeg <= source.bounds->westDeg ||
                        rect.westDeg >= source.bounds->eastDeg ||
                        rect.northDeg <= source.bounds->southDeg ||
                        rect.southDeg >= source.bounds->northDeg)) {
    return QUrl();
  }
  const QString bbox = bboxParameter(source.version, source.crs, rect);
  if (bbox.isEmpty()) {
    return QUrl();
  }
  QUrlQuery query = foreignItems(source.url);
  const QString size = QString::number(source.tileSizePx);
  query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMS"));
  query.addQueryItem(QStringLiteral("VERSION"), source.version);
  query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetMap"));
  query.addQueryItem(QStringLiteral("LAYERS"),
                     source.layers.join(QLatin1Char(',')));
  query.addQueryItem(QStringLiteral("STYLES"),
                     source.styles.join(QLatin1Char(',')));
  query.addQueryItem(isVersion13(source.version) ? QStringLiteral("CRS")
                                                 : QStringLiteral("SRS"),
                     source.crs);
  query.addQueryItem(QStringLiteral("BBOX"), bbox);
  query.addQueryItem(QStringLiteral("WIDTH"), size);
  query.addQueryItem(QStringLiteral("HEIGHT"), size);
  query.addQueryItem(QStringLiteral("FORMAT"), source.format);
  query.addQueryItem(QStringLiteral("TRANSPARENT"),
                     source.transparent ? QStringLiteral("TRUE")
                                        : QStringLiteral("FALSE"));
  query.addQueryItem(QStringLiteral("EXCEPTIONS"),
                     isVersion13(source.version)
                         ? QStringLiteral("XML")
                         : QStringLiteral("application/vnd.ogc.se_xml"));
  QUrl url = source.url;
  url.setQuery(query);
  return url;
}

QUrl capabilitiesUrl(const QUrl& serviceUrl, const QString& version)
{
  QUrlQuery query = foreignItems(serviceUrl);
  query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMS"));
  query.addQueryItem(QStringLiteral("REQUEST"),
                     QStringLiteral("GetCapabilities"));
  query.addQueryItem(QStringLiteral("VERSION"), version);
  QUrl url = serviceUrl;
  url.setQuery(query);
  return url;
}

namespace
{

QString layerTitle(const WmsLayer& layer)
{
  return layer.title.isEmpty() ? layer.name : layer.title;
}

/** Why @p layer (asked for as @p name) can't be drawn as tiles, or empty. */
QString unusableLayerReason(const WmsLayer* layer, const QString& name)
{
  if (layer == nullptr) {
    return QCoreApplication::translate("ts::WmsSource",
                                       "The server has no layer \"%1\".")
        .arg(name);
  }
  if (layer->noSubsets || layer->fixedWidth > 0 || layer->fixedHeight > 0) {
    return QCoreApplication::translate(
               "ts::WmsSource",
               "\"%1\" can only be drawn whole, at a fixed size, "
               "which doesn't work as tiles.")
        .arg(layerTitle(*layer));
  }
  return QString();
}

/** The codes in @p crs that @p layer supports too. */
QStringList crsInCommon(const QStringList& crs, const WmsLayer& layer)
{
  QStringList both;
  for (const QString& code : crs) {
    if (layer.supportsCrs(code)) {
      both.append(code);
    }
  }
  return both;
}

/** The rectangle around both, or none if either is unknown. */
std::optional<GeoRect> unitedBounds(const std::optional<GeoRect>& a,
                                    const std::optional<GeoRect>& b)
{
  if (!a || !b) {
    return std::nullopt;
  }
  return GeoRect{
      std::min(a->westDeg, b->westDeg), std::min(a->southDeg, b->southDeg),
      std::max(a->eastDeg, b->eastDeg), std::max(a->northDeg, b->northDeg)};
}

/** @p tileSizePx halved until the server can draw it. */
int tileSizeWithin(const WmsCapabilities& capabilities, int tileSizePx)
{
  const int limit =
      std::min(capabilities.maxWidth > 0 ? capabilities.maxWidth : 4096,
               capabilities.maxHeight > 0 ? capabilities.maxHeight : 4096);
  while (tileSizePx > limit && tileSizePx > 16) {
    tileSizePx /= 2;
  }
  return tileSizePx;
}

} // namespace

// Layers and their styles are parallel lists, as LAYERS and STYLES are in
// the request itself.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::optional<WmsSource> wmsSourceFor(const WmsCapabilities& capabilities,
                                      const QStringList& layerNames,
                                      const QStringList& styles,
                                      const QString& format,
                                      QString* errorString)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  if (layerNames.isEmpty()) {
    return fail(errorString, QCoreApplication::translate("ts::WmsSource",
                                                         "Choose a layer."));
  }
  if (capabilities.layerLimit > 0 &&
      layerNames.size() > capabilities.layerLimit) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::WmsSource",
                                 "The server draws at most %1 layers at once.")
                                 .arg(capabilities.layerLimit));
  }
  WmsSource source;
  source.url = capabilities.getMapUrl;
  source.version = capabilities.version.isEmpty() ? QStringLiteral("1.3.0")
                                                  : capabilities.version;
  bool opaque = false;
  QStringList titles;
  QStringList commonCrs;
  QStringList attributions;
  for (int i = 0; i < layerNames.size(); ++i) {
    const WmsLayer* layer = capabilities.findLayer(layerNames.at(i));
    const QString problem = unusableLayerReason(layer, layerNames.at(i));
    if (!problem.isEmpty()) {
      return fail(errorString, problem);
    }
    if (i == 0) {
      commonCrs = layer->crs;
      source.bounds = layer->geographicBounds;
    } else {
      commonCrs = crsInCommon(commonCrs, *layer);
      source.bounds = unitedBounds(source.bounds, layer->geographicBounds);
    }
    opaque = opaque || layer->opaque;
    titles.append(layerTitle(*layer));
    if (!layer->attributionTitle.isEmpty() &&
        !attributions.contains(layer->attributionTitle)) {
      attributions.append(layer->attributionTitle);
    }
    source.layers.append(layer->name);
    source.styles.append(styles.value(i));
  }
  source.crs = chooseCrs(commonCrs, source.version);
  if (source.crs.isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsSource",
                    "The server can't draw this in a coordinate system "
                    "Terra Soar can use (EPSG:4326, CRS:84 or Web "
                    "Mercator)."));
  }
  source.format = format.isEmpty()
                      ? chooseFormat(capabilities.getMapFormats, opaque)
                      : format;
  if (source.format.isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsSource",
                    "The server offers no image format Terra Soar can "
                    "show (PNG, JPEG or GIF)."));
  }
  source.transparent = isTransparentFormat(source.format) && !opaque;
  // Tiles no larger than the server draws.
  source.tileSizePx = tileSizeWithin(capabilities, source.tileSizePx);
  source.title = titles.join(QStringLiteral(", "));
  source.attribution = attributions.join(QStringLiteral("; "));
  return source;
}

} // namespace ts
