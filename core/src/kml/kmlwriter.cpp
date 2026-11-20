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
#include "ts/kml/kmlwriter.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUrl>

#include <cpl_conv.h>

#include "ts/kml/kmlreader.h"

namespace ts
{

namespace
{

const QString kmlNamespace = QStringLiteral("http://www.opengis.net/kml/2.2");
const QString gxNamespace = QStringLiteral("http://www.google.com/kml/ext/2.2");
const QString atomNamespace = QStringLiteral("http://www.w3.org/2005/Atom");

/** A rank for repeated or later elements: the children come last. */
constexpr int32_t childrenRank = 1000;

QString indent(int32_t depth)
{
  return QString(static_cast<qsizetype>(depth) * 2, QLatin1Char(' '));
}

QString newline(int32_t depth)
{
  return QLatin1Char('\n') + indent(depth);
}

QString escaped(const QString& text)
{
  return text.toHtmlEscaped();
}

/** Text as KML wants it: CDATA for HTML, escaped otherwise. */
QString textContent(const QString& text)
{
  if (!text.contains(QLatin1Char('<')) && !text.contains(QLatin1Char('&'))) {
    return escaped(text);
  }
  QString safe = text;
  safe.replace(QLatin1String("]]>"), QLatin1String("]]]]><![CDATA[>"));
  return QStringLiteral("<![CDATA[") + safe + QStringLiteral("]]>");
}

/** A number without needless digits or an exponent. */
QString number(double value)
{
  if (value == 0.0) {
    return QStringLiteral("0");
  }
  QString text = QString::number(value, 'f', 10);
  if (text.contains(QLatin1Char('.'))) {
    while (text.endsWith(QLatin1Char('0'))) {
      text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
      text.chop(1);
    }
  }
  return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

QString leaf(const QString& name, const QString& value)
{
  return QStringLiteral("<%1>%2</%1>").arg(name, value);
}

QString timeText(const QDateTime& time)
{
  return time.toUTC().toString(Qt::ISODate);
}

QString coordinatesText(const std::vector<KmlCoordinate>& coordinates)
{
  const bool heights =
      std::any_of(coordinates.begin(), coordinates.end(),
                  [](const KmlCoordinate& c) { return c.altM != 0.0; });
  QStringList parts;
  for (const KmlCoordinate& c : coordinates) {
    parts << (heights
                  ? QStringLiteral("%1,%2,%3")
                        .arg(number(c.lonDeg), number(c.latDeg), number(c.altM))
                  : QStringLiteral("%1,%2").arg(number(c.lonDeg),
                                                number(c.latDeg)));
  }
  return parts.join(QLatin1Char(' '));
}

QString unitsName(KmlVec2::Units units)
{
  switch (units) {
  case KmlVec2::Units::Pixels:
    return QStringLiteral("pixels");
  case KmlVec2::Units::InsetPixels:
    return QStringLiteral("insetPixels");
  case KmlVec2::Units::Fraction:
    break;
  }
  return QStringLiteral("fraction");
}

QString vec2(const QString& name, const KmlVec2& v)
{
  return QStringLiteral("<%1 x=\"%2\" y=\"%3\" xunits=\"%4\" yunits=\"%5\"/>")
      .arg(name, number(v.x), number(v.y), unitsName(v.xUnits),
           unitsName(v.yUnits));
}

bool isGxMode(KmlAltitudeMode mode)
{
  return mode == KmlAltitudeMode::ClampToSeaFloor ||
         mode == KmlAltitudeMode::RelativeToSeaFloor;
}

void append(QString& inner, int32_t d, const QString& element)
{
  if (!element.isEmpty()) {
    inner += newline(d + 1) + element;
  }
}

QString ring(const QString& boundary, const std::vector<KmlCoordinate>& c,
             int32_t d)
{
  return QStringLiteral("<%1>%2<LinearRing>%3<coordinates>%4</coordinates>%5"
                        "</LinearRing>%6</%1>")
      .arg(boundary, newline(d + 2), newline(d + 3), coordinatesText(c),
           newline(d + 2), newline(d + 1));
}

QString refreshModeName(KmlLink::RefreshMode mode)
{
  switch (mode) {
  case KmlLink::RefreshMode::OnInterval:
    return QStringLiteral("onInterval");
  case KmlLink::RefreshMode::OnExpire:
    return QStringLiteral("onExpire");
  case KmlLink::RefreshMode::OnChange:
    break;
  }
  return QString();
}

QString viewRefreshModeName(KmlLink::ViewRefreshMode mode)
{
  switch (mode) {
  case KmlLink::ViewRefreshMode::OnStop:
    return QStringLiteral("onStop");
  case KmlLink::ViewRefreshMode::OnRequest:
    return QStringLiteral("onRequest");
  case KmlLink::ViewRefreshMode::OnRegion:
    return QStringLiteral("onRegion");
  case KmlLink::ViewRefreshMode::Never:
    break;
  }
  return QString();
}

/** Writes elements, knowing whether gx: is declared where they go. */
class Builder
{
public:
  explicit Builder(bool gxDeclared) : m_gxDeclared(gxDeclared) {}

  /** A gx: element's start tag, declaring gx: if it isn't. */
  QString gxStart(const QString& name) const
  {
    return m_gxDeclared ? QStringLiteral("<gx:%1>").arg(name)
                        : QStringLiteral("<gx:%1 xmlns:gx=\"%2\">")
                              .arg(name, gxNamespace);
  }

  QString altitudeMode(KmlAltitudeMode mode) const
  {
    if (mode == KmlAltitudeMode::ClampToGround) {
      return QString();
    }
    if (isGxMode(mode)) {
      return gxStart(QStringLiteral("altitudeMode")) +
             kmlAltitudeModeName(mode) + QStringLiteral("</gx:altitudeMode>");
    }
    return leaf(QStringLiteral("altitudeMode"), kmlAltitudeModeName(mode));
  }

  QString geometryCommon(const KmlGeometry& g, int32_t d) const
  {
    QString inner;
    if (g.extrude) {
      append(inner, d, leaf(QStringLiteral("extrude"), QStringLiteral("1")));
    }
    if (g.tessellate && g.kind != KmlGeometry::Kind::Point) {
      append(inner, d, leaf(QStringLiteral("tessellate"), QStringLiteral("1")));
    }
    append(inner, d, altitudeMode(g.altitudeMode));
    return inner;
  }

  QString track(const KmlGeometry& g, int32_t d) const
  {
    // gx:Track: its altitude mode, the times, then the places.
    QString inner;
    append(inner, d, altitudeMode(g.altitudeMode));
    for (const QDateTime& when : g.when) {
      append(inner, d, leaf(QStringLiteral("when"), timeText(when)));
    }
    for (const KmlCoordinate& c : g.coordinates) {
      append(inner, d,
             QStringLiteral("<gx:coord>%1 %2 %3</gx:coord>")
                 .arg(number(c.lonDeg), number(c.latDeg), number(c.altM)));
    }
    return gxStart(QStringLiteral("Track")) + inner + newline(d) +
           QStringLiteral("</gx:Track>");
  }

  QString geometry(const KmlGeometry& g, int32_t d) const
  {
    QString inner;
    QString name;
    switch (g.kind) {
    case KmlGeometry::Kind::Point:
    case KmlGeometry::Kind::LineString:
    case KmlGeometry::Kind::LinearRing:
      name = g.kind == KmlGeometry::Kind::Point ? QStringLiteral("Point")
             : g.kind == KmlGeometry::Kind::LineString
                 ? QStringLiteral("LineString")
                 : QStringLiteral("LinearRing");
      inner = geometryCommon(g, d);
      append(
          inner, d,
          leaf(QStringLiteral("coordinates"), coordinatesText(g.coordinates)));
      break;
    case KmlGeometry::Kind::Polygon:
      name = QStringLiteral("Polygon");
      inner = geometryCommon(g, d);
      append(inner, d,
             ring(QStringLiteral("outerBoundaryIs"), g.coordinates, d));
      for (const std::vector<KmlCoordinate>& hole : g.inner) {
        append(inner, d, ring(QStringLiteral("innerBoundaryIs"), hole, d));
      }
      break;
    case KmlGeometry::Kind::MultiGeometry:
      name = QStringLiteral("MultiGeometry");
      for (const KmlGeometry& child : g.children) {
        append(inner, d, geometry(child, d + 1));
      }
      break;
    case KmlGeometry::Kind::Track:
      return track(g, d);
    }
    return QStringLiteral("<%1>%2%3</%1>").arg(name, inner, newline(d));
  }

  static QString subStyle(const QString& name, const QStringList& elements,
                          int32_t d)
  {
    QString inner;
    for (const QString& element : elements) {
      if (!element.isEmpty()) {
        inner += newline(d + 1) + element;
      }
    }
    return QStringLiteral("<%1>%2%3</%1>").arg(name, inner, newline(d));
  }

  QString style(const KmlStyle& s, int32_t d) const
  {
    QString inner;
    if (s.icon) {
      const KmlIconStyle& icon = *s.icon;
      append(inner, d,
             subStyle(
                 QStringLiteral("IconStyle"),
                 {icon.color != QColor(255, 255, 255)
                      ? leaf(QStringLiteral("color"), kmlColorText(icon.color))
                      : QString(),
                  icon.scale != 1.0
                      ? leaf(QStringLiteral("scale"), number(icon.scale))
                      : QString(),
                  icon.heading != 0.0
                      ? leaf(QStringLiteral("heading"), number(icon.heading))
                      : QString(),
                  icon.href.isEmpty()
                      ? QString()
                      : QStringLiteral("<Icon><href>%1</href></Icon>")
                            .arg(escaped(icon.href)),
                  vec2(QStringLiteral("hotSpot"), icon.hotSpot)},
                 d + 1));
    }
    if (s.label) {
      append(
          inner, d,
          subStyle(QStringLiteral("LabelStyle"),
                   {leaf(QStringLiteral("color"), kmlColorText(s.label->color)),
                    leaf(QStringLiteral("scale"), number(s.label->scale))},
                   d + 1));
    }
    if (s.line) {
      append(
          inner, d,
          subStyle(QStringLiteral("LineStyle"),
                   {leaf(QStringLiteral("color"), kmlColorText(s.line->color)),
                    leaf(QStringLiteral("width"), number(s.line->width))},
                   d + 1));
    }
    if (s.poly) {
      append(
          inner, d,
          subStyle(
              QStringLiteral("PolyStyle"),
              {leaf(QStringLiteral("color"), kmlColorText(s.poly->color)),
               leaf(QStringLiteral("fill"),
                    s.poly->fill ? QStringLiteral("1") : QStringLiteral("0")),
               leaf(QStringLiteral("outline"), s.poly->outline
                                                   ? QStringLiteral("1")
                                                   : QStringLiteral("0"))},
              d + 1));
    }
    if (s.balloon) {
      const KmlBalloonStyle& b = *s.balloon;
      append(
          inner, d,
          subStyle(
              QStringLiteral("BalloonStyle"),
              {leaf(QStringLiteral("bgColor"), kmlColorText(b.backgroundColor)),
               leaf(QStringLiteral("textColor"), kmlColorText(b.textColor)),
               b.text.isEmpty()
                   ? QString()
                   : leaf(QStringLiteral("text"), textContent(b.text)),
               b.display ? QString()
                         : leaf(QStringLiteral("displayMode"),
                                QStringLiteral("hide"))},
              d + 1));
    }
    if (s.list) {
      QString type = QStringLiteral("check");
      switch (s.list->itemType) {
      case KmlListStyle::ItemType::RadioFolder:
        type = QStringLiteral("radioFolder");
        break;
      case KmlListStyle::ItemType::CheckOffOnly:
        type = QStringLiteral("checkOffOnly");
        break;
      case KmlListStyle::ItemType::CheckHideChildren:
        type = QStringLiteral("checkHideChildren");
        break;
      case KmlListStyle::ItemType::Check:
        break;
      }
      append(inner, d,
             subStyle(QStringLiteral("ListStyle"),
                      {leaf(QStringLiteral("listItemType"), type)}, d + 1));
    }
    const QString id = s.id.isEmpty()
                           ? QString()
                           : QStringLiteral(" id=\"%1\"").arg(escaped(s.id));
    return QStringLiteral("<Style%1>%2%3</Style>").arg(id, inner, newline(d));
  }

  QString pair(const QString& key, const QString& url,
               const std::optional<KmlStyle>& inlineStyle, int32_t d) const
  {
    const QString value = inlineStyle
                              ? style(*inlineStyle, d + 1)
                              : leaf(QStringLiteral("styleUrl"), escaped(url));
    return QStringLiteral("<Pair>%1%2%3%4</Pair>")
        .arg(newline(d + 1), leaf(QStringLiteral("key"), key),
             newline(d + 1) + value, newline(d));
  }

  QString styleMap(const KmlStyleMap& m, int32_t d) const
  {
    const QString id = m.id.isEmpty()
                           ? QString()
                           : QStringLiteral(" id=\"%1\"").arg(escaped(m.id));
    return QStringLiteral("<StyleMap%1>%2%3%4</StyleMap>")
        .arg(id,
             newline(d + 1) +
                 pair(QStringLiteral("normal"), m.normalUrl, m.normal, d + 1),
             newline(d + 1) + pair(QStringLiteral("highlight"), m.highlightUrl,
                                   m.highlight, d + 1),
             newline(d));
  }

  QString view(const KmlView& v, int32_t d) const
  {
    const bool camera = v.kind == KmlView::Kind::Camera;
    QStringList parts{leaf(QStringLiteral("longitude"), number(v.lonDeg)),
                      leaf(QStringLiteral("latitude"), number(v.latDeg)),
                      leaf(QStringLiteral("altitude"), number(v.altM)),
                      leaf(QStringLiteral("heading"), number(v.headingDeg)),
                      leaf(QStringLiteral("tilt"), number(v.tiltDeg))};
    if (camera) {
      parts << leaf(QStringLiteral("roll"), number(v.rollDeg));
    } else {
      parts << leaf(QStringLiteral("range"), number(v.rangeM));
    }
    parts << altitudeMode(v.altitudeMode);
    return subStyle(
        camera ? QStringLiteral("Camera") : QStringLiteral("LookAt"), parts, d);
  }

  static QString time(const KmlTime& t, int32_t d)
  {
    if (t.begin && t.end && *t.begin == *t.end) {
      return subStyle(QStringLiteral("TimeStamp"),
                      {leaf(QStringLiteral("when"), timeText(*t.begin))}, d);
    }
    return subStyle(
        QStringLiteral("TimeSpan"),
        {t.begin ? leaf(QStringLiteral("begin"), timeText(*t.begin))
                 : QString(),
         t.end ? leaf(QStringLiteral("end"), timeText(*t.end)) : QString()},
        d);
  }

  static QString region(const KmlRegion& r, int32_t d)
  {
    const QString box =
        subStyle(QStringLiteral("LatLonAltBox"),
                 {leaf(QStringLiteral("north"), number(r.north)),
                  leaf(QStringLiteral("south"), number(r.south)),
                  leaf(QStringLiteral("east"), number(r.east)),
                  leaf(QStringLiteral("west"), number(r.west)),
                  r.minAltM != 0.0
                      ? leaf(QStringLiteral("minAltitude"), number(r.minAltM))
                      : QString(),
                  r.maxAltM != 0.0
                      ? leaf(QStringLiteral("maxAltitude"), number(r.maxAltM))
                      : QString()},
                 d + 1);
    const QString lod =
        subStyle(QStringLiteral("Lod"),
                 {leaf(QStringLiteral("minLodPixels"), number(r.minLodPixels)),
                  leaf(QStringLiteral("maxLodPixels"), number(r.maxLodPixels))},
                 d + 1);
    return subStyle(QStringLiteral("Region"), {box, lod}, d);
  }

  static QString extendedData(const KmlFeature& f, int32_t d)
  {
    QStringList data;
    for (const std::pair<QString, QString>& field : f.extendedData) {
      data << QStringLiteral("<Data name=\"%1\">%2</Data>")
                  .arg(escaped(field.first),
                       newline(d + 2) +
                           leaf(QStringLiteral("value"),
                                textContent(field.second)) +
                           newline(d + 1));
    }
    return subStyle(QStringLiteral("ExtendedData"), data, d);
  }

  static QString link(const KmlLink& l, int32_t d)
  {
    const QString mode = refreshModeName(l.refreshMode);
    const QString viewMode = viewRefreshModeName(l.viewRefreshMode);
    return subStyle(
        QStringLiteral("Link"),
        {leaf(QStringLiteral("href"), escaped(l.href)),
         mode.isEmpty() ? QString() : leaf(QStringLiteral("refreshMode"), mode),
         l.refreshMode == KmlLink::RefreshMode::OnInterval
             ? leaf(QStringLiteral("refreshInterval"),
                    number(l.refreshIntervalS))
             : QString(),
         viewMode.isEmpty() ? QString()
                            : leaf(QStringLiteral("viewRefreshMode"), viewMode),
         l.viewRefreshMode == KmlLink::ViewRefreshMode::OnStop
             ? leaf(QStringLiteral("viewRefreshTime"),
                    number(l.viewRefreshTimeS))
             : QString(),
         l.viewBoundScale != 1.0
             ? leaf(QStringLiteral("viewBoundScale"), number(l.viewBoundScale))
             : QString(),
         l.viewFormat.isEmpty()
             ? QString()
             : leaf(QStringLiteral("viewFormat"), escaped(l.viewFormat)),
         l.httpQuery.isEmpty()
             ? QString()
             : leaf(QStringLiteral("httpQuery"), escaped(l.httpQuery))},
        d);
  }

  /** The feature's element in @p slot (see kmlFeatureElement()). */
  QString element(const KmlFeature& f, const QString& slot, int32_t ordinal,
                  int32_t d) const;

private:
  QString styleElement(const KmlFeature& f, int32_t ordinal, int32_t d) const
  {
    const std::size_t at = static_cast<std::size_t>(ordinal);
    if (f.kind == KmlFeature::Kind::Document) {
      return at < f.styles.size() ? style(f.styles[at], d) : QString();
    }
    return ordinal == 0 && f.style ? style(*f.style, d) : QString();
  }

  QString styleMapElement(const KmlFeature& f, int32_t ordinal, int32_t d) const
  {
    const std::size_t at = static_cast<std::size_t>(ordinal);
    if (f.kind == KmlFeature::Kind::Document) {
      return at < f.styleMaps.size() ? styleMap(f.styleMaps[at], d) : QString();
    }
    return ordinal == 0 && f.styleMap ? styleMap(*f.styleMap, d) : QString();
  }

  QString nameElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.name.isEmpty() ? QString()
                            : leaf(QStringLiteral("name"), escaped(f.name));
  }

  QString visibilityElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.visibility
               ? QString()
               : leaf(QStringLiteral("visibility"), QStringLiteral("0"));
  }

