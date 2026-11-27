# KML

How KML and KMZ files become layers: the element model, reading, how
styles resolve, drawing through the vector pipeline, the layer tree,
balloons and views. Code: `core/include/ts/kml/`. For users:
[kml.md](../user/kml.md).

Phase 16 is delivered in parts: 16a (import and display) and 16b
(authoring and export).

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
- points as icons with labels (below), kept as features only for
  picking;
- a MultiGeometry as one feature per member.

Longitudes are made continuous along each ring and line (no jump across
±180°), and a feature reaching past ±180° also comes shifted by 360°, so
each side's tiles clip their part: a line from 170° E to 170° W draws
both halves, the short way. The features are in memory; a tile gets
those whose extent meets it, stored at level 6 at most. All documents
with one path share one copy (`sharedKmlDocument()`).

## Icons and labels

Each point of a placemark (a MultiGeometry's too) is a
`ts::KmlBillboard` (`kmlBillboards()`): its icon's address (resolved
against the document, so inside a KMZ), IconStyle's colour, scale,
heading and hotSpot, and the placemark's name in LabelStyle's colour and
scale, on the first point only. The main window turns the visible KML
layers' billboards into `BillboardDraw`s whenever the layers change, and
`GlobeView` paints them with `QPainter` over each frame, upright on the
globe, the maps and through the morph:

