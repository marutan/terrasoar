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
 * The KML element model: documents, folders, placemarks, overlays and
 * network links, their geometry, styles, views, regions and times, as
 * read from KML 2.2 (with the gx: extensions that matter) and written
 * back. See docs/design/kml.md.
 */
#ifndef TS_KML_KMLDOCUMENT_H
#define TS_KML_KMLDOCUMENT_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <QColor>
#include <QDateTime>
#include <QString>
#include <QStringList>

namespace ts
{

/** How a KML geometry's heights are measured (altitudeMode). */
enum class KmlAltitudeMode : uint8_t {
  ClampToGround,      ///< On the ground (or the globe); heights ignored.
  RelativeToGround,   ///< Above the ground (the terrain where shown).
  Absolute,           ///< Above sea level.
  ClampToSeaFloor,    ///< On the sea floor (gx:); the ground on land.
  RelativeToSeaFloor, ///< Above the sea floor (gx:); the ground on land.
};

/** One position: longitude, latitude (degrees) and height (metres). */
struct KmlCoordinate {
  /** Longitude, degrees. */
  double lonDeg = 0.0;
  /** Latitude, degrees. */
  double latDeg = 0.0;
  /** Height, metres. */
  double altM = 0.0;

  /** Returns true if both are the same position. */
  bool operator==(const KmlCoordinate& other) const
  {
    return lonDeg == other.lonDeg && latDeg == other.latDeg &&
           altM == other.altM;
  }
};

/**
 * A geometry: a point, line, ring, polygon (outer boundary in
 * coordinates, holes in inner), a gx:Track (coordinates with their
 * times), or a MultiGeometry of others.
 *
 * @reentrant
 */
struct KmlGeometry {
  /** Which geometry. */
  enum class Kind : uint8_t {
    Point,
    LineString,
    LinearRing,
    Polygon,
    MultiGeometry,
    Track,
  };
  /** Which geometry. */
  Kind kind = Kind::Point;
  /** Points, the line, the ring or the outer boundary; a track's points. */
  std::vector<KmlCoordinate> coordinates;
  /** A polygon's holes. */
  std::vector<std::vector<KmlCoordinate>> inner;
  /** A track's times, one per coordinate (gx:Track's when). */
  std::vector<QDateTime> when;
  /** A MultiGeometry's members. */
  std::vector<KmlGeometry> children;
  /** How the heights are measured. */
  KmlAltitudeMode altitudeMode = KmlAltitudeMode::ClampToGround;
  /** Walls (or a stalk) down to the ground. */
  bool extrude = false;
  /** Lines follow the curve of the Earth when clamped. */
  bool tessellate = false;
};

/** The hot spot of an icon, or a screen overlay's anchor (vec2 units). */
struct KmlVec2 {
  /** Units of x and y. */
  enum class Units : uint8_t { Fraction, Pixels, InsetPixels };
  /** Horizontal value. */
  double x = 0.5;
  /** Vertical value. */
  double y = 0.5;
  /** Units of x. */
  Units xUnits = Units::Fraction;
  /** Units of y. */
  Units yUnits = Units::Fraction;
};

/** IconStyle. */
struct KmlIconStyle {
  /** The icon's address (href) as written; empty for the default. */
  QString href;
  /** Tint of the icon. */
  QColor color = QColor(255, 255, 255);
  /** Size, as a factor of the default. */
  double scale = 1.0;
  /** Degrees clockwise from north. */
  double heading = 0.0;
  /** The point of the icon placed on the position. */
  KmlVec2 hotSpot;
};

/** LabelStyle. */
struct KmlLabelStyle {
  /** Colour of the label. */
  QColor color = QColor(255, 255, 255);
  /** Size, as a factor of the default. */
  double scale = 1.0;
};

/** LineStyle. */
struct KmlLineStyle {
  /** Colour of the line. */
  QColor color = QColor(255, 255, 255);
  /** Width in pixels. */
  double width = 1.0;
};

/** PolyStyle. */
struct KmlPolyStyle {
  /** Colour of the fill. */
  QColor color = QColor(255, 255, 255);
  /** Whether polygons are filled. */
  bool fill = true;
  /** Whether polygons are outlined. */
  bool outline = true;
};

/** BalloonStyle. */
struct KmlBalloonStyle {
  /** The balloon's template ($[name], $[description], …); empty: default. */
  QString text;
  /** Background colour. */
  QColor backgroundColor = QColor(255, 255, 255);
  /** Text colour. */
  QColor textColor = QColor(0, 0, 0);
  /** False for displayMode "hide". */
  bool display = true;
};

/** ListStyle: how a folder shows in the layer tree. */
struct KmlListStyle {
  /** listItemType. */
  enum class ItemType : uint8_t {
    Check,             ///< Children switched on and off one by one.
    RadioFolder,       ///< At most one child on.
    CheckOffOnly,      ///< Can only be switched off together.
    CheckHideChildren, ///< Children not listed.
  };
  /** How its children are listed and switched. */
  ItemType itemType = ItemType::Check;
};

/**
 * A Style: each sub-style where the document gives it (unset ones fall
 * back to the referenced style's, then the defaults).
 *
 * @reentrant
 */
struct KmlStyle {
  /** The id attribute (what a styleUrl or link names). */
  QString id;
  /** IconStyle, if given. */
  std::optional<KmlIconStyle> icon;
  /** LabelStyle, if given. */
  std::optional<KmlLabelStyle> label;
  /** LineStyle, if given. */
  std::optional<KmlLineStyle> line;
  /** PolyStyle, if given. */
  std::optional<KmlPolyStyle> poly;
  /** BalloonStyle, if given. */
  std::optional<KmlBalloonStyle> balloon;
  /** ListStyle, if given. */
  std::optional<KmlListStyle> list;

