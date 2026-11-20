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
#include "ts/kml/kmlreader.h"

#include <cstdint>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cpl_string.h>
#include <cpl_vsi.h>

#include "ts/time/timeextent.h"

namespace ts
{

namespace
{

bool isBool(const QString& text)
{
  return text == QLatin1String("1") ||
         text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

double number(const QString& text, double fallback)
{
  bool ok = false;
  const double value = text.trimmed().toDouble(&ok);
  return ok ? value : fallback;
}

KmlAltitudeMode altitudeMode(const QString& text)
{
  const QString t = text.trimmed();
  if (t == QLatin1String("relativeToGround")) {
    return KmlAltitudeMode::RelativeToGround;
  }
  if (t == QLatin1String("absolute")) {
    return KmlAltitudeMode::Absolute;
  }
  if (t == QLatin1String("clampToSeaFloor")) {
    return KmlAltitudeMode::ClampToSeaFloor;
  }
  if (t == QLatin1String("relativeToSeaFloor")) {
    return KmlAltitudeMode::RelativeToSeaFloor;
  }
  return KmlAltitudeMode::ClampToGround;
}

/** Reads a coordinates element's tuples ("lon,lat[,alt]" by whitespace). */
std::vector<KmlCoordinate> coordinates(QString text)
{
  // Some writers put spaces after the commas.
  text.replace(QRegularExpression(QStringLiteral("\\s*,\\s*")),
               QStringLiteral(","));
  std::vector<KmlCoordinate> list;
  const QStringList tuples = text.split(
      QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  for (const QString& tuple : tuples) {
    const QStringList values = tuple.split(QLatin1Char(','));
    bool okLon = false;
    bool okLat = false;
    KmlCoordinate c;
    c.lonDeg = values.value(0).toDouble(&okLon);
    c.latDeg = values.value(1).toDouble(&okLat);
    c.altM = values.size() > 2 ? values.at(2).toDouble() : 0.0;
    if (okLon && okLat) {
      list.push_back(c);
    }
  }
  return list;
}

KmlVec2::Units units(const QString& text)
{
  if (text == QLatin1String("pixels")) {
    return KmlVec2::Units::Pixels;
  }
  if (text == QLatin1String("insetPixels")) {
    return KmlVec2::Units::InsetPixels;
  }
  return KmlVec2::Units::Fraction;
}

std::optional<QDateTime> time(const QString& text)
{
  return parseIsoTime(text.trimmed(), QDateTime::currentDateTimeUtc());
}

/** Reads one document from a stream of XML. */
class Reader
{
public:
  Reader(QXmlStreamReader& xml, KmlDocument& document)
      : m_xml(xml), m_document(document)
  {
  }

  /** Reads the feature whose start element the reader is at. */
  KmlFeature feature(KmlFeature::Kind kind)
  {
    KmlFeature f;
    f.kind = kind;
    f.id = m_xml.attributes().value(QLatin1String("id")).toString();
    QString last;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (common(f, n) || (isContainer(kind) && container(f, n)) ||
          (kind == KmlFeature::Kind::Placemark && placemark(f, n)) ||
          (isOverlay(kind) && overlay(f, n)) ||
          (kind == KmlFeature::Kind::NetworkLink && networkLink(f, n))) {
        last = n;
        continue;
      }
      f.unknown.emplace_back(last, keep(n));
    }
    return f;
  }

  /** Returns the current element as XML, and notes it as skipped. */
  QString keep(const QString& n)
  {
    note(n);
    QString captured;
    QXmlStreamWriter writer(&captured);
    int32_t depth = 0;
    do {
      if (m_xml.isStartElement()) {
        ++depth;
      } else if (m_xml.isEndElement()) {
        --depth;
      }
      writer.writeCurrentToken(m_xml);
      if (depth == 0) {
        break;
      }
    } while (!m_xml.atEnd() && (m_xml.readNext(), true));
    return captured;
  }

private:
  static bool isContainer(KmlFeature::Kind kind)
  {
    return kind == KmlFeature::Kind::Document ||
           kind == KmlFeature::Kind::Folder;
  }

  static bool isOverlay(KmlFeature::Kind kind)
  {
    return kind == KmlFeature::Kind::GroundOverlay ||
           kind == KmlFeature::Kind::ScreenOverlay;
  }

  void note(const QString& n)
  {
    m_document.skipped.append(
        QCoreApplication::translate("ts::KmlReader", "%1 (line %2, column %3)")
            .arg(n)
            .arg(m_xml.lineNumber())
            .arg(m_xml.columnNumber()));
  }

  QString text()
  {
    return m_xml.readElementText(QXmlStreamReader::IncludeChildElements);
  }

  bool common(KmlFeature& f, const QString& n)
  {
    if (n == QLatin1String("name")) {
      f.name = text().trimmed();
    } else if (n == QLatin1String("visibility")) {
      f.visibility = isBool(text().trimmed());
    } else if (n == QLatin1String("open")) {
      f.open = isBool(text().trimmed());
    } else if (n == QLatin1String("description")) {
      f.description = text().trimmed();
    } else if (n == QLatin1String("Snippet") || n == QLatin1String("snippet")) {
      f.snippet = text().trimmed();
    } else if (n == QLatin1String("styleUrl")) {
      f.styleUrl = text().trimmed();
    } else if (n == QLatin1String("Style") &&
               f.kind != KmlFeature::Kind::Document) {
      f.style = style();
    } else if (n == QLatin1String("StyleMap") &&
               f.kind != KmlFeature::Kind::Document) {
      f.styleMap = styleMap();
    } else if (n == QLatin1String("LookAt") || n == QLatin1String("Camera")) {
      f.view = view(n == QLatin1String("Camera") ? KmlView::Kind::Camera
                                                 : KmlView::Kind::LookAt);
    } else if (n == QLatin1String("Region")) {
      f.region = region();
    } else if (n == QLatin1String("TimeStamp")) {
      f.time = timeStamp();
    } else if (n == QLatin1String("TimeSpan")) {
      f.time = timeSpan();
    } else if (n == QLatin1String("ExtendedData")) {
      extendedData(f);
    } else {
      return false;
    }
    return true;
  }

  bool container(KmlFeature& f, const QString& n)
  {
    if (n == QLatin1String("Style") && f.kind == KmlFeature::Kind::Document) {
      f.styles.push_back(style());
    } else if (n == QLatin1String("StyleMap") &&
               f.kind == KmlFeature::Kind::Document) {
      f.styleMaps.push_back(styleMap());
    } else if (n == QLatin1String("Document")) {
      f.children.push_back(feature(KmlFeature::Kind::Document));
    } else if (n == QLatin1String("Folder")) {
      f.children.push_back(feature(KmlFeature::Kind::Folder));
    } else if (n == QLatin1String("Placemark")) {
      f.children.push_back(feature(KmlFeature::Kind::Placemark));
    } else if (n == QLatin1String("GroundOverlay")) {
      f.children.push_back(feature(KmlFeature::Kind::GroundOverlay));
    } else if (n == QLatin1String("ScreenOverlay")) {
      f.children.push_back(feature(KmlFeature::Kind::ScreenOverlay));
    } else if (n == QLatin1String("NetworkLink")) {
      f.children.push_back(feature(KmlFeature::Kind::NetworkLink));
    } else {
      return false;
    }
    return true;
  }

  bool placemark(KmlFeature& f, const QString& n)
  {
    std::optional<KmlGeometry> read = geometry(n);
    if (!read) {
      return false;
    }
    f.geometry = std::move(read);
    return true;
  }

  bool overlay(KmlFeature& f, const QString& n)
  {
    if (n == QLatin1String("Icon")) {
      f.iconHref = link().href;
    } else if (n == QLatin1String("color")) {
      f.color = kmlColor(text());
    } else if (n == QLatin1String("drawOrder")) {
      f.drawOrder = text().trimmed().toInt();
    } else if (n == QLatin1String("altitude")) {
      f.altitudeM = number(text(), 0.0);
    } else if (n == QLatin1String("altitudeMode")) {
      f.altitudeMode = altitudeMode(text());
    } else if (n == QLatin1String("LatLonBox")) {
      f.latLonBox = latLonBox(f.latLonBox.value_or(KmlLatLonBox{}));
    } else if (n == QLatin1String("LatLonQuad")) {
      KmlLatLonBox box = f.latLonBox.value_or(KmlLatLonBox{});
      while (m_xml.readNextStartElement()) {
        if (m_xml.name() == QLatin1String("coordinates")) {
          box.quad = coordinates(text());
        } else {
          m_xml.skipCurrentElement();
        }
      }
      f.latLonBox = box;
    } else if (f.kind == KmlFeature::Kind::ScreenOverlay) {
      return screen(f, n);
    } else {
      return false;
    }
    return true;
  }

  bool screen(KmlFeature& f, const QString& n)
  {
    KmlScreenPlacement placement = f.screen.value_or(KmlScreenPlacement{});
    if (n == QLatin1String("overlayXY")) {
      placement.overlayXY = vec2();
    } else if (n == QLatin1String("screenXY")) {
      placement.screenXY = vec2();
    } else if (n == QLatin1String("rotationXY")) {
      placement.rotationXY = vec2();
    } else if (n == QLatin1String("size")) {
      placement.size = vec2();
    } else if (n == QLatin1String("rotation")) {
      placement.rotationDeg = number(text(), 0.0);
    } else {
      return false;
    }
    f.screen = placement;
    return true;
  }

  bool networkLink(KmlFeature& f, const QString& n)
  {
    if (n == QLatin1String("Link") || n == QLatin1String("Url")) {
      f.link = link();
    } else if (n == QLatin1String("flyToView")) {
      f.flyToView = isBool(text().trimmed());
    } else {
      return false;
    }
    return true;
  }

  std::optional<KmlGeometry> geometry(const QString& n)
  {
    KmlGeometry g;
    if (n == QLatin1String("Point")) {
      g.kind = KmlGeometry::Kind::Point;
    } else if (n == QLatin1String("LineString")) {
      g.kind = KmlGeometry::Kind::LineString;
    } else if (n == QLatin1String("LinearRing")) {
      g.kind = KmlGeometry::Kind::LinearRing;
    } else if (n == QLatin1String("Polygon")) {
      g.kind = KmlGeometry::Kind::Polygon;
    } else if (n == QLatin1String("MultiGeometry") ||
               n == QLatin1String("MultiTrack")) {
      g.kind = KmlGeometry::Kind::MultiGeometry;
    } else if (n == QLatin1String("Track")) {
      g.kind = KmlGeometry::Kind::Track;
    } else {
      return std::nullopt;
    }
    while (m_xml.readNextStartElement()) {
      geometryPart(g, m_xml.name().toString());
    }
    return g;
  }

  void geometryPart(KmlGeometry& g, const QString& n)
  {
    if (n == QLatin1String("coordinates")) {
      g.coordinates = coordinates(text());
    } else if (n == QLatin1String("altitudeMode")) {
      g.altitudeMode = altitudeMode(text());
    } else if (n == QLatin1String("extrude")) {
      g.extrude = isBool(text().trimmed());
    } else if (n == QLatin1String("tessellate")) {
      g.tessellate = isBool(text().trimmed());
    } else if (n == QLatin1String("outerBoundaryIs") ||
               n == QLatin1String("innerBoundaryIs")) {
      std::vector<KmlCoordinate> ring;
      while (m_xml.readNextStartElement()) {
        const std::optional<KmlGeometry> part =
            geometry(m_xml.name().toString());
        if (part) {
          ring = part->coordinates;
        } else {
          m_xml.skipCurrentElement();
        }
      }
      if (n == QLatin1String("outerBoundaryIs")) {
        g.coordinates = ring;
      } else {
        g.inner.push_back(ring);
      }
    } else if (n == QLatin1String("when")) {
      g.when.push_back(time(text()).value_or(QDateTime()));
    } else if (n == QLatin1String("coord")) {
      const QStringList values = text().split(
          QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
      g.coordinates.push_back(KmlCoordinate{number(values.value(0), 0.0),
                                            number(values.value(1), 0.0),
                                            number(values.value(2), 0.0)});
    } else if (g.kind == KmlGeometry::Kind::MultiGeometry) {
      std::optional<KmlGeometry> member = geometry(n);
      if (member) {
        member->altitudeMode = member->altitudeMode;
        g.children.push_back(std::move(*member));
      } else {
        note(n);
        m_xml.skipCurrentElement();
      }
    } else {
      note(n);
      m_xml.skipCurrentElement();
    }
  }

  KmlStyle style()
  {
    KmlStyle s;
    s.id = m_xml.attributes().value(QLatin1String("id")).toString();
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n == QLatin1String("IconStyle")) {
        s.icon = iconStyle();
      } else if (n == QLatin1String("LabelStyle")) {
        KmlLabelStyle label;
        while (m_xml.readNextStartElement()) {
          colourOrScale(m_xml.name(), &label.color, &label.scale);
        }
        s.label = label;
      } else if (n == QLatin1String("LineStyle")) {
        KmlLineStyle line;
        while (m_xml.readNextStartElement()) {
          if (m_xml.name() == QLatin1String("width")) {
            line.width = number(text(), 1.0);
          } else {
            colourOrScale(m_xml.name(), &line.color, nullptr);
          }
        }
        s.line = line;
      } else if (n == QLatin1String("PolyStyle")) {
        s.poly = polyStyle();
      } else if (n == QLatin1String("BalloonStyle")) {
        s.balloon = balloonStyle();
      } else if (n == QLatin1String("ListStyle")) {
        s.list = listStyle();
      } else {
        note(n);
        m_xml.skipCurrentElement();
      }
    }
    return s;
  }

  /** Reads color or scale into @p colour and @p scale; skips others. */
  void colourOrScale(QStringView name, QColor* colour, double* scale)
  {
    if (name == QLatin1String("color")) {
      const QColor read = kmlColor(text());
      if (read.isValid()) {
        *colour = read;
      }
    } else if (name == QLatin1String("scale") && scale != nullptr) {
      *scale = number(text(), 1.0);
    } else {
      m_xml.skipCurrentElement();
    }
  }

  KmlIconStyle iconStyle()
  {
    KmlIconStyle icon;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n == QLatin1String("Icon")) {
        icon.href = link().href;
      } else if (n == QLatin1String("heading")) {
        icon.heading = number(text(), 0.0);
      } else if (n == QLatin1String("hotSpot")) {
        icon.hotSpot = vec2();
      } else {
        colourOrScale(m_xml.name(), &icon.color, &icon.scale);
      }
    }
    return icon;
  }