  QString openElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.open ? leaf(QStringLiteral("open"), QStringLiteral("1"))
                  : QString();
  }

  QString snippetElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.snippet.isEmpty()
               ? QString()
               : leaf(QStringLiteral("Snippet"), escaped(f.snippet));
  }

  QString descriptionElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.description.isEmpty() ? QString()
                                   : leaf(QStringLiteral("description"),
                                          textContent(f.description));
  }

  QString viewElement(const KmlFeature& f, int32_t d) const
  {
    return f.view ? view(*f.view, d) : QString();
  }

  QString timeElement(const KmlFeature& f, int32_t d) const
  {
    return f.time ? time(*f.time, d) : QString();
  }

  QString styleUrlElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.styleUrl.isEmpty()
               ? QString()
               : leaf(QStringLiteral("styleUrl"), escaped(f.styleUrl));
  }

  QString regionElement(const KmlFeature& f, int32_t d) const
  {
    return f.region ? region(*f.region, d) : QString();
  }

  QString extendedDataElement(const KmlFeature& f, int32_t d) const
  {
    return f.extendedData.empty() ? QString() : extendedData(f, d);
  }

  QString geometryElement(const KmlFeature& f, int32_t d) const
  {
    return f.geometry ? geometry(*f.geometry, d) : QString();
  }

  QString colorElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.color == QColor(255, 255, 255)
               ? QString()
               : leaf(QStringLiteral("color"), kmlColorText(f.color));
  }

  QString drawOrderElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.drawOrder == 0 ? QString()
                            : leaf(QStringLiteral("drawOrder"),
                                   QString::number(f.drawOrder));
  }

  QString iconElement(const KmlFeature& f, int32_t d) const
  {
    return f.iconHref.isEmpty()
               ? QString()
               : QStringLiteral("<Icon>%1%2</Icon>")
                     .arg(newline(d + 1) +
                              leaf(QStringLiteral("href"), escaped(f.iconHref)),
                          newline(d));
  }

  QString altitudeElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.altitudeM == 0.0
               ? QString()
               : leaf(QStringLiteral("altitude"), number(f.altitudeM));
  }

  QString altitudeModeElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return altitudeMode(f.altitudeMode);
  }

  QString latLonBoxElement(const KmlFeature& f, int32_t d) const
  {
    if (!f.latLonBox || f.latLonBox->quad.size() == 4) {
      return QString();
    }
    const KmlLatLonBox& b = *f.latLonBox;
    return subStyle(QStringLiteral("LatLonBox"),
                    {leaf(QStringLiteral("north"), number(b.north)),
                     leaf(QStringLiteral("south"), number(b.south)),
                     leaf(QStringLiteral("east"), number(b.east)),
                     leaf(QStringLiteral("west"), number(b.west)),
                     b.rotationDeg != 0.0 ? leaf(QStringLiteral("rotation"),
                                                 number(b.rotationDeg))
                                          : QString()},
                    d);
  }

  QString latLonQuadElement(const KmlFeature& f, int32_t d) const
  {
    if (!f.latLonBox || f.latLonBox->quad.size() != 4) {
      return QString();
    }
    return gxStart(QStringLiteral("LatLonQuad")) + newline(d + 1) +
           leaf(QStringLiteral("coordinates"),
                coordinatesText(f.latLonBox->quad)) +
           newline(d) + QStringLiteral("</gx:LatLonQuad>");
  }

  QString overlayXYElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.screen ? vec2(QStringLiteral("overlayXY"), f.screen->overlayXY)
                    : QString();
  }

  QString screenXYElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.screen ? vec2(QStringLiteral("screenXY"), f.screen->screenXY)
                    : QString();
  }

  QString rotationXYElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.screen ? vec2(QStringLiteral("rotationXY"), f.screen->rotationXY)
                    : QString();
  }

  QString sizeElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.screen ? vec2(QStringLiteral("size"), f.screen->size) : QString();
  }

  QString rotationElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.screen && f.screen->rotationDeg != 0.0
               ? leaf(QStringLiteral("rotation"), number(f.screen->rotationDeg))
               : QString();
  }

  QString flyToViewElement(const KmlFeature& f, int32_t /*d*/) const
  {
    return f.flyToView ? leaf(QStringLiteral("flyToView"), QStringLiteral("1"))
                       : QString();
  }

  QString linkElement(const KmlFeature& f, int32_t d) const
  {
    return f.link ? link(*f.link, d) : QString();
  }

  bool m_gxDeclared = true;
};

