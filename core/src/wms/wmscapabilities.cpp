// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/wms/wmscapabilities.h"

#include <algorithm>
#include <functional>

#include <QCoreApplication>
#include <QXmlStreamReader>

namespace ts
{

namespace
{

/** A layer as written, before inheritance. */
struct RawLayer {
  WmsLayer own;
  std::optional<bool> opaque;
  std::optional<bool> queryable;
  std::optional<bool> noSubsets;
  std::optional<int> fixedWidth;
  std::optional<int> fixedHeight;
  bool hasScale = false;
  std::vector<RawLayer> children;
};

QString attribute(const QXmlStreamReader& reader, QLatin1String name)
{
  // Attributes are matched by local name (xlink:href is "href").
  const QXmlStreamAttributes attributes = reader.attributes();
  for (const QXmlStreamAttribute& candidate : attributes) {
    if (candidate.name() == name) {
      return candidate.value().toString();
    }
  }
  return QString();
}

std::optional<bool> boolAttribute(const QXmlStreamReader& reader,
                                  QLatin1String name)
{
  const QString value = attribute(reader, name).trimmed();
  if (value.isEmpty()) {
    return std::nullopt;
  }
  return value == QLatin1String("1") ||
         value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

std::optional<int> intAttribute(const QXmlStreamReader& reader,
                                QLatin1String name)
{
  bool ok = false;
  const int value = attribute(reader, name).trimmed().toInt(&ok);
  return ok ? std::optional<int>(value) : std::nullopt;
}

double number(const QString& text, bool* ok)
{
  return text.trimmed().toDouble(ok);
}

QString text(QXmlStreamReader& reader)
{
  return reader.readElementText(QXmlStreamReader::IncludeChildElements)
      .trimmed();
}

/** The href of the first OnlineResource under the current element. */
QUrl onlineResource(QXmlStreamReader& reader)
{
  QUrl url;
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("OnlineResource") && url.isEmpty()) {
      url = QUrl(attribute(reader, QLatin1String("href")).trimmed());
      reader.skipCurrentElement();
    } else {
      reader.skipCurrentElement();
    }
  }
  return url;
}

std::optional<GeoRect> rectFromLonLat(double west, double south, double east,
                                      double north)
{
  if (!(west <= east && south <= north)) {
    return std::nullopt;
  }
  return GeoRect{
      std::clamp(west, -180.0, 180.0), std::clamp(south, -90.0, 90.0),
      std::clamp(east, -180.0, 180.0), std::clamp(north, -90.0, 90.0)};
}

void parseStyle(QXmlStreamReader& reader, WmsStyle& style)
{
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("Name")) {
      style.name = text(reader);
    } else if (reader.name() == QLatin1String("Title")) {
      style.title = text(reader);
    } else if (reader.name() == QLatin1String("LegendURL")) {
      style.legendUrl = onlineResource(reader);
    } else {
      reader.skipCurrentElement();
    }
  }
}