  /** Returns @p over laid over this: its set sub-styles replace these. */
  KmlStyle mergedWith(const KmlStyle& over) const;
};

/** A StyleMap: the normal and highlight styles, by URL or inline. */
struct KmlStyleMap {
  /** The id attribute (what a styleUrl or link names). */
  QString id;
  /** The normal style's address. */
  QString normalUrl;
  /** The highlighted style's address. */
  QString highlightUrl;
  /** The normal style, if inline. */
  std::optional<KmlStyle> normal;
  /** The highlighted style, if inline. */
  std::optional<KmlStyle> highlight;
};

/** TimeStamp (when) or TimeSpan (begin and end, either open). */
struct KmlTime {
  /** Start; none for always. */
  std::optional<QDateTime> begin;
  /** End; none for ever. */
  std::optional<QDateTime> end;

  /** Returns true if @p time is within it (open ends always are). */
  bool contains(const QDateTime& time) const;
};

/** LookAt or Camera. */
struct KmlView {
  /** Which kind. */
  enum class Kind : uint8_t { LookAt, Camera };
  /** LookAt or Camera. */
  Kind kind = Kind::LookAt;
  /** Longitude, degrees. */
  double lonDeg = 0.0;
  /** Latitude, degrees. */
  double latDeg = 0.0;
  /** Height, metres. */
  double altM = 0.0;
  /** Direction faced, degrees clockwise from north. */
  double headingDeg = 0.0;
  /** Tilt from looking straight down, degrees. */
  double tiltDeg = 0.0;
  /** LookAt: distance from the point, metres. */
  double rangeM = 1000.0;
  /** Camera: roll, degrees. */
  double rollDeg = 0.0;
  /** How the heights are measured. */
  KmlAltitudeMode altitudeMode = KmlAltitudeMode::ClampToGround;
};

/** Region: a box and the sizes on screen it shows between. */
struct KmlRegion {
  /** Northern edge, degrees. */
  double north = 90.0;
  /** Southern edge, degrees. */
  double south = -90.0;
  /** Eastern edge, degrees. */
  double east = 180.0;
  /** Western edge, degrees. */
  double west = -180.0;
  /** Lowest altitude of the box, metres. */
  double minAltM = 0.0;
  /** Highest altitude of the box, metres. */
  double maxAltM = 0.0;
  /** Lod: shown once the box is this many pixels across (side). */
  double minLodPixels = 0.0;
  /** Lod: hidden once it is more; −1 for no limit. */
  double maxLodPixels = -1.0;
};

/** GroundOverlay's place: a LatLonBox (with rotation) or a gx:LatLonQuad. */
struct KmlLatLonBox {
  /** Northern edge, degrees. */
  double north = 0.0;
  /** Southern edge, degrees. */
  double south = 0.0;
  /** Eastern edge, degrees. */
  double east = 0.0;
  /** Western edge, degrees. */
  double west = 0.0;
  /** Degrees anticlockwise. */
  double rotationDeg = 0.0;
  /** gx:LatLonQuad: lower left, lower right, upper right, upper left. */
  std::vector<KmlCoordinate> quad;
};

/** ScreenOverlay's placement. */
struct KmlScreenPlacement {
  /** The point of the image placed at screenXY. */
  KmlVec2 overlayXY{0.0, 0.0};
  /** Where on the screen. */
  KmlVec2 screenXY{0.0, 0.0};
  /** The point it turns about. */
  KmlVec2 rotationXY{0.0, 0.0};
  /** −1 or 0 per axis keep the image's own size (or aspect). */
  KmlVec2 size{-1.0, -1.0};
  /** Rotation, degrees anticlockwise. */
  double rotationDeg = 0.0;
};

/** NetworkLink's Link. */
struct KmlLink {
  /** The address loaded. */
  QString href;
  /** refreshMode: onChange (0), onInterval (1), onExpire (2). */
  enum class RefreshMode : uint8_t { OnChange, OnInterval, OnExpire };
  /** When it is loaded again. */
  RefreshMode refreshMode = RefreshMode::OnChange;
  /** Seconds between loads (onInterval). */
  double refreshIntervalS = 4.0;
  /** viewRefreshMode: never, onStop, onRequest, onRegion. */
  enum class ViewRefreshMode : uint8_t { Never, OnStop, OnRequest, OnRegion };
  /** When a change of view loads it again. */
  ViewRefreshMode viewRefreshMode = ViewRefreshMode::Never;
  /** Seconds after the view stops (onStop). */
  double viewRefreshTimeS = 4.0;
  /** Scale of the view's box sent to the server. */
  double viewBoundScale = 1.0;
  /** viewFormat (BBOX=[bboxWest],… by default when view-based). */
  QString viewFormat;
  /** Extra query items (with [placeholders]). */
  QString httpQuery;
};

/**
 * One feature: a Document, Folder, Placemark, GroundOverlay,
 * ScreenOverlay or NetworkLink, with what every feature has (name,
 * visibility, description, style, view, region, time, extended data) and
 * what its kind has. Elements Terra Soar doesn't model are kept as XML
 * (unknown), in place, for writing back.
 *
 * @reentrant
 */
struct KmlFeature {
  /** Which kind. */
  enum class Kind : uint8_t {
    Document,
    Folder,
    Placemark,
    GroundOverlay,
    ScreenOverlay,
    NetworkLink,
  };
  /** Which kind. */
  Kind kind = Kind::Placemark;
  /** The id attribute (what a styleUrl or link names). */
  QString id;
  /** Name for the user (and its label). */
  QString name;
  /** Whether it is shown. */
  bool visibility = true;
  /** Whether it is shown open in the layer tree. */
  bool open = false;
  /** Its description (HTML), shown in its balloon. */
  QString description;
  /** A short description. */
  QString snippet;
  /** The shared style it uses (\#id, or file\#id). */
  QString styleUrl;
  /** An inline Style (StyleSelector), if any. */
  std::optional<KmlStyle> style;
  /** An inline StyleMap, if any. */
  std::optional<KmlStyleMap> styleMap;
  /** Where to look at it from (double-clicking flies there). */
  std::optional<KmlView> view;
  /** Shown only while its region is big enough on screen. */
  std::optional<KmlRegion> region;
  /** When it exists (TimeStamp or TimeSpan). */
  std::optional<KmlTime> time;
  /** ExtendedData: Data and SchemaData's SimpleData, name and value. */
  std::vector<std::pair<QString, QString>> extendedData;