bool isContainer(KmlFeature::Kind kind)
{
  return kind == KmlFeature::Kind::Document || kind == KmlFeature::Kind::Folder;
}

/** The slots a feature of @p kind has, in KML 2.2's order. */
const QStringList& slotsOf(KmlFeature::Kind kind)
{
  static const QStringList common{
      QStringLiteral("name"),        QStringLiteral("visibility"),
      QStringLiteral("open"),        QStringLiteral("Snippet"),
      QStringLiteral("description"), QStringLiteral("view"),
      QStringLiteral("time"),        QStringLiteral("styleUrl"),
      QStringLiteral("Style"),       QStringLiteral("StyleMap"),
      QStringLiteral("Region"),      QStringLiteral("ExtendedData")};
  static const QStringList container = common;
  static const QStringList placemark =
      common + QStringList{QStringLiteral("geometry")};
  static const QStringList ground =
      common +
      QStringList{QStringLiteral("color"),        QStringLiteral("drawOrder"),
                  QStringLiteral("Icon"),         QStringLiteral("altitude"),
                  QStringLiteral("altitudeMode"), QStringLiteral("LatLonBox"),
                  QStringLiteral("LatLonQuad")};
  static const QStringList screen =
      common +
      QStringList{QStringLiteral("color"),    QStringLiteral("drawOrder"),
                  QStringLiteral("Icon"),     QStringLiteral("overlayXY"),
                  QStringLiteral("screenXY"), QStringLiteral("rotationXY"),
                  QStringLiteral("size"),     QStringLiteral("rotation")};
  static const QStringList link =
      common + QStringList{QStringLiteral("flyToView"), QStringLiteral("Link")};
  switch (kind) {
  case KmlFeature::Kind::Placemark:
    return placemark;
  case KmlFeature::Kind::GroundOverlay:
    return ground;
  case KmlFeature::Kind::ScreenOverlay:
    return screen;
  case KmlFeature::Kind::NetworkLink:
    return link;
  case KmlFeature::Kind::Document:
  case KmlFeature::Kind::Folder:
    break;
  }
  return container;
}