  KmlPolyStyle polyStyle()
  {
    KmlPolyStyle poly;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n == QLatin1String("fill")) {
        poly.fill = isBool(text().trimmed());
      } else if (n == QLatin1String("outline")) {
        poly.outline = isBool(text().trimmed());
      } else {
        colourOrScale(m_xml.name(), &poly.color, nullptr);
      }
    }
    return poly;
  }

  KmlBalloonStyle balloonStyle()
  {
    KmlBalloonStyle balloon;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n == QLatin1String("bgColor") || n == QLatin1String("color")) {
        balloon.backgroundColor = kmlColor(text());
      } else if (n == QLatin1String("textColor")) {
        balloon.textColor = kmlColor(text());
      } else if (n == QLatin1String("text")) {
        balloon.text = text().trimmed();
      } else if (n == QLatin1String("displayMode")) {
        balloon.display = text().trimmed() != QLatin1String("hide");
      } else {
        m_xml.skipCurrentElement();
      }
    }
    return balloon;
  }

  KmlListStyle listStyle()
  {
    KmlListStyle list;
    while (m_xml.readNextStartElement()) {
      if (m_xml.name() == QLatin1String("listItemType")) {
        const QString t = text().trimmed();
        if (t == QLatin1String("radioFolder")) {
          list.itemType = KmlListStyle::ItemType::RadioFolder;
        } else if (t == QLatin1String("checkOffOnly")) {
          list.itemType = KmlListStyle::ItemType::CheckOffOnly;
        } else if (t == QLatin1String("checkHideChildren")) {
          list.itemType = KmlListStyle::ItemType::CheckHideChildren;
        }
      } else {
        m_xml.skipCurrentElement();
      }
    }
    return list;
  }

  KmlStyleMap styleMap()
  {
    KmlStyleMap map;
    map.id = m_xml.attributes().value(QLatin1String("id")).toString();
    while (m_xml.readNextStartElement()) {
      if (m_xml.name() != QLatin1String("Pair")) {
        m_xml.skipCurrentElement();
        continue;
      }
      QString key;
      QString url;
      std::optional<KmlStyle> inline_;
      while (m_xml.readNextStartElement()) {
        const QString n = m_xml.name().toString();
        if (n == QLatin1String("key")) {
          key = text().trimmed();
        } else if (n == QLatin1String("styleUrl")) {
          url = text().trimmed();
        } else if (n == QLatin1String("Style")) {
          inline_ = style();
        } else {
          m_xml.skipCurrentElement();
        }
      }
      if (key == QLatin1String("highlight")) {
        map.highlightUrl = url;
        map.highlight = inline_;
      } else {
        map.normalUrl = url;
        map.normal = inline_;
      }
    }
    return map;
  }

  KmlView view(KmlView::Kind kind)
  {
    KmlView v;
    v.kind = kind;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n == QLatin1String("longitude")) {
        v.lonDeg = number(text(), 0.0);
      } else if (n == QLatin1String("latitude")) {
        v.latDeg = number(text(), 0.0);
      } else if (n == QLatin1String("altitude")) {
        v.altM = number(text(), 0.0);
      } else if (n == QLatin1String("heading")) {
        v.headingDeg = number(text(), 0.0);
      } else if (n == QLatin1String("tilt")) {
        v.tiltDeg = number(text(), 0.0);
      } else if (n == QLatin1String("range")) {
        v.rangeM = number(text(), 1000.0);
      } else if (n == QLatin1String("roll")) {
        v.rollDeg = number(text(), 0.0);
      } else if (n == QLatin1String("altitudeMode")) {
        v.altitudeMode = altitudeMode(text());
      } else {
        m_xml.skipCurrentElement();
      }
    }
    return v;
  }

  KmlRegion region()
  {
    KmlRegion r;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n != QLatin1String("LatLonAltBox") && n != QLatin1String("Lod")) {
        m_xml.skipCurrentElement();
        continue;
      }
      while (m_xml.readNextStartElement()) {
        const QString p = m_xml.name().toString();
        const double value = number(text(), 0.0);
        if (p == QLatin1String("north")) {
          r.north = value;
        } else if (p == QLatin1String("south")) {
          r.south = value;
        } else if (p == QLatin1String("east")) {
          r.east = value;
        } else if (p == QLatin1String("west")) {
          r.west = value;
        } else if (p == QLatin1String("minAltitude")) {
          r.minAltM = value;
        } else if (p == QLatin1String("maxAltitude")) {
          r.maxAltM = value;
        } else if (p == QLatin1String("minLodPixels")) {
          r.minLodPixels = value;
        } else if (p == QLatin1String("maxLodPixels")) {
          r.maxLodPixels = value;
        }
      }
    }
    return r;
  }

  KmlLatLonBox latLonBox(KmlLatLonBox box)
  {
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      const double value = number(text(), 0.0);
      if (n == QLatin1String("north")) {
        box.north = value;
      } else if (n == QLatin1String("south")) {
        box.south = value;
      } else if (n == QLatin1String("east")) {
        box.east = value;
      } else if (n == QLatin1String("west")) {
        box.west = value;
      } else if (n == QLatin1String("rotation")) {
        box.rotationDeg = value;
      }
    }
    return box;
  }

  KmlTime timeStamp()
  {
    KmlTime t;
    while (m_xml.readNextStartElement()) {
      if (m_xml.name() == QLatin1String("when")) {
        t.begin = time(text());
        t.end = t.begin;
      } else {
        m_xml.skipCurrentElement();
      }
    }
    return t;
  }

  KmlTime timeSpan()
  {
    KmlTime t;
    while (m_xml.readNextStartElement()) {
      if (m_xml.name() == QLatin1String("begin")) {
        t.begin = time(text());
      } else if (m_xml.name() == QLatin1String("end")) {
        t.end = time(text());
      } else {
        m_xml.skipCurrentElement();
      }
    }
    return t;
  }

  void extendedData(KmlFeature& f)
  {
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      if (n == QLatin1String("Data")) {
        const QString name =
            m_xml.attributes().value(QLatin1String("name")).toString();
        QString value;
        QString display;
        while (m_xml.readNextStartElement()) {
          if (m_xml.name() == QLatin1String("value")) {
            value = text().trimmed();
          } else if (m_xml.name() == QLatin1String("displayName")) {
            display = text().trimmed();
          } else {
            m_xml.skipCurrentElement();
          }
        }
        f.extendedData.emplace_back(display.isEmpty() ? name : display, value);
      } else if (n == QLatin1String("SchemaData")) {
        while (m_xml.readNextStartElement()) {
          if (m_xml.name() == QLatin1String("SimpleData")) {
            const QString name =
                m_xml.attributes().value(QLatin1String("name")).toString();
            f.extendedData.emplace_back(name, text().trimmed());
          } else {
            m_xml.skipCurrentElement();
          }
        }
      } else {
        m_xml.skipCurrentElement();
      }
    }
  }

  KmlLink link()
  {
    KmlLink l;
    while (m_xml.readNextStartElement()) {
      const QString n = m_xml.name().toString();
      const QString t = text().trimmed();
      if (n == QLatin1String("href")) {
        l.href = t;
      } else if (n == QLatin1String("refreshMode")) {
        l.refreshMode =
            t == QLatin1String("onInterval") ? KmlLink::RefreshMode::OnInterval
            : t == QLatin1String("onExpire") ? KmlLink::RefreshMode::OnExpire
                                             : KmlLink::RefreshMode::OnChange;
      } else if (n == QLatin1String("refreshInterval")) {
        l.refreshIntervalS = number(t, 4.0);
      } else if (n == QLatin1String("viewRefreshMode")) {
        l.viewRefreshMode = t == QLatin1String("onStop")
                                ? KmlLink::ViewRefreshMode::OnStop
                            : t == QLatin1String("onRequest")
                                ? KmlLink::ViewRefreshMode::OnRequest
                            : t == QLatin1String("onRegion")
                                ? KmlLink::ViewRefreshMode::OnRegion
                                : KmlLink::ViewRefreshMode::Never;
      } else if (n == QLatin1String("viewRefreshTime")) {
        l.viewRefreshTimeS = number(t, 4.0);
      } else if (n == QLatin1String("viewBoundScale")) {
        l.viewBoundScale = number(t, 1.0);
      } else if (n == QLatin1String("viewFormat")) {
        l.viewFormat = t;
      } else if (n == QLatin1String("httpQuery")) {
        l.httpQuery = t;
      }
    }
    return l;
  }

  KmlVec2 vec2()
  {
    KmlVec2 v;
    const QXmlStreamAttributes a = m_xml.attributes();
    v.x = number(a.value(QLatin1String("x")).toString(), 0.5);
    v.y = number(a.value(QLatin1String("y")).toString(), 0.5);
    v.xUnits = units(a.value(QLatin1String("xunits")).toString());
    v.yUnits = units(a.value(QLatin1String("yunits")).toString());
    m_xml.skipCurrentElement();
    return v;
  }

  QXmlStreamReader& m_xml;
  KmlDocument& m_document;
};