void parseLayer(QXmlStreamReader& reader, RawLayer& layer)
{
  layer.opaque = boolAttribute(reader, QLatin1String("opaque"));
  layer.queryable = boolAttribute(reader, QLatin1String("queryable"));
  layer.noSubsets = boolAttribute(reader, QLatin1String("noSubsets"));
  layer.fixedWidth = intAttribute(reader, QLatin1String("fixedWidth"));
  layer.fixedHeight = intAttribute(reader, QLatin1String("fixedHeight"));
  WmsLayer& own = layer.own;
  std::optional<GeoRect> latLonBox;
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("Name")) {
      own.name = text(reader);
    } else if (name == QLatin1String("Title")) {
      own.title = text(reader);
    } else if (name == QLatin1String("Abstract")) {
      own.abstract = text(reader);
    } else if (name == QLatin1String("CRS") || name == QLatin1String("SRS")) {
      // WMS 1.1.0 allowed several codes in one element.
      const QStringList codes =
          text(reader).split(QLatin1Char(' '), Qt::SkipEmptyParts);
      for (const QString& code : codes) {
        own.crs.append(code.trimmed());
      }
    } else if (name == QLatin1String("EX_GeographicBoundingBox")) {
      double west = 0.0;
      double east = 0.0;
      double south = 0.0;
      double north = 0.0;
      int found = 0;
      while (reader.readNextStartElement()) {
        bool ok = false;
        const QString side = reader.name().toString();
        const double value = number(text(reader), &ok);
        if (!ok) {
          continue;
        }
        if (side == QLatin1String("westBoundLongitude")) {
          west = value;
        } else if (side == QLatin1String("eastBoundLongitude")) {
          east = value;
        } else if (side == QLatin1String("southBoundLatitude")) {
          south = value;
        } else if (side == QLatin1String("northBoundLatitude")) {
          north = value;
        } else {
          continue;
        }
        ++found;
      }
      if (found == 4) {
        own.geographicBounds = rectFromLonLat(west, south, east, north);
      }
    } else if (name == QLatin1String("LatLonBoundingBox") ||
               name == QLatin1String("BoundingBox")) {
      bool ok = true;
      bool each = false;
      WmsBoundingBox box;
      box.crs = attribute(reader, QLatin1String("CRS")).trimmed();
      if (box.crs.isEmpty()) {
        box.crs = attribute(reader, QLatin1String("SRS")).trimmed();
      }
      box.minX = number(attribute(reader, QLatin1String("minx")), &each);
      ok = ok && each;
      box.minY = number(attribute(reader, QLatin1String("miny")), &each);
      ok = ok && each;
      box.maxX = number(attribute(reader, QLatin1String("maxx")), &each);
      ok = ok && each;
      box.maxY = number(attribute(reader, QLatin1String("maxy")), &each);
      ok = ok && each;
      if (ok && name == QLatin1String("LatLonBoundingBox")) {
        latLonBox = rectFromLonLat(box.minX, box.minY, box.maxX, box.maxY);
      } else if (ok) {
        own.boundingBoxes.push_back(box);
      }
      reader.skipCurrentElement();
    } else if (name == QLatin1String("Style")) {
      WmsStyle style;
      parseStyle(reader, style);
      own.styles.push_back(style);
    } else if (name == QLatin1String("MinScaleDenominator")) {
      own.minScaleDenominator = number(text(reader), nullptr);
      layer.hasScale = true;
    } else if (name == QLatin1String("MaxScaleDenominator")) {
      own.maxScaleDenominator = number(text(reader), nullptr);
      layer.hasScale = true;
    } else if (name == QLatin1String("ScaleHint")) {
      // WMS 1.1.1: diagonal ground size of a pixel, in metres. A pixel is
      // 0.28 mm, so the scale denominator is that / √2 / 0.00028.
      bool okMin = false;
      bool okMax = false;
      const double hintMin =
          number(attribute(reader, QLatin1String("min")), &okMin);
      const double hintMax =
          number(attribute(reader, QLatin1String("max")), &okMax);
      constexpr double perHint = 1.0 / (1.4142135623730951 * 0.00028);
      if (okMin && okMax) {
        own.minScaleDenominator = hintMin * perHint;
        own.maxScaleDenominator = hintMax * perHint;
        layer.hasScale = true;
      }
      reader.skipCurrentElement();
    } else if (name == QLatin1String("Attribution")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("Title")) {
          own.attributionTitle = text(reader);
        } else if (reader.name() == QLatin1String("OnlineResource")) {
          own.attributionUrl =
              QUrl(attribute(reader, QLatin1String("href")).trimmed());
          reader.skipCurrentElement();
        } else {
          reader.skipCurrentElement();
        }
      }
    } else if (name == QLatin1String("Layer")) {
      layer.children.emplace_back();
      parseLayer(reader, layer.children.back());
    } else {
      reader.skipCurrentElement();
    }
  }
  if (!own.geographicBounds) {
    own.geographicBounds = latLonBox;
  }
}

bool sameCode(const QString& a, const QString& b)
{
  return a.compare(b, Qt::CaseInsensitive) == 0;
}