int32_t rankOf(KmlFeature::Kind kind, const QString& slot)
{
  const int32_t index = static_cast<int32_t>(slotsOf(kind).indexOf(slot));
  return index < 0 ? childrenRank - 1 : index;
}

/** How many elements a feature has in @p slot (repeated: shared styles). */
int32_t countOf(const KmlFeature& f, const QString& slot)
{
  if (f.kind == KmlFeature::Kind::Document) {
    if (slot == QLatin1String("Style")) {
      return static_cast<int32_t>(f.styles.size());
    }
    if (slot == QLatin1String("StyleMap")) {
      return static_cast<int32_t>(f.styleMaps.size());
    }
  }
  return 1;
}

QString Builder::element(const KmlFeature& f, const QString& slot,
                         int32_t ordinal, int32_t d) const
{
  if (slot == QLatin1String("Style")) {
    return styleElement(f, ordinal, d);
  }
  if (slot == QLatin1String("StyleMap")) {
    return styleMapElement(f, ordinal, d);
  }
  if (ordinal != 0) {
    return QString();
  }
  using SlotWriter = QString (Builder::*)(const KmlFeature&, int32_t) const;
  static const std::map<QString, SlotWriter> writers{
      {QStringLiteral("name"), &Builder::nameElement},
      {QStringLiteral("visibility"), &Builder::visibilityElement},
      {QStringLiteral("open"), &Builder::openElement},
      {QStringLiteral("Snippet"), &Builder::snippetElement},
      {QStringLiteral("description"), &Builder::descriptionElement},
      {QStringLiteral("view"), &Builder::viewElement},
      {QStringLiteral("time"), &Builder::timeElement},
      {QStringLiteral("styleUrl"), &Builder::styleUrlElement},
      {QStringLiteral("Region"), &Builder::regionElement},
      {QStringLiteral("ExtendedData"), &Builder::extendedDataElement},
      {QStringLiteral("geometry"), &Builder::geometryElement},
      {QStringLiteral("color"), &Builder::colorElement},
      {QStringLiteral("drawOrder"), &Builder::drawOrderElement},
      {QStringLiteral("Icon"), &Builder::iconElement},
      {QStringLiteral("altitude"), &Builder::altitudeElement},
      {QStringLiteral("altitudeMode"), &Builder::altitudeModeElement},
      {QStringLiteral("LatLonBox"), &Builder::latLonBoxElement},
      {QStringLiteral("LatLonQuad"), &Builder::latLonQuadElement},
      {QStringLiteral("overlayXY"), &Builder::overlayXYElement},
      {QStringLiteral("screenXY"), &Builder::screenXYElement},
      {QStringLiteral("rotationXY"), &Builder::rotationXYElement},
      {QStringLiteral("size"), &Builder::sizeElement},
      {QStringLiteral("rotation"), &Builder::rotationElement},
      {QStringLiteral("flyToView"), &Builder::flyToViewElement},
      {QStringLiteral("Link"), &Builder::linkElement},
  };
  const std::map<QString, SlotWriter>::const_iterator found =
      writers.find(slot);
  return found != writers.end() ? (this->*(found->second))(f, d) : QString();
}