std::optional<KmlFeature::Kind> featureKind(const QString& n)
{
  if (n == QLatin1String("Document")) {
    return KmlFeature::Kind::Document;
  }
  if (n == QLatin1String("Folder")) {
    return KmlFeature::Kind::Folder;
  }
  if (n == QLatin1String("Placemark")) {
    return KmlFeature::Kind::Placemark;
  }
  if (n == QLatin1String("GroundOverlay")) {
    return KmlFeature::Kind::GroundOverlay;
  }
  if (n == QLatin1String("ScreenOverlay")) {
    return KmlFeature::Kind::ScreenOverlay;
  }
  if (n == QLatin1String("NetworkLink")) {
    return KmlFeature::Kind::NetworkLink;
  }
  return std::nullopt;
}

std::optional<KmlDocument> fail(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return std::nullopt;
}

/** Reads a whole file through GDAL's virtual file system. */
QByteArray readVsi(const QString& path)
{
  const QByteArray name = path.toUtf8();
  VSILFILE* file = VSIFOpenL(name.constData(), "rb");
  if (file == nullptr) {
    return QByteArray();
  }
  QByteArray data;
  char buffer[65536];
  size_t read = 0;
  while ((read = VSIFReadL(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, static_cast<qsizetype>(read));
  }
  VSIFCloseL(file);
  return data;
}

/** The KML file inside a KMZ: doc.kml, else the first .kml at the top. */
QString kmzMain(const QString& archive)
{
  const QByteArray name = archive.toUtf8();
  char** entries = VSIReadDir(name.constData());
  QString first;
  bool doc = false;
  for (char** e = entries; e != nullptr && *e != nullptr; ++e) {
    const QString entry = QString::fromUtf8(*e);
    if (entry.compare(QLatin1String("doc.kml"), Qt::CaseInsensitive) == 0) {
      first = entry;
      doc = true;
    } else if (!doc && first.isEmpty() &&
               entry.endsWith(QLatin1String(".kml"), Qt::CaseInsensitive)) {
      first = entry;
    }
  }
  CSLDestroy(entries);
  return first;
}

} // namespace