/** Applies inheritance from @p parent (null at the top). */
WmsLayer resolve(const RawLayer& raw, const WmsLayer* parent, bool lonLatAxes)
{
  WmsLayer layer = raw.own;
  layer.children.clear();
  if (parent != nullptr) {
    // Added to the parent's.
    QStringList crs = parent->crs;
    for (const QString& code : raw.own.crs) {
      if (std::none_of(crs.begin(), crs.end(), [&code](const QString& known) {
            return sameCode(known, code);
          })) {
        crs.append(code);
      }
    }
    layer.crs = crs;
    std::vector<WmsStyle> styles = parent->styles;
    for (const WmsStyle& style : raw.own.styles) {
      if (std::none_of(styles.begin(), styles.end(),
                       [&style](const WmsStyle& known) {
                         return known.name == style.name;
                       })) {
        styles.push_back(style);
      }
    }
    layer.styles = styles;
    // Replaced when given.
    if (!layer.geographicBounds) {
      layer.geographicBounds = parent->geographicBounds;
    }
    if (layer.boundingBoxes.empty()) {
      layer.boundingBoxes = parent->boundingBoxes;
    }
    if (!raw.hasScale) {
      layer.minScaleDenominator = parent->minScaleDenominator;
      layer.maxScaleDenominator = parent->maxScaleDenominator;
    }
    if (layer.attributionTitle.isEmpty() && layer.attributionUrl.isEmpty()) {
      layer.attributionTitle = parent->attributionTitle;
      layer.attributionUrl = parent->attributionUrl;
    }
  }
  layer.opaque = raw.opaque.value_or(parent != nullptr && parent->opaque);
  layer.queryable =
      raw.queryable.value_or(parent != nullptr && parent->queryable);
  layer.noSubsets =
      raw.noSubsets.value_or(parent != nullptr && parent->noSubsets);
  layer.fixedWidth =
      raw.fixedWidth.value_or(parent != nullptr ? parent->fixedWidth : 0);
  layer.fixedHeight =
      raw.fixedHeight.value_or(parent != nullptr ? parent->fixedHeight : 0);
  // Without a geographic box, use a BoundingBox in a geographic CRS.
  if (!layer.geographicBounds) {
    for (const WmsBoundingBox& box : layer.boundingBoxes) {
      if (sameCode(box.crs, QStringLiteral("CRS:84")) ||
          (lonLatAxes && sameCode(box.crs, QStringLiteral("EPSG:4326")))) {
        layer.geographicBounds =
            rectFromLonLat(box.minX, box.minY, box.maxX, box.maxY);
      } else if (sameCode(box.crs, QStringLiteral("EPSG:4326"))) {
        layer.geographicBounds =
            rectFromLonLat(box.minY, box.minX, box.maxY, box.maxX);
      }
      if (layer.geographicBounds) {
        break;
      }
    }
  }
  for (const RawLayer& child : raw.children) {
    layer.children.push_back(resolve(child, &layer, lonLatAxes));
  }
  return layer;
}

void parseGetMap(QXmlStreamReader& reader, WmsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("Format")) {
      const QString format = text(reader);
      if (!format.isEmpty()) {
        capabilities.getMapFormats.append(format);
      }
    } else if (reader.name() == QLatin1String("DCPType")) {
      // DCPType / HTTP / Get / OnlineResource (Post is ignored).
      while (reader.readNextStartElement()) {
        if (reader.name() != QLatin1String("HTTP")) {
          reader.skipCurrentElement();
          continue;
        }
        while (reader.readNextStartElement()) {
          if (reader.name() == QLatin1String("Get") &&
              capabilities.getMapUrl.isEmpty()) {
            capabilities.getMapUrl = onlineResource(reader);
          } else {
            reader.skipCurrentElement();
          }
        }
      }
    } else {
      reader.skipCurrentElement();
    }
  }
}

void parseCapability(QXmlStreamReader& reader, WmsCapabilities& capabilities,
                     std::vector<RawLayer>& layers)
{
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("Request")) {
      while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("GetMap")) {
          parseGetMap(reader, capabilities);
        } else {
          reader.skipCurrentElement();
        }
      }
    } else if (reader.name() == QLatin1String("Layer")) {
      layers.emplace_back();
      parseLayer(reader, layers.back());
    } else {
      reader.skipCurrentElement();
    }
  }
}

void parseService(QXmlStreamReader& reader, WmsCapabilities& capabilities)
{
  while (reader.readNextStartElement()) {
    const QString name = reader.name().toString();
    if (name == QLatin1String("Title")) {
      capabilities.title = text(reader);
    } else if (name == QLatin1String("Abstract")) {
      capabilities.abstract = text(reader);
    } else if (name == QLatin1String("Fees")) {
      capabilities.fees = text(reader);
    } else if (name == QLatin1String("AccessConstraints")) {
      capabilities.accessConstraints = text(reader);
    } else if (name == QLatin1String("MaxWidth")) {
      capabilities.maxWidth = text(reader).toInt();
    } else if (name == QLatin1String("MaxHeight")) {
      capabilities.maxHeight = text(reader).toInt();
    } else if (name == QLatin1String("LayerLimit")) {
      capabilities.layerLimit = text(reader).toInt();
    } else {
      reader.skipCurrentElement();
    }
  }
}