QString kindName(KmlFeature::Kind kind)
{
  switch (kind) {
  case KmlFeature::Kind::Document:
    return QStringLiteral("Document");
  case KmlFeature::Kind::Folder:
    return QStringLiteral("Folder");
  case KmlFeature::Kind::GroundOverlay:
    return QStringLiteral("GroundOverlay");
  case KmlFeature::Kind::ScreenOverlay:
    return QStringLiteral("ScreenOverlay");
  case KmlFeature::Kind::NetworkLink:
    return QStringLiteral("NetworkLink");
  case KmlFeature::Kind::Placemark:
    break;
  }
  return QStringLiteral("Placemark");
}

/** Writes a document. */
class Writer
{
public:
  Writer(const KmlDocument& document, bool gxDeclared)
      : m_text(document.sourceText.get()), m_build(gxDeclared)
  {
  }

  QString feature(const KmlFeature& f, int32_t d) const
  {
    if (f.source && m_text != nullptr) {
      return keptFeature(f, d);
    }
    return newFeature(f, d);
  }

  QString slice(int64_t begin, int64_t end) const
  {
    if (end <= begin) {
      return QString();
    }
    return m_text->mid(static_cast<qsizetype>(begin),
                       static_cast<qsizetype>(end - begin));
  }

private:
  QString startTag(const KmlFeature& f) const
  {
    return f.id.isEmpty() ? QStringLiteral("<%1>").arg(kindName(f.kind))
                          : QStringLiteral("<%1 id=\"%2\">")
                                .arg(kindName(f.kind), escaped(f.id));
  }