std::optional<KmlDocument> parseKml(const QByteArray& xml, const QString& base,
                                    QString* errorString)
{
  QXmlStreamReader stream(xml);
  KmlDocument document;
  document.base = base;
  Reader reader(stream, document);
  if (!stream.readNextStartElement()) {
    return fail(errorString,
                QCoreApplication::translate("ts::KmlReader",
                                            "The file is not KML: %1 (line %2, "
                                            "column %3).")
                    .arg(stream.errorString())
                    .arg(stream.lineNumber())
                    .arg(stream.columnNumber()));
  }
  const QString root = stream.name().toString();
  bool found = false;
  if (root == QLatin1String("kml")) {
    while (stream.readNextStartElement()) {
      const QString n = stream.name().toString();
      const std::optional<KmlFeature::Kind> kind = featureKind(n);
      if (kind && !found) {
        document.root = reader.feature(*kind);
        found = true;
      } else {
        document.unknownTopLevel.push_back(reader.keep(n));
      }
    }
  } else if (const std::optional<KmlFeature::Kind> kind = featureKind(root)) {
    document.root = reader.feature(*kind);
    found = true;
  } else {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::KmlReader",
                                 "The file is not KML: it starts with <%1>.")
                                 .arg(root));
  }
  if (stream.hasError()) {
    return fail(errorString, QCoreApplication::translate(
                                 "ts::KmlReader",
                                 "The KML is damaged: %1 (line %2, column %3).")
                                 .arg(stream.errorString())
                                 .arg(stream.lineNumber())
                                 .arg(stream.columnNumber()));
  }
  if (!found) {
    document.root.kind = KmlFeature::Kind::Document;
  }
  return document;
}

