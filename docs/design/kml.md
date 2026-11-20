# KML

How KML and KMZ files become layers: the element model, reading, how
styles resolve, drawing through the vector pipeline, the layer tree,
balloons and views. Code: `core/include/ts/kml/`. For users:
[kml.md](../user/kml.md).

Phase 16 is delivered in parts: 16a (import and display) and 16b
(authoring and export). This page grows with each part.

## The model

`ts::KmlDocument` holds a root `ts::KmlFeature` (a Document, or a Folder
or Placemark at the top) and what was skipped. A feature has what every
KML feature has (id, name, visibility, open, description, snippet,
styleUrl, an inline Style or StyleMap, a LookAt or Camera, a Region, a
TimeStamp or TimeSpan, ExtendedData) and what its kind has:

| Kind | Its own |
| --- | --- |
| Document, Folder | Children in order; a Document's shared styles and style maps |
| Placemark | A `ts::KmlGeometry`: Point, LineString, LinearRing, Polygon (outer and holes), MultiGeometry, gx:Track (with its times; gx:MultiTrack is a MultiGeometry of tracks), with altitude mode, extrude and tessellate |
| GroundOverlay | The image, colour, draw order, height and altitude mode, a LatLonBox with rotation or a gx:LatLonQuad |
| ScreenOverlay | The image and its placement (overlayXY, screenXY, rotationXY, size, rotation) |
| NetworkLink | Its Link (href, refresh and view-refresh modes and times, view bound scale, viewFormat, httpQuery) and flyToView |

Elements the model doesn't hold are kept as XML with the name of the
element they followed (`KmlFeature::unknown`), so a document can be
written back with them in place (16b), and listed in
`KmlDocument::skipped` with their line and column. Top-level elements
other than the feature (NetworkLinkControl) are kept too.

## Reading

`ts::parseKml()` reads with `QXmlStreamReader`, matching local names, so
the default namespace (KML 2.0, 2.1, 2.2), `kml:` prefixes and `gx:`
extensions (`gx:Track`, `gx:coord`, `gx:altitudeMode`,
`gx:LatLonQuad`) are all read. Coordinates tolerate spaces after commas,
tabs and newlines. Times use the program's ISO 8601 reader
([wms.md](wms.md#time)). A file that isn't well-formed XML fails with
the parser's message, line and column; one whose root isn't `kml` (or a
feature) is refused; anything else unknown is kept and listed, never an
error.

`ts::readKmlFile()` reads a `.kml`, or a `.kmz` (a zip: sniffed by its
first bytes) through GDAL's `/vsizip/`: its `doc.kml`, else the first
`.kml` at its top. A document's `base` is its directory, or the archive,
so `ts::resolveKmlHref()` and `ts::readKmlResource()` find icons and
images relative to it, inside the KMZ when there is one; web addresses
are left to the caller (fetched through the disk cache).

## Styles

`ts::KmlStyleResolver` gives a feature its style: the Style or StyleMap
its `styleUrl` names (`#id` in the document, any Document in it, or
`file.kml#id` in another local file, read once), the StyleMap's normal
or highlight style (by URL or inline), then the feature's own inline
Style laid over it. Each sub-style (Icon, Label, Line, Poly, Balloon,
List) is replaced whole by a later one; unset ones take KML's defaults
where used (white, width 1, fill and outline on).

## Drawing

A layer source `{"kind": "kml", "path": …, "container": "0.2"}` shows the
placemarks directly in one container (child indices from the root).
`ts::KmlFeatureProvider` turns them into vector features
(`kmlPlacemarkFeatures()`) with their KML styles as per-feature styles
([vector-rendering.md](vector-rendering.md#per-feature-styles)):

- lines in LineStyle's colour and width; polygons filled with PolyStyle's
  colour (unless `fill` is 0) and outlined in the line's (unless
  `outline` is 0);
- points as dots in IconStyle's colour, sized by its scale, yellow for
  the default pushpin (icons and labels come in 16a-3);
- a MultiGeometry as one feature per member.

Longitudes are made continuous along each ring and line (no jump across
±180°), and a feature reaching past ±180° also comes shifted by 360°, so
each side's tiles clip their part: a line from 170° E to 170° W draws
both halves, the short way. The features are in memory; a tile gets
those whose extent meets it, stored at level 6 at most. All documents
with one path share one copy (`sharedKmlDocument()`).

## In the window

**Opening.** File → Open KML or KMZ…, files dropped on the window, and
files given on the command line (as the desktop's Open With passes them:
`.kml` and `.kmz` are associated on Linux and offered in Open With on
Windows, without taking them from another program) all go through
`MainWindow::openFile()`. The document becomes a collection named after
it (or its file), each Folder and Document inside a collection in order,
and each container's placemarks one vector layer (Places) where its first
placemark is; `visibility` 0 leaves an entry switched off. The document's
own view, if any, is flown to. Elements skipped are counted in the status
bar.

**Balloons.** A click (What Is Here, [wms.md](wms.md#getfeatureinfo))
first asks the visible KML layers for the placemark under the pointer
(`placemarksAt()`: points before lines before areas, then the last
drawn). Its balloon (`kmlBalloonHtml()`) is its BalloonStyle's text with
`$[name]`, `$[description]`, `$[Snippet]`, `$[id]` and each extended
data `$[field]` or `$[field/displayName]` filled in (text escaped, the
description kept as HTML), else its name, description and a table of its
extended data, in the balloon's background colour. It shows in a
`QTextBrowser`: rich text only, nothing run and nothing fetched from the
web (local images next to a `.kml` show); web links open in the browser.
`displayMode` hide shows nothing.

**Views.** `kmlViewPose()` turns a LookAt into the camera's pose as it
is, and a Camera into the ground point it looks at (straight down when
not tilted) at the matching range; tilts are kept below 85°. Without a
view, `kmlFeaturePose()` frames the geometry or the overlay's box. A
folder's or layer's context menu has Fly There, and a balloon has a Fly
there link.

## Tests

`tests/kml`: every element of `everything.kml` (structure, geometries,
times, views, regions, overlays, links), unknown elements kept in place
and listed, styles through StyleMaps, inline, ListStyle and another file,
colours, a KMZ with its icon and image, GDAL's KML test files (Google's
KML Samples, every geometry, gx:Track and gx:MultiTrack, prefixes, odd
whitespace, extended data, external styles) and broken files; the vector
features of placemarks (styles, default icon, MultiGeometry, the
antimeridian), the provider (tiles, picking order), the registry, view
poses and balloon HTML. `tests/ui`: a KML opened into the layer tree, a
balloon on click, Fly There, a KMZ.