  QString children(const KmlFeature& f, int32_t d, const QString& between) const
  {
    QString out;
    for (std::size_t i = 0; i < f.children.size(); ++i) {
      out += (i == 0 ? QString() : between) + feature(f.children[i], d + 1);
    }
    return out;
  }

  /** A feature made (or read) without its source: KML 2.2's order. */
  QString newFeature(const KmlFeature& f, int32_t d) const
  {
    QString out = startTag(f);
    std::set<std::size_t> used;
    // Unknown elements after the element they followed.
    const std::function<void(const QString&)> unknownAfter =
        [&](const QString& preceding) {
          for (std::size_t i = 0; i < f.unknown.size(); ++i) {
            if (used.count(i) == 0 && f.unknown[i].first == preceding) {
              used.insert(i);
              out += newline(d + 1) + f.unknown[i].second;
              QString name = f.unknown[i]
                                 .second.section(QLatin1Char('>'), 0, 0)
                                 .mid(1)
                                 .section(QLatin1Char(' '), 0, 0)
                                 .section(QLatin1Char(':'), -1);
              if (name.endsWith(QLatin1Char('/'))) {
                name.chop(1);
              }
              unknownAfter(name);
            }
          }
        };
    unknownAfter(QString());
    for (const QString& slot : slotsOf(f.kind)) {
      for (int32_t i = 0; i < countOf(f, slot); ++i) {
        const QString element = m_build.element(f, slot, i, d + 1);
        if (!element.isEmpty()) {
          out += newline(d + 1) + element;
        }
      }
      for (const QString& name : kmlElementsOfSlot(slot)) {
        unknownAfter(name);
      }
    }
    for (std::size_t i = 0; i < f.unknown.size(); ++i) {
      if (used.count(i) == 0) {
        out += newline(d + 1) + f.unknown[i].second;
      }
    }
    if (!f.children.empty()) {
      out += newline(d + 1) + children(f, d, newline(d + 1));
    }
    return out + newline(d) + QStringLiteral("</%1>").arg(kindName(f.kind));
  }

  /** A feature read with its source: as it was, but for what changed. */
  QString keptFeature(const KmlFeature& f, int32_t d) const;

  friend class KeptFeature;

  const QString* m_text = nullptr;
  Builder m_build;
};

/**
 * Writes one feature read with its source: each part as it was unless it
 * changed, new elements where KML 2.2's order puts them, the children
 * where the first of them was.
 */
class KeptFeature
{
public:
  KeptFeature(const Writer& writer, const KmlFeature& f, int32_t d)
      : m_writer(writer), m_f(f), m_d(d), m_source(*f.source)
  {
    for (const KmlSourcePart& part : m_source.parts) {
      if (part.kind == KmlSourcePart::Kind::Known) {
        m_done.emplace(part.slot, part.ordinal);
      }
    }
  }

  QString write()
  {
    const QString start = m_writer.slice(m_source.begin, m_source.startEnd);
    const bool rawTags =
        m_f.id == m_source.id && !start.endsWith(QLatin1String("/>"));
    m_out = rawTags ? start : m_writer.startTag(m_f);
    m_previous = m_source.startEnd;
    std::size_t childParts = 0;
    for (const KmlSourcePart& part : m_source.parts) {
      childParts += part.kind == KmlSourcePart::Kind::Child ? 1 : 0;
    }
    std::size_t childPart = 0;
    for (const KmlSourcePart& part : m_source.parts) {
      const QString gap = m_writer.slice(m_previous, part.begin);
      m_previous = part.end;
      switch (part.kind) {
      case KmlSourcePart::Kind::Child:
        child(gap, ++childPart == childParts);
        break;
      case KmlSourcePart::Kind::Unknown:
        m_out += gap + m_writer.slice(part.begin, part.end);
        break;
      case KmlSourcePart::Kind::Known:
        known(part, gap);
        break;
      }
    }
    addNew(childrenRank);
    if (childParts == 0) {
      for (const KmlFeature& extra : m_f.children) {
        m_out += newline(m_d + 1) + m_writer.feature(extra, m_d + 1);
      }
    }
    if (rawTags) {
      return m_out + m_writer.slice(m_previous, m_source.end);
    }
    return m_out + newline(m_d) +
           QStringLiteral("</%1>").arg(kindName(m_f.kind));
  }

private:
  /** Elements the source didn't have, ranked below @p belowRank. */
  void addNew(int32_t belowRank)
  {
    for (const QString& slot : slotsOf(m_f.kind)) {
      if (rankOf(m_f.kind, slot) >= belowRank) {
        break;
      }
      for (int32_t i = 0; i < countOf(m_f, slot); ++i) {
        if (m_done.count({slot, i}) != 0 ||
            !m_written.emplace(slot, i).second) {
          continue;
        }
        const QString element = m_writer.m_build.element(m_f, slot, i, m_d + 1);
        if (!element.isEmpty()) {
          m_out += newline(m_d + 1) + element;
        }
      }
    }
  }

  void known(const KmlSourcePart& part, const QString& gap)
  {
    addNew(rankOf(m_f.kind, part.slot));
    const QString now =
        m_writer.m_build.element(m_f, part.slot, part.ordinal, 0);
    if (kmlTextHash(now) == part.hash) {
      m_out += gap + m_writer.slice(part.begin, part.end);
    } else if (!now.isEmpty()) {
      m_out +=
          gap + m_writer.m_build.element(m_f, part.slot, part.ordinal, m_d + 1);
    }
  }

  /**
   * A child where one was: the next of the feature's children (none if
   * there are fewer now); after the last, any more there are now.
   */
  void child(const QString& gap, bool last)
  {
    addNew(childrenRank);
    const std::vector<KmlFeature>& children = m_f.children;
    if (m_child < children.size()) {
      m_out += gap + m_writer.feature(children[m_child++], m_d + 1);
    }
    if (!last) {
      return;
    }
    const QString between = gap.trimmed().isEmpty() ? gap : newline(m_d + 1);
    while (m_child < children.size()) {
      m_out += between + m_writer.feature(children[m_child++], m_d + 1);
    }
  }