- the icon (32 pixels times the scale) turned by the heading, with its
  hot spot on the place (KML counts y from the bottom; the default
  pushpin's needle is at its bottom centre), tinted by multiplying its
  colours and opacity by IconStyle's colour;
- the label to the right of the icon, with a dark halo so it reads on any
  imagery.

On the globe a place on the far side (its ellipsoid normal facing away
from the eye) is hidden. Where they crowd, one icon is drawn per 6-pixel
cell and labels that would overlap one already drawn are left out, with
at most 400 labels, so a file with tens of thousands of places stays
smooth. Their heights are as for raised shapes (below); an extruded
point has a stalk down to the ground in LineStyle's colour.

`IconCache` (app) holds the icons: KMZ members and local files read at
once (`readKmlLocalFile()`), web icons fetched in the background through
the program's network client and kept in the cache directory
(`kml-icons/`, by the address's SHA-1) so they show again offline. Until
an icon arrives, and for one that can't be had, the default pushpin
(drawn by the program: yellow, 32 pixels) stands in; tinted copies are
kept per colour.

## Heights

Clamped geometry (clampToGround, the default, and gx:clampToSeaFloor) is
drawn on the ground through the tiles, as above; tessellate makes no
difference there, since draped lines follow the ground anyway. Lines,
rings and polygons with heights (absolute, relativeToGround and
gx:relativeToSeaFloor) are `ts::KmlRaisedShape`s instead
(`kmlRaisedShapes()`): edges over a degree are divided, with their
heights interpolated, so long ones follow the curve of the Earth rather
than cutting through it. Their features stay with the provider for
picking (by their footprint) but are left out of the tiles.

`GlobeView` draws them with `QPainter` over each frame (`RaisedShapeDraw`):

- each point at its height above the ellipsoid (absolute) or above the
  terrain's loaded height there (relative; the terrain as drawn, so
  exaggerated when it is), points facing away from the eye hidden;
- extruded shapes have a wall from each edge down to the ground, filled
  with PolyStyle's colour; a polygon's top is filled with it too when the
  whole of it is in sight (holes left open);
- the line (and polygon outline unless PolyStyle's `outline` is 0) in
  LineStyle's colour and width, over the walls it tops;
- walls, tops and lines from all shapes sorted by depth and drawn far to
  near.

Terrain in front of a shape doesn't hide it: they are drawn over the
frame, like the billboards. On the maps they are drawn flat, without
walls.

## Overlays

**GroundOverlay.** Each is an imagery layer (source `{"kind":
"kmloverlay", "path": …, "feature": "0.3"}`), so it is draped on the
terrain, blended and ordered like any imagery, and can be faded with the
layer's opacity. `ts::KmlGroundOverlayProvider` reads the picture once
(from the KMZ or next to the file; from the web through the network
client, asked again after a failure), multiplies it by the overlay's
colour, and fills each tile it touches by finding, for each tile pixel,
where it falls in the picture (`kmlOverlayImagePoint()`, bilinear):

- a LatLonBox: x from west to east, y from north to south, the picture
  turned `rotation` degrees anticlockwise about the box's centre (in
  degrees of latitude and longitude, as Google Earth does);
- a gx:LatLonQuad: the inverse of the bilinear stretch between its four
  corners (lower left, lower right, upper right, upper left);
- boxes across the antimeridian (east < west), longitudes taken on the
  box's side.

Its deepest level is one past where the tiles' pixels match the
picture's. `altitude` is not used: the picture is always draped
(clampToGround); drawOrder is the order in the layer tree.

**ScreenOverlay.** Each is a layer of its own (a `kml` source naming the
overlay), so it can be switched off. The main window hands the visible
ones to `GlobeView` (`ScreenOverlayDraw`: the picture through
`IconCache`, tinted, sorted by drawOrder), which places each with
`kmlScreenRect()` on every frame: size (−1 the picture's own, 0 keeping
its aspect, else a fraction of the view or pixels), overlayXY on the
picture put on screenXY on the view (fractions and pixels from the
bottom left, insetPixels from the top right), turned `rotation` degrees
anticlockwise about rotationXY on the view. They are drawn over
everything but the attributions and scale bar.

## Time

A feature shows at the program's clock (`ts/time`, [wms.md](wms.md#time))
if its TimeStamp is at or before it (from then on, the way Google Earth
shows stamped features up to the slider's time) or the clock is within its
TimeSpan, from begin up to (not including) end; a container's time
narrows its children's (`kmlShownAt()`, `kmlNarrowTime()`).

So that tiles aren't rebuilt as the clock runs, a KML layer's source gets
an `"at"` member only from the main window (`timedStack()`), and only one
of a few values: the latest of the layer's *boundaries* (every begin, end
and stamp in it, `kmlTimeBoundaries()`) at or before the clock
(`kmlTimeKey()`). What shows is the same anywhere between two boundaries,
so the source, the provider (which filters its placemarks by `at`) and the
tiles change only as the clock crosses one. `clockMoved()` compares the
vector sources as it does the imagery's.

The time bar is offered the layers' boundaries, and each gx:Track's span
in hundredths, as part of its extent (`kmlTimeExtent()`), so it steps to
the moments something changes.

**gx:Track.** A track is drawn as its line all the time; a marker (the
placemark's icon and label) shows where it is at the clock, interpolated
between its points, and nothing before its first time or after its last
(`kmlTrackPosition()`, `KmlFeatureProvider::trackMarkersAt()`). The
billboards are made again whenever the clock moves.

## Regions

A Region is active when some of its LatLonAltBox is in sight and its size
on the screen (the square root of the area of the screen rectangle its
sample points cover, `kmlRegionPixels()`) is between minLodPixels and
maxLodPixels. Regions are checked on the layer tree's KML entries when the
view has been still for 300 ms (`updateKmlRegions()`): an entry whose own
Region or a container's is inactive, and everything under it in the tree
(a link's document included), is left out of the stack until it is
active again. A NetworkLink with a Region is fetched the first time its
Region becomes active (superoverlays load level by level as the view
comes closer). A placemark listed as an entry of its own has its own
Region checked too; one inside a Places layer (a folder of more than
500) is not used (the layer is the unit); minAltitude places the box's
sample
points, maxAltitude and the fade extents are not used.

## NetworkLinks

Each NetworkLink is a collection in the tree; `KmlLinks` (app) fetches
what it links to and the main window puts that document inside the
collection, replacing what was there:

- the address is the Link's href resolved against its document (a file
  next to it or in its KMZ, or the web), with viewFormat (by default
  `BBOX=[bboxWest],[bboxSouth],[bboxEast],[bboxNorth]` when the link
  refreshes with the view) and httpQuery filled in from the view and the
  program (`kmlLinkUrl()`, `kmlViewParameters()`: the view's bounds from
  where its edges meet the ground, scaled by viewBoundScale);
- local files are read where they are; web documents (and KMZ members)
  are written to a temporary folder for the session, each fetch a file of
  its own, and remembered as copies of their address (`setKmlWebBase()`),
  so relative addresses inside them (icons, more links) point at the web
  (`KmlDocument::webBase`);
- refreshMode onInterval fetches again every refreshInterval;
  viewRefreshMode onStop fetches viewRefreshTime after the view stops;
  onRequest and the rest only when asked (Refresh in the entry's menu);
  onExpire is treated as onChange (once);
- flyToView flies to the new document's view; a failure is shown in the
  status bar and what was there stays;
- each web link's last document is also kept in the cache folder
  (`kml-links/`, by the SHA-1 of its address without the view's
  parameters), and shown, with a message, when it can't be fetched and
  nothing is shown yet (offline);
- links inside links are followed eight deep at most.

## Lists (ListStyle)

A container's ListStyle `listItemType` shapes its entries: `radioFolder`
keeps at most one child on (the first the file shows; switching one on
switches the others off); `checkHideChildren` lists it as one entry, a
Places layer of everything inside (`"deep": true`); `check` and
`checkOffOnly` list children as usual.

## In the window

**Opening.** Local Sources → Open KML or KMZ…, files dropped on the
window, and files given on the command line (as the desktop's Open With
passes them: `.kml` and `.kmz` are associated on Linux and offered in
Open With on Windows, without taking them from another program) all go
through `MainWindow::openFile()`. The document becomes a collection
named after it (or its file), each Folder and Document inside a
collection in order, and each placemark a vector layer of its own where
it is (`addKmlPlace()`: the layer's source has the placemark's path as
its container, so its provider draws it alone), named after it or, with
no name, its kind (`kmlEntryName()`: Placemark, Path, Polygon, Track).
A container with more than 500 placemarks (`kmlListedPlacemarks`) has
them in one vector layer, Places, where its first is: a layer each
would cost its own draws on every tile in view. `visibility` 0 leaves an
entry switched off; in a radio folder, each placemark's entry is one of
the choices. The document's
own view, if any, is flown to. The file is read on a worker thread
(`readKmlInBackground()`): the window keeps drawing, with a busy dialog
after half a second (100,000 placemarks take about 1.5 s to read and
0.15 s to make into features). `sharedKmlDocument()` reads without
holding its lock, so other documents can be had meanwhile. Ground and screen overlays are layers of
their own where they are in the file. Elements skipped are counted in the status
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
folder's or layer's context menu has Fly There, double-clicking a KML
entry flies there (it isn't renamed; F2 still renames it), and a balloon
has a Fly there link.

## Writing

`ts::writeKml()` writes a document; `ts::writeKmlFile()` writes it to a
`.kml`, or a `.kmz` (`CPLCreateZip`: doc.kml first, then the files it
uses).

**Documents made in Terra Soar** are written in KML 2.2's element order
(the schema's), with only what isn't the default, the `kml`, `gx` and
`atom` namespaces declared on the root, HTML descriptions and balloon
texts in CDATA, numbers without exponents or needless digits, times in
UTC. Unknown elements of a document read without its text go back after
the element they followed.

**Round trips.** A document to be saved is read with
`KmlReadOptions::keepSource`: the text is kept (`KmlDocument::sourceText`,
decoded from UTF-8 or Latin-1; files in other encodings are read without
it) and each feature notes where it is in the text (`KmlSource`): its
start and end tags, and each element inside it in order
(`KmlSourcePart`: a known element by *slot* – `kmlSlotOf()`, e.g. "view"
for LookAt or Camera – an unknown element, or a child feature). For each
known element the reader keeps a hash of how Terra Soar would write it
from what was read (`kmlFeatureElement()`, `kmlTextHash()`). The writer
then goes through each feature's parts in the order of the text:

- the text between parts (space, line ends, comments) as it was;
- a known element whose hash still matches as it was in the text, else
  written anew from the model (and left out if it is gone);
- unknown elements as they were;
- child features where children were, each written the same way, new
  ones after the last;
- elements the feature didn't have, where KML 2.2's order puts them;
- the start and end tags as they were, unless the id changed.

So a document read and written without changes comes back as it was
(but for Terra Soar's XML declaration), whatever the program that wrote
it, and an edit changes only the elements it touches: other programs'
extensions, `ExtendedData` with `SchemaData`, comments and layout stay.
Elements written anew lose what the model doesn't hold (`colorMode`, an
Icon's refresh settings). `gx:` elements written anew declare the `gx`
namespace on themselves if the root doesn't.

The positions come from the stream reader's position after each tag;
where a tag starts is found by looking back for its `<` (the reader's
position before a token may already be past it).

**KMZ.** The files the document uses (icons in its styles, overlay
pictures) go in the archive: those given by relative address at that
address (a KMZ saved again keeps its layout), others (absolute paths)
in `files/`, the document changed to point there. Web addresses stay as
they are; local files that can't be read are reported (not an error).

**Exporting.** `ts::kmlExtract()` makes a document of one folder (or
overlay or link) with the shared styles it uses, keeping its text.
`ts::kmlDocumentOf()` makes a document of named geometries in one style.
In the window: Save As KML or KMZ… on a KML entry of the layer tree
(the file read again with its text kept: the whole document for its top
entry, else that folder), Save as KML… in the elevation profile window
(the line, along the ground) and Export as KML… in Offline Regions (the
area, its first ring the outside and the others holes).

## Editing

**The model.** `ts/kml/kmledit.h`: features are addressed by their path
(child indices from the root, as everywhere in this page);
`kmlInsertFeature()`, `kmlTakeFeature()` and `kmlMoveFeature()` change
the tree (a move into the feature itself is refused, and a path made
stale by taking the feature out is corrected). A placemark's geometry is
edited through its *handles* (`kmlHandles()`): each vertex of each of its
lines and rings in turn (a polygon's outer ring, then its holes; a
MultiGeometry's members), a ring's repeated closing point once, and the
middle of each edge. `kmlMoveVertex()` keeps a vertex's height (and a
ring's first and last point together), `kmlInsertVertex()` puts a vertex
before another with the height between its neighbours',
`kmlDeleteVertex()` refuses to leave a line fewer than 2 points or a ring
fewer than 3; points and tracks only move. A ground overlay's handles are
its corners: a gx:LatLonQuad's move freely, a LatLonBox's move their two
edges.

**`ts::KmlEditDocument`** holds a document being edited, under a *key*
(`kmledit:<uuid>`, also its path) by which `registerKmlDocument()` makes
`sharedKmlDocument()` find it: each change publishes an unchanging copy,
so drawing never sees a change half made. Every `edit()` keeps the whole
document before it as an undo step (snapshots, simple and safe: a
document being edited is small; at most 100 steps), with a description
for the Undo and Redo menu items; `adjust()` changes it without a step
(switching a folder on or off), in every step too. A revision number
changes with every change, undo and redo; `isModified()` compares it
with the one saved.

**In the window** (`mainwindowkml.cpp`):

- *KML → New KML Document* makes an empty Document; *Edit This Document*
  (or the entry's menu) reads an opened file again with its text kept
  and puts the editable copy in its place in the layer tree. The
  document edits go to (the *active* one) is the selected entry's, else
  the last edited; new elements go in the selected folder (or the
  selected element's).
- *Add Placemark* takes the next click; *Add Path* and *Add Polygon*
  use the view's line and polygon drawing (shared with elevation
  profiles and offline regions, each taking only drawings it asked for);
  *Add Hole to Polygon* draws a ring into the selected polygon; *Add
  Ground Overlay* asks for a picture and takes a dragged rectangle. Drawn
  paths and areas are tessellated, in the program's yellow.
- Clicking an element of a document being edited (instead of its
  balloon) selects it: `GlobeView` draws its handles (squares at
  vertices, dots at middles), dragging one emits `handleMoved()` (a
  middle dragged adds a vertex), right-clicking a vertex
  `handleDeleteRequested()`.
- *Properties…* (Alt+Return, `KmlPropertiesDialog`): name, description
  with its balloon as it will look, the view (from the camera), style
  (its own, or one of the document's shared styles, which then changes
  for every element using it), height (altitude mode, one height for
  all its points, extrude), time (always, a stamp or a span) and
  extended data.
- *Cut*, *Copy*, *Paste*, *Delete*: copying keeps the element with the
  shared styles it uses (`kmlExtract()`) and puts it on the clipboard as
  KML too; pasting adds them, written anew (`kmlWithoutSource()`), and
  the styles the document hasn't; KML copied in another program pastes
  too.
- *Properties…* in a placemark's or ground overlay's entry menu
  (`showKmlEntryProperties()`) opens the same dialog for that element.
  For a document opened as it is, the dialog shows the file as opened;
  only if what it gives back writes differently (`writeKml()` of the
  document with and without the change) does the document become the
  editable copy (as Edit This Document) and take the change
  (`setKmlEntryFeature()`). Elements of a document a network link
  fetched have no Properties…. Choosing an element's entry in the tree
  selects it (`kmlEntryChosen()`), as a click on it does.
- Dragging an entry into another folder of the same document in the
  layer tree moves the feature (a Places entry moves all the folder's
  placemarks); ticking and renaming entries change the document (a name
  given for the entry's kind isn't taken as a rename). After
  each change the document's entries are made again from it, their
  layer sources carrying the revision (`"rev"`) so their providers and
  tiles are made again.
- *Undo*, *Redo* (Ctrl+Z, Ctrl+Shift+Z), *Save KML Document* and *Save
  KML Document As…* (KML or KMZ) work on the active document.

## Projects

A project lists its KML documents (`kml`, [layer-model.md](layer-model.md#project-files)):
each by the id of its entry in the layer tree and its file, or its text
for a document never saved. On saving, an edited document linked to a
file with unsaved changes asks: save them to the file, or keep them in
the project (its text, with the file it belongs to; opened again, it is
still modified). The entries inside a document's entry aren't saved;
opening the project makes them again from the file or text, so links,
lists and regions work as when the file was opened.

## Tests

`tests/kml`: every element of `everything.kml` (structure, geometries,
times, views, regions, overlays, links), unknown elements kept in place
and listed, styles through StyleMaps, inline, ListStyle and another file,
colours, a KMZ with its icon and image, GDAL's KML test files (Google's
KML Samples, every geometry, gx:Track and gx:MultiTrack, prefixes, odd
whitespace, extended data, external styles) and broken files; the vector
features of placemarks (styles, default icon, MultiGeometry, the
antimeridian), billboards (icon, tint, scale, heading, hot spot, label, a
KMZ's icon read from the archive), raised shapes (modes, walls, fill
and outline rules, long edges divided, left out of tiles but picked),
ground overlays (box, rotation, antimeridian, gx:LatLonQuad, a picture
warped into a tile the right way round and tinted, its deepest level, a
KMZ's, the registry), screen overlay placement (units, size, aspect,
rotation), times (stamps, spans, containers', boundaries and keys, the
time bar's extent, a provider at a time), tracks (interpolation, the
antimeridian, markers), Regions (size on the screen from near and far,
out of sight, on the way to a feature), link addresses (view bounds,
viewFormat, httpQuery, relative to a file or a web copy), deep layers,
the provider (tiles without points,
picking order), the registry, view poses and balloon HTML; writing: every fixture read and written back unchanged (the same text),
edits written as the only changes, new documents (read back the same and
valid against the OGC KML 2.2 schema with `xmllint`, `tests/data/kml/schema`),
KMZ archives with their files, GDAL reading what is written with the same
shapes, a folder extracted with its styles; editing: paths, inserting,
taking and moving features, handles, moving, adding and deleting
vertices, ground overlay corners, undo and redo with their limits.
`tests/ui`: a
KML opened into the layer tree (an entry per placemark, named after it
or its kind, a folder of more than 500 as Places), a balloon on click, Fly There, icons and
labels drawn (not from the far side of the Earth, not when their folder
is off), a KMZ; raised shapes from `heights.kml` (walls, a polygon's top,
a stalk) drawn and hidden from the far side; ground overlays as imagery
layers and a screen overlay drawn and switched off; NetworkLinks to local
files (one waiting for its Region, then hidden again), Refresh, a radio
folder and a checkHideChildren folder (`links.kml`); a web link through a
mock server (the view and client in its query, a failure); placemarks
showing with the clock and a track's marker moving, each switched on in
its radio folder; Properties… on an entry (a file opened as it is
edited only when something changes, undone), an entry chosen selecting
its element, an entry named for its kind not renamed by a tick.

Editing in the window (`tests/ui`): a new document with a placemark,
path, polygon with a hole, folder and ground overlay; handles dragged,
middles turning into vertices, vertices deleted; undo and redo;
properties and the Properties dialog (its own and a shared style);
copy, paste, cut and the clipboard; saving a KMZ; a project keeping an
unsaved change; a file made editable, edited and saved (only the edit
changed); a folder dragged into another.