std::optional<KmlDocument> readKmlFile(const QString& path,
                                       QString* errorString)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return fail(
        errorString,
        QCoreApplication::translate("ts::KmlReader", "Cannot open \"%1\": %2")
            .arg(QDir::toNativeSeparators(path), file.errorString()));
  }
  const QByteArray head = file.peek(4);
  file.close();
  if (head.startsWith("PK")) {
    const QString archive = QStringLiteral("/vsizip/") + path;
    const QString main = kmzMain(archive);
    if (main.isEmpty()) {
      return fail(errorString,
                  QCoreApplication::translate(
                      "ts::KmlReader",
                      "\"%1\" is a KMZ archive without a KML file in it.")
                      .arg(QDir::toNativeSeparators(path)));
    }
    std::optional<KmlDocument> document = parseKml(
        readVsi(archive + QLatin1Char('/') + main), archive, errorString);
    if (document) {
      document->path = path;
      document->kmz = true;
    }
    return document;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    return fail(errorString, file.errorString());
  }
  std::optional<KmlDocument> document =
      parseKml(file.readAll(), QFileInfo(path).absolutePath(), errorString);
  if (document) {
    document->path = path;
  }
  return document;
}

QString resolveKmlHref(const KmlDocument& document, const QString& href)
{
  const QString trimmed = href.trimmed();
  if (trimmed.isEmpty()) {
    return QString();
  }
  const QUrl url(trimmed);
  if (url.scheme() == QLatin1String("http") ||
      url.scheme() == QLatin1String("https")) {
    return trimmed;
  }
  if (url.scheme() == QLatin1String("file")) {
    return url.toLocalFile();
  }
  if (!document.webBase.isEmpty() && !document.kmz) {
    return QUrl(document.webBase).resolved(QUrl(trimmed)).toString();
  }
  if (QDir::isAbsolutePath(trimmed) || document.base.isEmpty()) {
    return trimmed;
  }
  return document.base + QLatin1Char('/') + trimmed;
}