  const Writer& m_writer;
  const KmlFeature& m_f;
  int32_t m_d = 0;
  const KmlSource& m_source;
  QString m_out;
  int64_t m_previous = 0;
  std::size_t m_child = 0;
  std::set<std::pair<QString, int32_t>> m_done;
  std::set<std::pair<QString, int32_t>> m_written;
};

QString Writer::keptFeature(const KmlFeature& f, int32_t d) const
{
  return KeptFeature(*this, f, d).write();
}

/** Every style and overlay picture address of @p f and inside it. */
void collectHrefs(KmlFeature& f, const std::function<void(QString&)>& visit)
{
  const std::function<void(std::optional<KmlStyle>&)> inStyle =
      [&visit](std::optional<KmlStyle>& style) {
        if (style && style->icon) {
          visit(style->icon->href);
        }
      };
  inStyle(f.style);
  if (f.styleMap) {
    inStyle(f.styleMap->normal);
    inStyle(f.styleMap->highlight);
  }
  for (KmlStyle& style : f.styles) {
    if (style.icon) {
      visit(style.icon->href);
    }
  }
  for (KmlStyleMap& map : f.styleMaps) {
    inStyle(map.normal);
    inStyle(map.highlight);
  }
  if (f.kind == KmlFeature::Kind::GroundOverlay ||
      f.kind == KmlFeature::Kind::ScreenOverlay) {
    visit(f.iconHref);
  }
  for (KmlFeature& child : f.children) {
    collectHrefs(child, visit);
  }
}

bool isWeb(const QString& href)
{
  return href.startsWith(QLatin1String("http://")) ||
         href.startsWith(QLatin1String("https://"));
}

void collectStyleUrls(const KmlFeature& f, std::set<QString>& ids)
{
  for (const QString& url :
       {f.styleUrl, f.styleMap ? f.styleMap->normalUrl : QString(),
        f.styleMap ? f.styleMap->highlightUrl : QString()}) {
    if (url.startsWith(QLatin1Char('#'))) {
      ids.insert(url.mid(1));
    }
  }
  for (const KmlFeature& child : f.children) {
    collectStyleUrls(child, ids);
  }
}

void collectShared(const KmlFeature& f, std::vector<KmlStyle>& styles,
                   std::vector<KmlStyleMap>& maps)
{
  styles.insert(styles.end(), f.styles.begin(), f.styles.end());
  maps.insert(maps.end(), f.styleMaps.begin(), f.styleMaps.end());
  for (const KmlFeature& child : f.children) {
    collectShared(child, styles, maps);
  }
}

} // namespace

QString kmlFeatureElement(const KmlFeature& feature, const QString& slot,
                          int32_t ordinal)
{
  return Builder(true).element(feature, slot, ordinal, 0);
}

uint64_t kmlTextHash(const QString& text)
{
  // FNV-1a over the UTF-16 code units.
  constexpr uint64_t offset = uint64_t{14695981039346656037U};
  constexpr uint64_t prime = uint64_t{1099511628211U};
  uint64_t hash = offset;
  for (const QChar c : text) {
    hash ^= c.unicode();
    hash *= prime;
  }
  return hash;
}

QByteArray writeKml(const KmlDocument& document)
{
  QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  const QString* text = document.sourceText.get();
  if (text != nullptr && document.kmlSource) {
    // The kml element as it was: its namespaces, what came before it, the
    // elements around the feature.
    const KmlSource& kml = *document.kmlSource;
    const QString start =
        text->mid(static_cast<qsizetype>(kml.begin),
                  static_cast<qsizetype>(kml.startEnd - kml.begin));
    Writer writer(document, start.contains(QLatin1String("xmlns:gx")));
    QString prologue = writer.slice(document.prologueBegin, kml.begin);
    out += prologue.trimmed().isEmpty()
               ? QString()
               : prologue.trimmed() + QLatin1Char('\n');
    out += start;
    int64_t previous = kml.startEnd;
    for (const KmlSourcePart& part : kml.parts) {
      out += writer.slice(previous, part.begin);
      previous = part.end;
      out += part.kind == KmlSourcePart::Kind::Child
                 ? writer.feature(document.root, 1)
                 : writer.slice(part.begin, part.end);
    }
    out += writer.slice(previous, kml.end) + QLatin1Char('\n');
    return out.toUtf8();
  }
  Writer writer(document, true);
  out += QStringLiteral("<kml xmlns=\"%1\" xmlns:gx=\"%2\" xmlns:atom=\"%3\">")
             .arg(kmlNamespace, gxNamespace, atomNamespace);
  for (const QString& element : document.unknownTopLevel) {
    out += newline(1) + element;
  }
  out += newline(1) + writer.feature(document.root, 1);
  out += QStringLiteral("\n</kml>\n");
  return out.toUtf8();
}

namespace
{

bool writeFailed(QString* errorString, const QString& path, const QString& why)
{
  if (errorString != nullptr) {
    *errorString =
        QCoreApplication::translate("ts::KmlWriter", "Cannot write \"%1\": %2")
            .arg(QDir::toNativeSeparators(path), why);
  }
  return false;
}

bool addToZip(void* zip, const QString& name, const QByteArray& bytes)
{
  return CPLCreateFileInZip(zip, name.toUtf8().constData(), nullptr) ==
             CE_None &&
         CPLWriteFileInZip(zip, bytes.constData(),
                           static_cast<int32_t>(bytes.size())) == CE_None &&
         CPLCloseFileInZip(zip) == CE_None;
}

/**
 * The files @p document uses that go in a KMZ, by their place in it;
 * @p copy's addresses changed to point there.
 */
std::map<QString, QByteArray>
kmzMembers(const KmlDocument& document, KmlDocument& copy, QStringList* missing)
{
  std::map<QString, QByteArray> members;
  collectHrefs(copy.root, [&](QString& href) {
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty() || isWeb(trimmed)) {
      return;
    }
    const QByteArray bytes =
        readKmlLocalFile(resolveKmlHref(document, trimmed));
    if (bytes.isEmpty()) {
      if (missing != nullptr && !missing->contains(trimmed)) {
        missing->append(trimmed);
      }
      return;
    }
    // Relative addresses stay as they are; others point into files/.
    const bool relative = QUrl(trimmed).scheme().isEmpty() &&
                          !QDir::isAbsolutePath(trimmed) &&
                          !trimmed.startsWith(QLatin1String("../"));
    QString member = QDir::cleanPath(trimmed);
    if (!relative) {
      const QString name =
          QFileInfo(QUrl(trimmed).isLocalFile() ? QUrl(trimmed).toLocalFile()
                                                : trimmed)
              .fileName();
      member = QStringLiteral("files/") + name;
      for (int32_t n = 2;
           members.count(member) != 0 && members.at(member) != bytes; ++n) {
        member = QStringLiteral("files/%1-%2").arg(n).arg(name);
      }
      href = member;
    }
    members[member] = bytes;
  });
  return members;
}