std::optional<WmsCapabilities> fail(QString* errorString,
                                    const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return std::nullopt;
}

QString serviceExceptionText(QXmlStreamReader& reader)
{
  QStringList messages;
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("ServiceException")) {
      messages.append(text(reader));
    } else {
      reader.skipCurrentElement();
    }
  }
  return messages.join(QLatin1Char(' ')).simplified();
}

} // namespace

bool WmsLayer::supportsCrs(const QString& code) const
{
  return std::any_of(crs.begin(), crs.end(), [&code](const QString& known) {
    return sameCode(known, code);
  });
}

const WmsLayer* WmsCapabilities::findLayer(const QString& layerName) const
{
  for (const WmsLayer* layer : namedLayers()) {
    if (layer->name == layerName) {
      return layer;
    }
  }
  return nullptr;
}

std::vector<const WmsLayer*> WmsCapabilities::namedLayers() const
{
  std::vector<const WmsLayer*> result;
  const std::function<void(const WmsLayer&)> visit =
      [&result, &visit](const WmsLayer& layer) {
        if (!layer.name.isEmpty()) {
          result.push_back(&layer);
        }
        for (const WmsLayer& child : layer.children) {
          visit(child);
        }
      };
  for (const WmsLayer& layer : layers) {
    visit(layer);
  }
  return result;
}

std::optional<WmsCapabilities> parseWmsCapabilities(const QByteArray& document,
                                                    QString* errorString)
{
  if (document.trimmed().isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmsCapabilities",
                                            "The capabilities document is "
                                            "empty."));
  }
  QXmlStreamReader reader(document);
  if (!reader.readNextStartElement()) {
    return fail(errorString,
                reader.hasError()
                    ? QCoreApplication::translate(
                          "ts::WmsCapabilities",
                          "The capabilities are not valid XML: %1 (line %2).")
                          .arg(reader.errorString())
                          .arg(reader.lineNumber())
                    : QCoreApplication::translate(
                          "ts::WmsCapabilities",
                          "The capabilities document is empty."));
  }
  const QString root = reader.name().toString();
  if (root == QLatin1String("ServiceExceptionReport") ||
      root == QLatin1String("ExceptionReport")) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmsCapabilities",
                                            "The server reported an error: %1")
                    .arg(serviceExceptionText(reader)));
  }
  if (root.compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsCapabilities",
                    "The server sent a web page, not WMS capabilities. Check "
                    "the address."));
  }
  if (root != QLatin1String("WMS_Capabilities") &&
      root != QLatin1String("WMT_MS_Capabilities")) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsCapabilities",
                    "This is not a WMS capabilities document (it starts with "
                    "<%1>).")
                    .arg(root));
  }
  WmsCapabilities capabilities;
  capabilities.version = attribute(reader, QLatin1String("version")).trimmed();
  std::vector<RawLayer> rawLayers;
  while (reader.readNextStartElement()) {
    if (reader.name() == QLatin1String("Service")) {
      parseService(reader, capabilities);
    } else if (reader.name() == QLatin1String("Capability")) {
      parseCapability(reader, capabilities, rawLayers);
    } else {
      reader.skipCurrentElement();
    }
  }
  if (reader.hasError()) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsCapabilities",
                    "The capabilities are not valid XML: %1 (line %2).")
                    .arg(reader.errorString())
                    .arg(reader.lineNumber()));
  }
  // WMS 1.1.x writes EPSG:4326 boxes longitude first, 1.3.0 latitude first.
  const bool lonLatAxes =
      !capabilities.version.startsWith(QLatin1String("1.3"));
  for (const RawLayer& raw : rawLayers) {
    capabilities.layers.push_back(resolve(raw, nullptr, lonLatAxes));
  }
  if (capabilities.getMapUrl.isEmpty()) {
    return fail(errorString,
                QCoreApplication::translate(
                    "ts::WmsCapabilities",
                    "The capabilities don't say where to send map requests."));
  }
  if (capabilities.namedLayers().empty()) {
    return fail(errorString,
                QCoreApplication::translate("ts::WmsCapabilities",
                                            "The server offers no layers."));
  }
  return capabilities;
}

} // namespace ts