  /** Placemark: its geometry. */
  std::optional<KmlGeometry> geometry;
  /** Overlays: the image (Icon href) and its colour and order. */
  QString iconHref;
  /** Tint and opacity of the overlay's image. */
  QColor color = QColor(255, 255, 255);
  /** Order of overlapping overlays (higher on top). */
  int32_t drawOrder = 0;
  /** GroundOverlay: where, at which height and how. */
  std::optional<KmlLatLonBox> latLonBox;
  /** GroundOverlay: its height, metres. */
  double altitudeM = 0.0;
  /** How the heights are measured. */
  KmlAltitudeMode altitudeMode = KmlAltitudeMode::ClampToGround;
  /** ScreenOverlay: where on the screen. */
  std::optional<KmlScreenPlacement> screen;
  /** NetworkLink: what it loads, and whether it flies to its view. */
  std::optional<KmlLink> link;
  /** NetworkLink: fly to the loaded document's view. */
  bool flyToView = false;

  /** Document: its shared styles and style maps. */
  std::vector<KmlStyle> styles;
  /** Document: its shared style maps. */
  std::vector<KmlStyleMap> styleMaps;
  /** Document and Folder: what they contain, in order. */
  std::vector<KmlFeature> children;

  /**
   * Child elements not modelled, as XML (namespaces declared), with the
   * name of the modelled element they followed (empty: first), to be
   * written back in place.
   */
  std::vector<std::pair<QString, QString>> unknown;
};

/**
 * A KML document as read: its root feature (a Document, or a Folder or
 * Placemark at the top), where it came from, and what was skipped.
 *
 * @reentrant
 */
struct KmlDocument {
  /** The top feature (usually a Document). */
  KmlFeature root;
  /** The file it was read from (inside a KMZ: the archive). */
  QString path;
  /** True if read from a KMZ archive. */
  bool kmz = false;
  /** Where relative addresses start: a directory, or /vsizip/ and the archive's
   * path. */
  QString base;
  /** NetworkLinkControl and other top-level elements kept as XML. */
  std::vector<QString> unknownTopLevel;
  /**
   * Elements skipped as unknown or unsupported, one line each: element
   * name, line and column (for the user, not an error).
   */
  QStringList skipped;

  /** Returns the shared style with @p id, or null. */
  const KmlStyle* findStyle(const QString& id) const;
  /** Returns the shared style map with @p id, or null. */
  const KmlStyleMap* findStyleMap(const QString& id) const;
};

/** Returns @p mode's name as written in KML. */
QString kmlAltitudeModeName(KmlAltitudeMode mode);

/** Reads a KML colour (aabbggrr hex); invalid if it isn't one. */
QColor kmlColor(const QString& text);

/** Returns @p color as KML writes it (aabbggrr). */
QString kmlColorText(const QColor& color);

} // namespace ts

#endif // TS_KML_KMLDOCUMENT_H