bool writeKmz(const KmlDocument& document, const QString& path,
              QString* errorString, QStringList* missing)
{
  KmlDocument copy = document;
  const std::map<QString, QByteArray> members =
      kmzMembers(document, copy, missing);
  const QString part = path + QStringLiteral(".part");
  QFile::remove(part);
  void* zip = CPLCreateZip(part.toUtf8().constData(), nullptr);
  if (zip == nullptr) {
    return writeFailed(errorString, path,
                       QString::fromUtf8(CPLGetLastErrorMsg()));
  }
  // The document first, as KMZ readers expect.
  bool written = addToZip(zip, QStringLiteral("doc.kml"), writeKml(copy));
  for (const std::pair<const QString, QByteArray>& member : members) {
    written = written && addToZip(zip, member.first, member.second);
  }
  written = CPLCloseZip(zip) == CE_None && written;
  if (!written) {
    QFile::remove(part);
    return writeFailed(errorString, path,
                       QString::fromUtf8(CPLGetLastErrorMsg()));
  }
  QFile::remove(path);
  if (!QFile::rename(part, path)) {
    QFile::remove(part);
    return writeFailed(
        errorString, path,
        QCoreApplication::translate("ts::KmlWriter", "it can't be replaced."));
  }
  return true;
}

} // namespace

bool writeKmlFile(const KmlDocument& document, const QString& path,
                  QString* errorString, QStringList* missing)
{
  if (path.endsWith(QLatin1String(".kmz"), Qt::CaseInsensitive)) {
    return writeKmz(document, path, errorString, missing);
  }
  QSaveFile file(path);
  const QByteArray bytes = writeKml(document);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    return writeFailed(errorString, path, file.errorString());
  }
  return true;
}

KmlDocument kmlExtract(const KmlDocument& document, const QString& featurePath)
{
  KmlDocument extracted;
  extracted.path = document.path;
  extracted.base = document.base;
  extracted.kmz = document.kmz;
  extracted.webBase = document.webBase;
  extracted.sourceText = document.sourceText;
  extracted.root.kind = KmlFeature::Kind::Document;
  const KmlFeature* feature = nullptr;
  if (featurePath.isEmpty()) {
    feature = &document.root;
  } else {
    feature = &document.root;
    for (const QString& part : featurePath.split(QLatin1Char('.'))) {
      bool ok = false;
      const int32_t index = part.toInt(&ok);
      if (!ok || index < 0 ||
          static_cast<std::size_t>(index) >= feature->children.size()) {
        return extracted;
      }
      feature = &feature->children.at(static_cast<std::size_t>(index));
    }
  }
  extracted.root.name = feature->name;
  KmlFeature copy = *feature;
  // The shared styles it uses (the copy keeps those it holds itself).
  std::set<QString> ids;
  collectStyleUrls(copy, ids);
  std::vector<KmlStyle> styles;
  std::vector<KmlStyleMap> maps;
  collectShared(document.root, styles, maps);
  for (const KmlStyleMap& map : maps) {
    if (ids.count(map.id) != 0) {
      for (const QString& url : {map.normalUrl, map.highlightUrl}) {
        if (url.startsWith(QLatin1Char('#'))) {
          ids.insert(url.mid(1));
        }
      }
    }
  }
  std::set<QString> own;
  for (const KmlStyle& style : copy.styles) {
    own.insert(style.id);
  }
  for (const KmlStyleMap& map : copy.styleMaps) {
    own.insert(map.id);
  }
  for (const KmlStyle& style : styles) {
    if (ids.count(style.id) != 0 && own.insert(style.id).second) {
      extracted.root.styles.push_back(style);
    }
  }
  for (const KmlStyleMap& map : maps) {
    if (ids.count(map.id) != 0 && own.insert(map.id).second) {
      extracted.root.styleMaps.push_back(map);
    }
  }
  if (copy.kind == KmlFeature::Kind::Document) {
    copy.kind = KmlFeature::Kind::Folder; // a Document inside: as a folder
    copy.source.reset();
    std::move(copy.styles.begin(), copy.styles.end(),
              std::back_inserter(extracted.root.styles));
    std::move(copy.styleMaps.begin(), copy.styleMaps.end(),
              std::back_inserter(extracted.root.styleMaps));
    copy.styles.clear();
    copy.styleMaps.clear();
  }
  extracted.root.children.push_back(std::move(copy));
  return extracted;
}

KmlDocument
kmlDocumentOf(const QString& name,
              const std::vector<std::pair<QString, KmlGeometry>>& placemarks,
              const QColor& color)
{
  KmlDocument document;
  document.root.kind = KmlFeature::Kind::Document;
  document.root.name = name;
  KmlStyle style;
  style.id = QStringLiteral("terrasoar");
  KmlLineStyle line;
  line.color = color;
  line.width = 2.0;
  style.line = line;
  KmlPolyStyle poly;
  QColor fill = color;
  fill.setAlpha(color.alpha() / 3);
  poly.color = fill;
  style.poly = poly;
  document.root.styles.push_back(style);
  for (const std::pair<QString, KmlGeometry>& placemark : placemarks) {
    KmlFeature feature;
    feature.kind = KmlFeature::Kind::Placemark;
    feature.name = placemark.first;
    feature.styleUrl = QStringLiteral("#terrasoar");
    feature.geometry = placemark.second;
    document.root.children.push_back(std::move(feature));
  }
  return document;
}

} // namespace ts