QByteArray readKmlResource(const KmlDocument& document, const QString& href,
                           QString* errorString)
{
  const QString where = resolveKmlHref(document, href);
  if (where.startsWith(QLatin1String("http://")) ||
      where.startsWith(QLatin1String("https://")) || where.isEmpty()) {
    if (errorString != nullptr) {
      *errorString = QCoreApplication::translate("ts::KmlReader",
                                                 "\"%1\" is not a local file.")
                         .arg(href);
    }
    return QByteArray();
  }
  const QByteArray data = readVsi(where);
  if (data.isEmpty() && errorString != nullptr) {
    *errorString =
        QCoreApplication::translate("ts::KmlReader", "Cannot read \"%1\".")
            .arg(href);
  }
  return data;
}

QByteArray readKmlLocalFile(const QString& where)
{
  return readVsi(where);
}

KmlStyleResolver::KmlStyleResolver(const KmlDocument& document)
    : m_document(document)
{
}

std::optional<KmlStyle> KmlStyleResolver::styleAt(const QString& url,
                                                  bool highlight)
{
  const int32_t hash = static_cast<int32_t>(url.indexOf(QLatin1Char('#')));
  if (hash < 0) {
    return std::nullopt;
  }
  const QString file = url.left(hash);
  const QString id = url.mid(hash + 1);
  const KmlDocument* document = &m_document;
  if (!file.isEmpty()) {
    std::shared_ptr<const KmlDocument>& external = m_external[file];
    if (!external) {
      const QString where = resolveKmlHref(m_document, file);
      QString error;
      std::optional<KmlDocument> read =
          where.startsWith(QLatin1String("/vsizip/"))
              ? parseKml(readVsi(where), QFileInfo(where).path(), &error)
              : readKmlFile(where, &error);
      external =
          std::make_shared<const KmlDocument>(read.value_or(KmlDocument{}));
    }
    document = external.get();
  }
  if (const KmlStyle* style = document->findStyle(id)) {
    return *style;
  }
  if (const KmlStyleMap* map = document->findStyleMap(id)) {
    const std::optional<KmlStyle>& inline_ =
        highlight ? map->highlight : map->normal;
    if (inline_) {
      return inline_;
    }
    const QString& target = highlight ? map->highlightUrl : map->normalUrl;
    if (!target.isEmpty() && target != url) {
      // A style map's url is relative to its own document.
      const QString full =
          target.startsWith(QLatin1Char('#')) ? file + target : target;
      return styleAt(full, false);
    }
  }
  return std::nullopt;
}

KmlStyle KmlStyleResolver::resolve(const KmlFeature& feature, bool highlight)
{
  KmlStyle style;
  if (!feature.styleUrl.isEmpty()) {
    style = styleAt(feature.styleUrl, highlight).value_or(KmlStyle{});
  }
  if (feature.styleMap) {
    const std::optional<KmlStyle>& inline_ =
        highlight ? feature.styleMap->highlight : feature.styleMap->normal;
    const QString& url = highlight ? feature.styleMap->highlightUrl
                                   : feature.styleMap->normalUrl;
    if (inline_) {
      style = style.mergedWith(*inline_);
    } else if (!url.isEmpty()) {
      style = style.mergedWith(styleAt(url, false).value_or(KmlStyle{}));
    }
  }
  if (feature.style) {
    style = style.mergedWith(*feature.style);
  }
  return style;
}

} // namespace ts
