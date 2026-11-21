# Terra Soar – Future Features

Nov 17, 2026 · @Peter · Draft

## Overview

The [phased roadmap](<Terra Soar – Phased Roadmap.md>) (Phases 0–8) is complete: Terra Soar 0.0.1 was released on 2026-11-14. This document plans the work after it. It gathers the roadmap's "Later work" list, the items 0.0.1 deferred, and new candidates, and groups them into proposed phases.

**Status**: a draft for review. The phase order, and whether each phase is wanted at all, are proposals until agreed. Agreed decisions are recorded in each phase's **Decisions** paragraph, as in the first roadmap. Items under [Ideas](#ideas-not-yet-planned) are not planned yet.

The first roadmap's rules still apply to every phase here:

- Each phase ends with working software, design notes in `docs/`, Doxygen API docs and regression tests. CI must be green on Windows and Linux × Qt 5/Qt 6.
- Every earlier test stays. A test changes only with a written reason in the PR.
- No test needs the internet. New network sources get a mock server in the test suite.
- The performance budgets hold on the reference PC (Core i3-4130, HD Graphics 4400, 4 GB RAM) and under software rendering.
- The coding standards hold, including the zero-warning, zero-finding policy and fixed-width integers.
- Each new data source is listed in `THIRD_PARTY_NOTICES` and the About window, with its licence and attribution.

## Summary

| Phase | Theme | Main deliverables |
| --- | --- | --- |
| 9 | User guide in the program | The `docs/user` guide in a window from the Help menu |
| 10 | WMTS | WMTS layers added, browsed, cached and taken offline like WMS layers |
| 11 | More projections | Transverse Mercator/UTM, UK National Grid, Cassini, Lambert Conformal, Polyconic, Azimuthal Equidistant, Gnomonic, Equidistant Cylindrical, Rotated Mercator, Gall (Stereographic, Peters/Orthographic), RPF native, polar and world projections |
| 12 | More web services | XYZ/TMS, WFS; WMS TIME and GetFeatureInfo |
| 16 | KML | 16a: import KML/KMZ and draw it on the globe and map; 16b: create KML, add elements, export |
| 13 | Terrain analysis | Slope/aspect, contours, line of sight, viewshed |
| 14 | Realism | Atmosphere, sun lighting, glTF models |
| 15 | Satellite orbits | Satellites from TLEs, with past and future tracks over chosen time spans |
| 17 | Aircraft from ADS-B | Recorded, live-receiver and airplanes.live ADS-B data as aircraft in 3D above the globe, with trails: off, a time span, or all |
| 18 | Ships from AIS | Recorded, live-receiver and open online AIS data as vessels on the map and globe, with trails like Phase 17's |
| 19 | Vector layer colours | Change the colours (and line width) of the graticule and shoreline layers |
| 20 | On-screen navigation controls | An overlay on the view with rotate, pan, zoom and tilt controls, on the globe and the map |
| 21 | VMAP0 data pack | NGA's Vector Map Level 0 (1:1,000,000 world vector map) prepared, tested and published as downloadable packs |
| 22 | Local data files | Vector files (GeoJSON, shapefile, GPX, GeoPackage) and GeoTIFF imagery |
| 23 | Commercial sources | API-key services, key storage, terms handling |
| 24 | Rendering backend | Qt 6 RHI (Vulkan, Direct3D 11) |
| 25 | Road to 1.0 | Code signing, Flathub, translations, accessibility, 1.0 release |

The phases are mostly independent. Phase 9 (the user guide in the program) depends on nothing else and comes first. Phase 25 (road to 1.0) comes last: 1.0 is released when the phases before it are done. Phases 12 (WFS), 16 (KML) and 22 (local data files) share a vector feature pipeline: per-feature styles, attributes, picking and draping. Phase 12 now comes first, so it builds the pipeline, and Phases 16 and 22 extend it. Phase 16 (KML) comes straight after Phase 12, ahead of Phases 13 to 15 (the table lists the phases in that order; their numbers stay as they were, so references between phases still hold): it extends Phase 12's vector feature pipeline and follows the program clock Phase 12 built for its time elements. Phase 14's lighting is built on OpenGL first, and Phase 24's backend (which comes later) carries it over with every other shader. Phase 15 needs a program-wide clock, which Phase 12's WMS time slider and Phase 14's sun position also need. Whichever of those phases comes first should build the clock for all three. KML's time elements use the same clock. Phase 10 (WMTS) reuses the WMS machinery and needs nothing else, so it comes early. Phase 12's XYZ tiles then reuse its Web Mercator reprojection. Phase 17 (aircraft) plays recordings on the program-wide clock and shares Phase 15's way of drawing moving objects and their tracks, so it is easiest after Phase 15. It can use Phase 14's 3D models if they exist, and falls back to simple shapes if not. Phase 18 (ships) is Phase 17 at sea level: it shares the track store, trails, playback and live connections. Whichever comes first builds them as a shared `ts/tracks` subsection for satellites, aircraft and ships. Phase 19 (vector layer colours) is small and depends on nothing, so it can be done at any time. WFS layers, KML without its own styles, and Phase 22's vector files reuse its style dialog. Phase 20 (navigation controls) depends on nothing either. It makes the flat map turn and tilt in every projection, including Phase 11's. Phase 21 (VMAP0) uses the existing packager and pack manager. Its place names need text labels on the globe, which Phases 16, 17 and 18 need too. Whichever comes first builds label drawing for all of them, and until then the VMAP0 packs can ship without names.

## Phase 9 – User guide in the program

Goal: the user guide in `docs/user/` can be read inside Terra Soar, offline, from the Help menu, so it matches the installed version.

**Status (2026-11-18)**: delivered. Help → User Guide (F1) opens the guide in a window of its own, built in from `docs/user/`, with links, anchors, Back/Forward/Contents, search and find in page, and saved window state. Context help (F1) works in six dialogs, and the About window links to the guide. `check_user_guide.py` is in CI. See `docs/design/help.md`.

**Deliverables**

- **Help → User Guide (F1)** opens a help window. It is non-modal, so the guide can stay open beside the globe.
- The window shows the guide's contents (`docs/user/README.md`) as its start page, with a contents list down the side.
- **Every page in `docs/user/` is built into the program** as a Qt resource, with the screenshots it uses (`docs/images/`). The window works offline and always shows the guide for the version installed.
- **Markdown is shown directly** with `QTextBrowser`/`QTextDocument::setMarkdown` (Qt 5.14 and later, so both Qt 5.15 and Qt 6). The pages stay plain Markdown, and the same files are what GitHub shows.
- **Links:**
  - Links between pages, and to headings within them (`offline-use.md#space-on-disk`), open in the window.
  - Back, Forward and Home buttons.
  - Web links open in the system browser.
  - Links that leave the guide (`../building.md`, `../design/...`) open the file's page in the repository on GitHub for this version's tag.
- Find in page (Ctrl+F), and a search across all pages.
- The window remembers its size, position and last page.
- Context help, if wanted: F1 in a dialog (Add WMS Layer, Offline Regions, Data Packs, Elevation Profile) opens the guide at that dialog's section.
- The About window's About tab links to the guide.
- The packages need nothing extra, because the guide is inside the program.

**Technical documentation**

- `docs/design/help.md`: how the guide is built in, how links are resolved, and how to add a page.
- `docs/user/README.md`: the guide's contents. A new page appears in the window by being added there and to the resource list (checked in CI).

**Regression tests**

- Every Markdown file in `docs/user/` is in the program's resources, and every resource has a source file (a style check like `check_notices.py`, so a new page can't be forgotten).
- Every page loads and renders, and every image it uses is found.
- Link check: every link between pages, and every heading anchor, resolves. Links out of the guide map to a repository URL.
- UI test: Help → User Guide opens the window at the contents. Following a link, Back and Home show the right pages, and search finds a known phrase.
- Context help, if built: each dialog's F1 opens its section.

**Exit criteria**: the whole user guide reads in the program offline, every link and image works, and CI fails if a page is added to `docs/user/` without being built in.

## Phase 10 – WMTS

**Status (2026-11-18):** being delivered in three parts. **10a**, the engine (capabilities, tile matrix maths, requests, the provider for EPSG:4326 and Web Mercator tile matrix sets, tests and `docs/design/wmts.md`), is done. **10b**, the Add WMTS Layer dialog (style, format, tile matrix set and dimension choices, terms and credit shown), the WMTS catalogue (its own file), the user guide page and offline regions for WMTS, is done. **10c**, any other CRS PROJ knows (British National Grid, RD New, LAEA Europe, …) reprojected on the CPU, with a British National Grid golden image, completes the phase.

Goal: users add any WMTS layer by URL or from the catalogue, and it streams, caches, blends and goes offline like a WMS layer. WMTS serves pre-cut tiles, so it is usually faster than WMS and is how many national mapping agencies and NASA GIBS publish their maps.

**Approach**: the same structure as WMS (Phase 5), sharing what WMS already built:
- `CachedTileFetcher` for the network, disk cache, revalidation and offline handling
- the catalogue
- the layer tree and project storage
- offline regions and the attribution display

What is new is the capabilities model, tile matrix sets, and fitting their tiles to the EPSG:4326 quadtree.

**Deliverables**

- **Capabilities parser** (WMTS 1.0.0), for both KVP (`GetCapabilities` by query) and RESTful (`WMTSCapabilities.xml`) services:
  - layers
  - styles (and the default)
  - formats
  - tile matrix sets: CRS, scale denominators, top-left corners, tile and matrix sizes, and limits per layer
  - dimensions (TIME and others)
  - resource URL templates
  - bounding boxes, legends, and the service's attribution and access constraints
- **`WmtsImageryProvider`:**
  - **Requests:** each quadtree tile is drawn from the tile matrix whose resolution best matches it, by `GetTile` (KVP) or the resource URL template (RESTful).
  - **Tile matrix sets:**
    - In EPSG:4326 (including `WorldCRS84Quad`), the tiles are used directly when they line up with the quadtree, and cut or combined when they don't.
    - In EPSG:3857 (`GoogleMapsCompatible`/`WebMercatorQuad`), they are reprojected as WMS's EPSG:3857 fallback is.
    - Other CRSs that PROJ knows (e.g. British National Grid, EPSG:27700; national grids) are reprojected through PROJ.
  - **Axis order** as each CRS defines it (WMTS uses the CRS's own axis order, like WMS 1.3.0).
  - **Coverage:** areas outside a layer's tile matrix limits are never requested.
- **Formats:** PNG with transparency for overlays and JPEG for opaque layers, choosing the service's best offer by default. The format can be changed per layer.
- **Dimensions:** TIME follows the program's clock (see Phase 15), with the service's default when there is no clock. Other dimensions are chosen in the layer's properties. Cache keys include every dimension value.
- **Add WMTS Layer… dialog**, like Add WMS Layer…:
  - the service URL, or a pick from the catalogue
  - a layer browser with titles, abstracts and legends
  - style, format, tile matrix set and dimension choices
  - the service's terms and attribution shown before adding
  - adding to the layer tree
- **Catalogue:** open WMTS services in the online catalogue (JSON), editable without a release, with their terms and attribution. Candidates include:
  - NASA GIBS
  - national mapping agencies with open WMTS services (Ordnance Survey's open data, swisstopo, basemap.at, the Dutch PDOK and others)
  - EMODnet Bathymetry
- **Network behaviour, as WMS:**
  - concurrency limit
  - off-screen tiles cancelled
  - retries with back-off
  - per-layer error and offline states
  - HTTP cache headers honoured
- **Offline regions:** WMTS layers can be kept offline, counting tiles by the service's own tile matrices, and only where the service's terms allow it.
- **Everywhere:** WMTS layers work in the 2D views, through the morph and draped on terrain, like every other layer.

**Technical documentation**

- `docs/design/wmts.md`: the capabilities model, tile matrix selection, alignment with the quadtree, reprojection, axis order, dimensions and cache keys
- `docs/user/adding-wmts-layers.md`: the user guide, alongside `adding-wms-layers.md`

**Regression tests**

- **Capabilities parsing:** a stored set of real-world capabilities documents, KVP and RESTful, including odd-but-valid ones, multiple tile matrix sets and per-layer limits.
- **Tile matrix maths:** the tile chosen for a quadtree tile, and its pixel window, match hand-computed values in EPSG:4326, EPSG:3857 and EPSG:27700. The scale denominator to resolution conversion is checked for degree- and metre-based CRSs.
- **Request building:** `GetTile` URLs and filled resource templates match expected strings, including style, format, dimension and tile matrix values.
- **Mock WMTS server** (local `QTcpServer`, as for WMS): slow, failing, oversized, wrong-format and missing-tile (404) responses.
- **Cancellation:** tiles leaving the view are cancelled and never uploaded.
- **Cache and offline:** hits and misses, revalidation, cache keys that differ by dimension value, and offline regions downloaded and used without a network.
- **Golden images:**
  - a mock WMTS overlay at 50% alpha over Blue Marble, in EPSG:4326 and EPSG:3857 tile matrix sets
  - the same overlay in Mercator, in Miller and at morph t = 0.5
  - a British National Grid layer lined up with the shoreline
- **Dialog:** the UI test adds a layer from a mock service through Add WMTS Layer…

**Exit criteria**: a public WMTS service can be added and browsed smoothly in 3D and 2D, in each supported tile matrix set, and taken offline. All tests run without internet access.

## Phase 11 – More projections

Goal: the flat map in the projections people need for regional, national, navigation and world maps. The first roadmap delivered Mercator, Web Mercator and Miller Cylindrical.

**Status (2026-11-19):** delivered in parts 11a to 11e. RPF data is read through GDAL (File → Add Imagery File…, an `A.TOC`), but resampled into the quadtree's tiles, not drawn pixel for pixel in its zone; drawing on the frames' own grid waits for Phase 22 (`docs/design/rpf.md`).

**Deliverables**

Projections, grouped by kind. Each is in View → Map Projection, with its parameters (centre, central meridian, standard parallels, zone) in a Map Projection Settings dialog where it has any.

- **Cylindrical:**
  - **Equidistant Cylindrical**, with a chosen standard parallel. Plate Carrée is its case at the equator.
  - **Gall Stereographic**: secant at 45° N and S, a compromise for world maps.
  - **Gall–Peters**: cylindrical equal-area with standard parallels at 45°.
  - **Gall Orthographic**: Gall's name for the same projection as Gall–Peters, so it is one entry in the menu, named "Gall–Peters (Gall Orthographic)".
  - **Rotated Mercator**: Mercator about a rotated pole (oblique Mercator), with the centre line chosen by a point and an azimuth, or by two points. It suits long, narrow areas that don't run north–south, such as a coastline or a flight route.
- **Transverse:**
  - **Transverse Mercator**, with a chosen central meridian, scale factor and false origin, and UTM zones (chosen, or picked automatically from the view's centre).
  - **Cassini** (Cassini–Soldner): the transverse equidistant cylindrical, still the basis of many older national and cadastral surveys.
  - **UK National Grid** (Ordnance Survey National Grid, EPSG:27700): Transverse Mercator on the Airy 1830 ellipsoid with the OSGB36 datum.
    - **Datum:** positions convert from WGS 84 through PROJ with the OSTN15 grid, accurate to about 0.1 m, falling back to the Helmert transform, accurate to about 5 m, without the grid. OSTN15 can join the existing `geoid-osgm15` pack as a "UK grids" pack.
    - **Grid:** the 100 km grid squares (e.g. `TQ`) and grid references (`TQ 30080 80729`) in the status bar.
- **Conic:**
  - **Lambert Conformal Conic**, with one or two standard parallels, the standard projection for aeronautical charts and many national grids.
  - **Polyconic** (American Polyconic): a cone per parallel, used for older topographic series.
- **Azimuthal:**
  - **Azimuthal Equidistant**, centred on any point. Distances and directions from the centre are true, as on radio and great-circle range maps. The polar case is offered as a preset.
  - **Gnomonic**, centred on any point: every great circle is a straight line, so routes plan as straight lines. It shows less than a hemisphere, and the map is clipped where the projection runs to infinity.
  - **Polar Stereographic** (north and south), carried over from this phase's first list.
- **World maps:**
  - **Robinson, Mollweide and Winkel Tripel**, carried over from the first list.
- **Raster Product Format (RPF) Native**:
  - **What it is:** the ARC (Equal Arc-second Raster Chart) system of the RPF products, CADRG maps and CIB imagery. It has 18 latitude zones, nine per hemisphere. Each zone is equirectangular with its own east–west pixel spacing, and the polar zones are azimuthal equidistant.
  - **What it gives:** RPF charts shown in it are drawn pixel for pixel, as they were made. The map shows the zone in view and changes zone as the view moves.
  - **Reading RPF data:** GDAL reads it (NITF and `A.TOC`), and this phase is where it fits best. If added here, Phase 22's local imagery reuses it.
- **Graticule and readout**: a latitude/longitude graticule in every projection, and a grid for projected systems (UTM, UK National Grid, Cassini and Lambert zones). The status bar reads coordinates in the current projection's system (eastings and northings, grid references) beside latitude and longitude.
- **Everywhere:**
  - Layers, terrain hillshade, the scale bar, attributions, picking and the 3D/2D morph work in every projection.
  - Each projection is written so that the flat map can later be turned and tilted in it (Phase 20, navigation controls): forward, inverse and outline don't assume a north-up view.
  - **Beyond cylindrical:** the renderer and the morph must handle:
    - curved and interrupted map outlines;
    - projections that show only part of the Earth (gnomonic, one RPF zone);
    - parameters that move the map's centre.
  - The scale bar shows when the scale varies across the view, which most of these projections do.
- **Projection framework:** the current cylindrical-only `Projection` classes are generalised to any projection with forward, inverse, a valid domain and its outline. The shader reprojection, which draws quadtree tiles, takes per-projection code for each one. PROJ is the reference implementation in the tests, not a run-time dependency of drawing.

**Technical documentation**

- `docs/design/projections.md`:
  - each projection, with its formulas, parameters, valid domain and distortion
  - how the morph and clipping handle non-cylindrical outlines
  - the RPF ARC zone system
  - the OSGB36 datum conversion
- `docs/user/getting-around.md`: choosing a projection and its settings

**Regression tests**

- **Transforms:**
  - Forward and inverse at reference points match PROJ (`+proj=tmerc`, `cass`, `aeqd`, `eqc`, `gnom`, `lcc`, `poly`, `omerc`, `gall`, `cea`, `stere`, `robin`, `moll`, `wintri`) to within millimetres, including near each projection's edges and poles.
  - A forward-then-inverse round trip returns the start point.
- **UK National Grid:**
  - the Ordnance Survey's published OSTN15 test points to their stated accuracy;
  - grid references formatted and parsed for known points;
  - the Helmert fallback within 5 m.
- **Gall–Peters/Gall Orthographic:** equal-area: the areas of graticule cells match the ellipsoid's.
- **Rotated Mercator:** the chosen centre line maps to a straight line, with scale true along it.
- **Gnomonic:** great circles map to straight lines, and points beyond the horizon are rejected, not drawn.
- **RPF ARC zones:**
  - the zone limits and pixel spacings match MIL-PRF-89038 (the RPF specification);
  - a fixture CADRG frame is drawn without resampling in its own zone.
- **Golden images:** a world map and a regional map in each projection, with the graticule, and the UK in the National Grid lined up with the shoreline.
- **Morph:** each projection morphs without frame jumps (continuity at every step).
- **Settings:** changing parameters (centre, zone, standard parallels) redraws correctly and is saved in the project.

**Exit criteria**: every listed projection draws, morphs, picks and reads coordinates correctly, matches PROJ, and stays within the frame budget.

## Phase 12 – More web services

**Status (2026-11-20):** delivered (PR #51): XYZ/TMS tiles (12a), the program clock with WMS and WMTS TIME and Blue Marble month by month as a data pack (12b), What Is Here with WMS GetFeatureInfo (12c), and WFS with per-feature styles in the vector pipeline (12d). See `docs/design/xyz.md`, `wms.md#time`, `wms.md#getfeatureinfo` and `wfs.md`.

Goal: the common web map services beyond WMS.

**Deliverables**

- XYZ/TMS tile services (`{z}/{x}/{y}` templates), reprojected from Web Mercator onto the quadtree, through `CachedTileFetcher` (disk cache, revalidation, offline regions).
- (WMTS has its own phase, 10.)
- WFS 2.0: features in the visible area, with paging, into the vector feature pipeline (per-feature styles, attributes, picking and draping), which this phase builds and Phases 16 (KML) and 22 (local data files) extend.
- WMS extras:
  - The TIME dimension, with a time slider and a play button
  - GetFeatureInfo on click
  - Blue Marble's monthly variants as a TIME-like choice
- Entries for these services in the catalogue, with terms and attribution.

**Technical documentation**

- `docs/design/wfs.md`; time handling added to `docs/design/wms.md`

**Regression tests**

- Mock servers for XYZ and WFS, with stored capabilities from real services.
- TIME: the right request for each slider position, and cache keys that include the time.
- GetFeatureInfo parsing (HTML, XML, JSON responses).
- Offline regions work for XYZ layers.

**Exit criteria**: each service type adds, draws, caches and goes offline like WMS.

## Phase 16 – KML

**Status (2026-11-20):** delivered (16a-1 to 16b-2 on one branch, one pull request). Design: `docs/design/kml.md`; for users: `docs/user/kml.md`. Differences from the plan below:
- Shapes above the ground, icons and labels are drawn over the frame, so terrain in front of them doesn't hide them; a Region on a single placemark inside a Places layer is not used (containers', overlays' and links' are).
- The reading fixtures are Google's KML Samples, GDAL's test files and files written by GDAL's KML and LIBKML drivers (which QGIS uses); the OGC specification's own examples are not included.
- Display is tested by what is drawn (counts, placement, hiding) and checked by eye, not by golden images.

Goal: Terra Soar reads KML and KMZ as faithfully as the programs people make them in, and can create, edit and export KML of its own. Phase 16a (import and display) is useful alone. Phase 16b (authoring and export) builds on it.

**Approach (proposed)**: our own KML 2.2 reader and writer on `QXmlStreamReader`/`QXmlStreamWriter`, in a `core` subsection (`ts/kml`) with its own unit test project (`tests/kml`).
- GDAL's KML drivers flatten documents into layers and lose most styling and structure, which both display and round-tripping need.
- KMZ archives are read and written through GDAL's zip support (`/vsizip/`, `CPLCreateZip`), which is already linked.
- The `gx:` extensions that Google Earth writes are read where they matter (`gx:Track`, `gx:altitudeMode`).

### Phase 16a – Import and display

**Deliverables**

- **Opening:**
  - File → Open KML/KMZ… and drag and drop.
  - `.kml` and `.kmz` registered with the desktop (Linux MIME type, Windows file association) to open in Terra Soar.
- **Document structure:** the document's `Document`/`Folder` tree becomes a collection in the layer tree.
  - Folders keep their names, order and visibility (`visibility`, `open`), and can be switched on and off like layers.
  - `ListStyle` is honoured (check, radio and hidden folders).
- **Placemarks:**
  - Geometry: `Point`, `LineString`, `LinearRing`, `Polygon` (with holes) and `MultiGeometry`.
  - Each has its name as a label, and its description (HTML) shown in a balloon on click.
  - `ExtendedData` is shown as a table in the balloon.
- **Styles:** `Style` and `StyleMap` (normal and highlight), shared by `styleUrl` within the document or in another file.
  - `IconStyle` (icon, scale, colour, heading, hotspot)
  - `LabelStyle`
  - `LineStyle` (colour and width)
  - `PolyStyle` (fill, outline)
  - `BalloonStyle`
  - Icons come from the KMZ, local files or the web (through the disk cache).
- **Heights:**
  - Altitude modes `clampToGround`, `relativeToGround` and `absolute`, plus `clampToSeaFloor`/`relativeToSeaFloor`, against the terrain when it is on.
  - `extrude` (walls and stalks to the ground)
  - `tessellate` (lines following the curve of the Earth)
- **Overlays:**
  - `GroundOverlay`: an image draped over a `LatLonBox` (with rotation) or `gx:LatLonQuad`, as an imagery layer with opacity.
  - `ScreenOverlay`: a legend or logo fixed to the view.
- **Views:**
  - `LookAt` and `Camera`: double-clicking a placemark or folder flies there.
  - The document's own view is used when it opens.
- **Network and time:**
  - `NetworkLink`: online KML loaded and refreshed on its interval or view change, through the disk cache, and offline from the last copy.
  - `Region` and `Lod`: content shown only when its area is big enough on screen.
  - `TimeStamp`, `TimeSpan` and `gx:Track` follow the program's clock: features appear and disappear with time, and tracks animate.
- **Robustness:**
  - Unknown or unsupported elements are skipped and listed (not an error).
  - Malformed files report the line and column.
  - Large files (100,000 placemarks) load off the GUI thread within the memory budget.
- **On the map:** everything draws on the flat map as on the globe, including split at the antimeridian, through the vector feature pipeline built in Phase 12.

### Phase 16b – Create, edit and export

**Deliverables**

- **New documents:** File → New KML Document creates an editable KML layer, and an imported document can be made editable.
- **Adding elements, drawn on the globe or map (reusing the region and profile drawing tools):**
  - placemarks
  - paths
  - polygons (with holes)
  - ground overlays (an image file placed and stretched by its corners)
  - folders to group them
- **Editing:**
  - Move a placemark, drag a path's or polygon's vertices, and insert and delete vertices.
  - Cut, copy, paste and delete.
  - Drag elements between folders.
  - Full undo and redo.
- **Properties dialog per element:**
  - name, description (with a preview)
  - style: icon, colours, line width, fill, label scale
  - altitude, altitude mode and extrude
  - time stamp or span
  - extended data fields
  - its view (set from the current camera)
- **Shared styles:** edit a style once for all the elements that use it.
- **Export:**
  - Save as KML or KMZ (a KMZ includes the icons and overlay images it uses).
  - Export a single folder.
  - Export a drawn region or an elevation profile line as KML.
- **Round trip:** a file opened, edited and saved keeps what Terra Soar doesn't edit, including unknown elements, `gx:` extensions and comments where possible, so editing in Terra Soar doesn't lose another program's data.
- **Projects:** editable KML layers are saved with the project, or linked to their file with a prompt to save changes.

**Technical documentation**

- `docs/design/kml.md`: the reader and writer, the element model, how styles resolve, altitude modes against terrain, network links and caching, and round-trip preservation.
- `docs/user/kml.md`: opening KML and KMZ, drawing and editing, and exporting.

**Regression tests**

- **Reading:**
  - every supported element from fixture files, checking geometry, structure, styles and resolved `styleUrl`s, including shared and external styles
  - the OGC KML 2.2 specification's examples
  - files written by Google Earth, QGIS and GDAL (stored, with their licences noted)
- **KMZ:** reading and writing archives with icons and overlay images, and paths relative to the archive.
- **Errors:** malformed XML, unknown elements, missing icons and broken links are each reported without crashing.
- **Display:** golden images of styled placemarks, lines, polygons, extruded and clamped geometry on terrain, and ground and screen overlays, on the globe and the map.
- **Network links:** refresh by interval and view, against a mock server, and offline reuse.
- **Time:** features appear and disappear as the clock passes their time stamps and spans, and `gx:Track` positions interpolate.
- **Editing (16b):**
  - each edit, and its undo and redo, leaves the document as expected
  - vertex dragging changes only that vertex
- **Writing (16b):**
  - Exported files validate against the OGC KML 2.2 schema (`xmllint`, schema stored in the tests).
  - GDAL reads every exported file with the same geometry.
  - A round trip of each fixture through Terra Soar keeps every element it doesn't edit.
- **Performance:** the large-file fixture loads within the time and memory budgets and draws within the frame budget.

**Exit criteria**:
- **16a:** the fixture KML from Google Earth, QGIS and the OGC examples displays correctly on the globe and the map.
- **16b:** documents made in Terra Soar export to valid KML and KMZ that other programs open, and round trips lose nothing.

## Phase 13 – Terrain analysis

Goal: answers from the terrain, not just pictures of it.

**Deliverables**

- Slope and aspect layers, computed from the visible elevation sources and coloured by a legend
- Contours at a chosen interval, as a vector layer that can be exported (GeoJSON, GPX)
- Line of sight between two points: visible or not, and where the line is blocked
- Viewshed from a point and height, as an overlay, computed off the GUI thread
- Distance and area measurement along geodesics (also listed under [Ideas](#ideas-not-yet-planned) if this phase is postponed)

**Technical documentation**

- `docs/design/terrain-analysis.md`: algorithms, accuracy and the datum used

**Regression tests**

- Slope and aspect of synthetic surfaces (planes, cones) match analytic values.
- Contours of a fixture DEM match GDAL's `gdal_contour`.
- Line of sight and viewshed on synthetic terrain with known answers.
- Long computations can be cancelled and never block the GUI thread.

**Exit criteria**: every analysis matches its reference within the source's accuracy.

## Phase 14 – Realism

Goal: a globe that looks like the Earth from space, and objects on it.

**Deliverables**

- Atmosphere: a scattering halo around the globe and haze near the horizon, off in software rendering if it misses the budget
- Sun lighting:
  - day and night from the date and time
  - a night-lights layer (NASA Black Marble)
  - terrain shading by the sun
- glTF 2.0 models placed at a position, heading and scale, with a few built-in examples
- View → Date and Time for the sun's position

**Technical documentation**

- `docs/design/lighting.md` and `docs/design/models.md`

**Regression tests**

- The sun's position matches an almanac at reference dates.
- Golden images of day, night, terminator and atmosphere, with lighting off matching the current golden images.
- glTF sample models (Khronos test assets) load and draw correctly.
- Performance budgets hold with atmosphere and lighting on.

**Exit criteria**: lighting is correct for any date, models draw correctly, and the budgets hold.

## Phase 15 – Satellite orbits

Goal: satellites shown where they are, where they have been and where they are going, computed from two-line element sets (TLEs).

**Decisions (2026-11-17)**: SGP4/SDP4 is Terra Soar's own code, written from the published reference (Spacetrack Report #3 as revised by Vallado et al., 2006), not a linked library. This keeps the licence GPL-2.0-or-later. The code has a full test suite of its own (see Regression tests).

**Status (2026-11-20):** delivered (15a to 15e on one branch, one pull request): element sets and SGP4/SDP4 (15a), tracks, passes, the orbit worker and CelesTrak (15b), the running clock (15c), tracks and markers drawn by the GPU (15d), and satellite layers in the program (15e). Design: `docs/design/satellites.md`; for users: `docs/user/satellites.md`. Differences from the plan below:
- Tracks are drawn in the Earth-fixed frame, so in space they lie above the ground track (the inertial orbit would leave it).
- With more than 20 satellites shown, only the selected one draws its tracks unless the layer's properties say all (a whole constellation's tracks hide the globe).
- Passes are found above the layer's footprint elevation (10° by default).
- The clock runs in real time from the start, but ticks only while satellites are shown, so a window with nothing moving stays idle; choosing a time on the time slider pauses it.

**Deliverables**

- **TLE sources:**
  - A `.tle`/`.txt` file, or text pasted in (two-line and three-line forms, with checksums checked)
  - CelesTrak's published groups (stations, GPS, weather, Starlink and so on), and its OMM/JSON form, fetched over HTTP. Fetches go through the disk cache, so the last download works offline.
  - Space-Track, which needs an account, comes with Phase 23's key storage.
- **Orbit propagation with SGP4/SDP4**, the model TLEs are made for. It is our own implementation, in `core/` as its own subsection (`ts/orbits`) with its own unit test project (`tests/orbits`):
  - Positions in TEME, converted to Earth-fixed WGS 84 through sidereal time (polar motion is negligible at this scale)
  - Computed off the GUI thread
- **A satellite layer** in the layer tree for each source, holding a list of satellites.
  - Each satellite can be shown or hidden.
  - A filter by name or catalogue number.
  - Colour, opacity and style kept in the project like any layer.
- **For each shown satellite:**
  - **Current position**: a marker at its altitude on the globe, with its name. On the map, the marker sits at the point below it.
  - **Past track**: from the current time back over a chosen time span, drawn as a solid line fading with age.
  - **Future track**: from the current time forward over its own chosen time span, drawn as a dashed line.
  - **Time spans**: set in minutes or in orbits, per layer, with a per-satellite override. Proposed default: one orbit back and one orbit forward.
  - On the globe, the tracks are drawn in space at the satellite's altitude, with an optional ground track below. On the map they are ground tracks, split cleanly at the antimeridian and the projection's edges.
  - Optionally, the footprint: the area of the Earth that can see the satellite above a chosen elevation angle.
- **Time controls:**
  - Real time by default
  - A clock in the status bar, with pause and play
  - Speed-up and slow-down, and stepping
  - Jump to a date and time
  - The tracks and markers follow the clock, so the past and future spans move with it.
- **Satellite details on click**: name, catalogue number, TLE epoch and age, latitude, longitude, altitude, speed, period, inclination, and the next pass over the point under the pointer.
- **TLE age**: a TLE's accuracy falls over days. Satellites whose TLE is older than a set age (proposed: 14 days) are marked, and the program offers to refresh them.
- **Performance**: a whole constellation (several thousand satellites, e.g. Starlink) can be shown at once within the frame budget. Markers are instanced, and tracks are recomputed only when the clock or the time span moves past a step.

**Technical documentation**

- `docs/design/satellites.md`: TLE and OMM parsing, SGP4/SDP4, the TEME to WGS 84 conversion, time handling, track sampling and the antimeridian, and accuracy limits
- `docs/user/satellites.md`: adding satellites, time spans, the clock

**Regression tests**

- **SGP4/SDP4 test suite** (`tests/orbits`, written with the code):
  - **Verification set**: every case in the published SGP4-VER test set (Vallado et al., "Revisiting Spacetrack Report #3"). Positions and velocities are checked at every listed time against the reference output, to the published tolerance (millimetres in position).
  - **Coverage**: near-Earth (SGP4) and deep-space (SDP4) orbits, 12-hour resonant (GPS, Molniya) and 24-hour geosynchronous orbits, high eccentricity, low perigee, and negative inclination handling.
  - **Errors**: the reference error codes (eccentricity out of range, mean motion negative, satellite decayed), each reported, not returned as a position.
  - **Edge cases**: propagating to the epoch itself, far before and after it, and across year boundaries in the epoch.
  - **Constants**: WGS 72 (the constants TLEs are fitted with) by default, as in the reference.
  - **Independence**: the test data files are stored in `tests/orbits/data`, with their source and licence noted, so the suite needs no network.
- **Frames**: the TEME to Earth-fixed conversion matches reference values at known epochs.
- **Parsing**: valid and invalid TLEs (bad checksum, wrong columns, three-line names), OMM JSON, and a whole CelesTrak group file.
- **Network**: CelesTrak fetches against a mock server, with caching and offline reuse of the last download.
- **Tracks**: the past and future spans cover exactly the chosen times, ground tracks split at the antimeridian, and changing the clock or a span updates them.
- **Golden images**: the ISS and a GPS satellite at a fixed epoch, on the globe and the map, with past and future tracks.
- **Performance**: the frame budget holds with a fixture of several thousand satellites shown.

**Exit criteria**: satellites from a file or CelesTrak show at positions matching the reference propagator, with correct past and future tracks for any chosen spans and clock time, within the frame budget.

## Phase 17 – Aircraft from ADS-B

Goal: ADS-B data imported from files (and, as a second step, from a live receiver) shows aircraft in 3D at their altitude above the globe, with optional trails behind them: none, over a chosen time span, or the whole of the data.

**Status (2026-11-21):** delivered except airplanes.live (17a to 17e on one branch, one pull request): the decoder (17a), the readers, the shared `ts/tracks` store and heights (17b), aircraft markers and timed trails drawn by the GPU (17c), aircraft layers in the program (17d) and live receivers (17e). Design: `docs/design/adsb.md`; for users: `docs/user/aircraft.md`. Differences from the plan below:
- **airplanes.live is left out** until its access and terms are agreed (open question 7); it will reuse the readsb JSON reader and the receivers' polling.
- No aircraft database pack: registrations and types are shown when the data has them (readsb JSON and traces); no suitably licensed database was chosen.
- Live receivers also take raw AVR (port 30002). Reports from a receiver are timed when they arrive, not by its clock. Raw and Beast sessions are recorded as timed raw messages (`time,hex`), which read back as a recording.
- Aircraft are a simple shape (no Phase 14 models yet), sized in pixels. Labels are the callsign (or address), optionally with altitude and speed.
- The altitude and area filters, which change with the clock, hide markers only; trails follow the callsign, address and hidden filters.
- Untimed AVR files (`*…;` with no counter) can be read, but all their messages are placed at the start time; the layer says so.

**Approach (proposed)**: a `core` subsection (`ts/adsb`) with its own unit test project (`tests/adsb`). It has:
- readers for the common ADS-B formats;
- our own Mode S/ADS-B message decoder for raw data, including CPR position decoding;
- a track store holding each aircraft's reports in time order.

Drawing shares Phase 15's machinery for moving objects and tracks, and the program-wide clock. ADS-B's geometric altitude is a height above the WGS 84 ellipsoid, which Terra Soar already uses internally. Barometric altitude is converted through the standard atmosphere and marked as approximate.

**Deliverables**

- **Import**: File → Import ADS-B Data… and drag and drop, in the common formats:
  - SBS-1/BaseStation CSV (the port 30003 format)
  - raw Mode S messages (AVR hex, and Beast binary)
  - dump1090/readsb JSON snapshots and tar1090/readsb trace files
  - CSV with mappable columns (time, ICAO address, latitude, longitude, altitude, and optional heading, speed and callsign)
  - Each import becomes an aircraft layer in the layer tree.
- **Decoder**: CRC checking, aircraft identification (callsign), airborne and surface positions (global and local CPR decoding), barometric and geometric altitude, velocity, vertical rate, squawk and emergency status.
- **Aircraft in 3D:**
  - A marker at the aircraft's position and altitude above the globe, oriented by its track (heading).
  - The marker is a simple aircraft shape scaled to stay visible, or a 3D model if Phase 14's models exist.
  - A label (callsign, or ICAO address), optionally with altitude and speed.
  - On the flat map, a rotated aircraft symbol at the point below it.
  - Optional drop lines from each aircraft to the ground, which make heights readable in 3D.
- **Trails**, per layer with a per-aircraft override:
  - **Off.**
  - **Time span**: the last N minutes (or seconds/hours) before the clock's time.
  - **All data**: every report the aircraft has up to the clock's time.
  - Trails are drawn in 3D at the reported altitudes on the globe, and as ground tracks on the map (split at the antimeridian).
  - Colour by aircraft, by altitude (with a legend) or by vertical rate.
  - Gaps in reception (no reports for longer than a set time) break the trail rather than drawing a straight line across them.
- **Time and playback:**
  - Recordings play on the program-wide clock: play, pause, speed (×1 to ×1000), stepping, and a time slider spanning the recording.
  - Positions are interpolated between reports.
  - An aircraft disappears after a set time without reports.
  - The trail's time span moves with the clock.
- **Live data (second step):**
  - a connection to a local receiver (dump1090/readsb: SBS on port 30003, Beast on port 30005, or its JSON over HTTP), drawn in real time;
  - the session optionally recorded to a file for replay.
  - Online services that need an account come with Phase 23's key and terms handling. OpenSky Network (free, with rate limits and terms) is one candidate.
- **airplanes.live**: worldwide live aircraft from the community network [airplanes.live](https://airplanes.live/api-docs/), through its REST API (`https://api.airplanes.live/v2/`):
  - **Adding it:** an "airplanes.live" entry in the catalogue adds a live aircraft layer with no receiver of your own and no API key.
  - **Area query:** aircraft are fetched for the area in view through the point query (`/point/{lat}/{lon}/{radius}`, radius up to 250 nautical miles), centred on the view. When the view is larger than the radius, the view's centre is used and the layer says so.
  - **Single-aircraft lookups:** the followed or selected aircraft is looked up by its ICAO address (the API guide's example uses `/icao/{hex}`), so it stays tracked when it leaves the area. Lookups by callsign, registration or squawk are used too, if the API offers them. Only the point query is formally documented, so the endpoint list is confirmed with airplanes.live before building.
  - **Rate limit:** at most 1 request per second, enforced by one limiter shared across every airplanes.live layer and lookup. The refresh interval is settable but never faster than that. A `429` or `403` answer backs off and shows the layer's state.
  - **Response:** readsb-style JSON (an `ac` list with `hex`, `flight`, `lat`, `lon`, `alt_baro`, `alt_geom`, `gs`, `track`, `baro_rate`, `squawk` and `seen`), read by the same reader as dump1090/readsb JSON.
  - **Trails:** trails build up from the positions received while the layer is on. The session can be recorded to a file for replay, if airplanes.live's terms allow storing its data.
  - **Terms:**
    - The API is for non-commercial use, with no SLA or uptime guarantee.
    - The data is credited as "airplanes.live" in the view's attributions and the About window.
    - Requests carry a User-Agent naming Terra Soar and its version.
    - The terms are checked again before release and recorded in the catalogue entry, like WMS terms.
  - **Access, to settle before building it:** the API currently answers requests from programs with `403` and asks the developer to contact airplanes.live with a description of the project. Terra Soar needs their agreement first (see [Open questions](#open-questions)).
- **Details on click:** ICAO address, callsign, registration and type (from an optional, downloadable aircraft database pack, if a suitably licensed one exists), altitude (geometric and barometric), ground speed, track, vertical rate, squawk, and the time of the last report.
- **Follow mode:** the camera follows the selected aircraft.
- **Filters:** by callsign, ICAO address, altitude band and area.
- **Export:** a selected aircraft's track exported as KML (`gx:Track`, with Phase 16), GPX or CSV.
- **Performance:**
  - A busy live view (1,000 aircraft) and a full day's recording (several million reports) stay within the frame and memory budgets.
  - Trails are GPU line strips with a time per vertex, so the time span is applied in the shader, and moving the clock rebuilds nothing.
- **Privacy:** ADS-B is a public broadcast, but some operators ask for their aircraft not to be shown (FAA LADD/PIA). When an online service supplies such a block list, it is honoured. Users' own receiver data is theirs to view.

**Technical documentation**

- `docs/design/adsb.md`: the formats read, the decoder (CPR, CRC), altitude types and datum handling, the track store, interpolation, trail drawing and time filtering, and live connections
- `docs/user/aircraft.md`: importing data, trails, playback, live receivers, airplanes.live and its terms

**Regression tests**

- **Decoder:**
  - Known messages (from published Mode S/ADS-B examples, with their licences checked) decode to the published callsigns, positions, altitudes and velocities.
  - Corrupt messages fail the CRC.
- **CPR:**
  - Global and local decoding at latitude zone boundaries, near the poles and across the antimeridian.
  - Odd/even pairs too far apart in time are rejected.
- **Readers:**
  - fixture files in every supported format give the same tracks;
  - malformed lines and files are reported with their line, not fatal.
- **Track store:** reports sorted and deduplicated, interpolation between reports, the stale-aircraft timeout and gap handling.
- **Trails:** off, time span and all-data modes show exactly the reports they should at given clock times, and changing the clock or the span updates them.
- **Heights:** geometric altitude lands at the right ellipsoidal height, and barometric altitude converts by the standard atmosphere.
- **Live:** a mock receiver (local `QTcpServer` sending SBS and Beast streams) drives aircraft in real time, including disconnection and reconnection.
- **airplanes.live:**
  - **Mock server:** a mock of the v2 API (point and ICAO-address queries, stored real responses) checks:
    - the request URLs built for a view and for a followed aircraft;
    - that requests never exceed 1 per second across several layers;
    - back-off on `429` and `403`;
    - that responses are read into tracks;
    - the attribution shown.
  - **Live check:** an optional test against the real API runs only when asked for, not in CI.
- **Golden images:** a fixture recording at a fixed clock time, on the globe in 3D with each trail mode, and on the map.
- **Performance:** the day-long fixture recording loads within the time and memory budgets and plays within the frame budget.

**Exit criteria**: imported recordings in every supported format play back with aircraft at the right positions and altitudes. Each trail mode is correct at any clock time, a local receiver works live, and the budgets hold.

## Phase 18 – Ships from AIS

Goal: AIS (Automatic Identification System) data, from files, a receiver or open online feeds, shows vessels on the map and the globe, with their names, types, headings and trails.

**Status (2026-11-21):** delivered (18a to 18e on one branch, one pull request): sentences and the decoder (18a), readers and vessel tracks (18b), drawing with hulls to scale and course vectors (18c), ship layers in the program (18d), and live receivers and open feeds (18e). Design: `docs/design/ais.md`; for users: `docs/user/ships.md`. Differences from the plan below:
- **Kystverket's stream is not verified live**: it didn't answer from the development machine, so it is tested against a mock server only. Digitraffic is checked live (an optional test).
- **Test sentences** come from pyais's test suite (MIT); the recordings are made up by our own encoder. No ITU example sentences were used.
- **Performance**: 10 000 stations with 1500 trails hold the software frame budget; 20 000 take about 100 ms in software GL (shading the markers' pixels), far less on a GPU.

**Approach (proposed)**: a `core` subsection (`ts/ais`) with its own unit test project (`tests/ais`). It holds our own AIS decoder and readers for the common formats. The track store, trails, playback and live connections are shared with Phase 17 (aircraft) through `ts/tracks`. Vessels are drawn at sea level, so they work the same on the flat map and the globe, and stay on the water surface when terrain is on.

**Deliverables**

- **Import**: File → Import AIS Data… and drag and drop:
  - **NMEA 0183 logs** (`!AIVDM`/`!AIVDO` sentences): multi-sentence messages reassembled, checksums checked.
    - Times come from NMEA 4.0 tag blocks (`\c:` and `\s:`) or a timestamp prefix on each line, or else from the file's position order.
  - **CSV exports**, including NOAA's MarineCadastre AIS data (US public domain, a good source of test and demonstration data), with mappable columns for other CSV.
  - Each import becomes a vessel layer in the layer tree.
- **Decoder** (ITU-R M.1371), covering:
  - Class A position reports (types 1–3), with navigational status, rate of turn, speed and course over ground, and true heading;
  - base stations (type 4);
  - static and voyage data (type 5): name, callsign, IMO number, ship type, dimensions, draught, destination and ETA;
  - Class B positions and static data (types 18, 19 and 24);
  - search-and-rescue aircraft (type 9, drawn at their altitude);
  - aids to navigation (type 21);
  - long-range reports (type 27).
  - Other messages are counted and listed, not errors.
- **Vessels on the map and globe:**
  - **Symbol:** a vessel symbol at its position, pointing along its true heading, or its course when heading is not available. Coloured by ship type with the usual AIS colour scheme, with a legend.
  - **Hull outline:** at close zoom, the symbol becomes the hull outline drawn to scale from the reported dimensions and the antenna's position on board.
  - **Course vector:** an optional line showing where the vessel will be in N minutes at its speed and course.
  - **Label:** name (or MMSI), optionally with speed and destination.
  - **Status:** moored and anchored vessels are shown differently from those under way.
  - **Other symbols:** aids to navigation and base stations have their own.
- **Trails**: the same three modes as Phase 17 (off, a time span before the clock's time, or all data), per layer with a per-vessel override. They are coloured by vessel, speed or type, and broken at gaps in reception.
- **Time and playback:**
  - Recordings play on the program-wide clock, with interpolation between reports.
  - Vessels fade out after a time without reports. Class A and Class B report at different rates, so the timeout depends on the class and the vessel's speed.
- **Live data:**
  - **Local receiver:** NMEA sentences over TCP or UDP from AIS-catcher, rtl-ais or a ship's AIS transponder, optionally recorded to a file for replay.
  - **Open online feeds:**
    - Fintraffic Digitraffic (Finland, Baltic; CC BY 4.0)
    - the Norwegian Coastal Administration's open AIS (NLOD licence)
    - other open national feeds found later
    - Each is a catalogue entry with its terms and attribution, like WMS services, and the terms are confirmed before release.
  - **Services that need a key** (e.g. aisstream.io) come with Phase 23.
- **Details on click:**
  - MMSI, name, callsign and IMO number;
  - ship type, dimensions and draught;
  - destination and ETA;
  - navigational status;
  - speed, course, heading and rate of turn;
  - the time of the last position and static reports.
- **Follow mode, and filters** by name or MMSI, ship type, navigational status, speed and area.
- **Export:** a vessel's track as KML (with Phase 16), GPX or CSV.
- **Performance:**
  - A live national feed (tens of thousands of vessels) and a day of NOAA data (millions of reports) stay within the frame and memory budgets.
  - Symbols are instanced, and trails are time-filtered in the shader, as in Phase 17.

**Technical documentation**

- `docs/design/ais.md`: the formats read, the decoder (6-bit payloads, fragments, message types), time sources, symbol orientation and hull outlines, and live connections
- `docs/user/ships.md`: importing AIS data, symbols and colours, trails, playback, receivers and online feeds

**Regression tests**

- **Decoder:**
  - every supported message type decodes known sentences to their published values (from ITU-R M.1371's examples and openly licensed AIS test sets, with their licences checked);
  - "not available" values (heading 511, speed 1023 and so on) are reported as missing, not as numbers.
- **Sentences:**
  - multi-sentence reassembly, including interleaved and out-of-order fragments;
  - checksum failures, truncated payloads, and fill bits.
- **Readers:**
  - NMEA logs with tag-block times, prefixed times and no times;
  - a NOAA MarineCadastre CSV fixture;
  - malformed lines reported with their line, not fatal.
- **Tracks and trails:**
  - interpolation, the timeouts by class and speed, gap handling;
  - each trail mode at given clock times (shared tests with Phase 17).
- **Symbols:** heading versus course fallback, and hull outlines placed and sized correctly from the reported dimensions.
- **Live:**
  - a mock receiver (local `QTcpServer` and UDP socket sending NMEA) drives vessels in real time, including reconnection;
  - mock Digitraffic and Norwegian feeds check the request handling, attribution and back-off.
- **Golden images:** a fixture harbour recording at a fixed clock time, on the map and the globe, with type colours, course vectors, hull outlines and each trail mode.
- **Performance:** the day-long NOAA fixture loads within the time and memory budgets and plays within the frame budget.

**Exit criteria**: AIS logs and CSV play back with vessels at the right positions, headings and types. Each trail mode is correct at any clock time, a local receiver and at least one open online feed work live, and the budgets hold.

## Phase 19 – Vector layer colours

Goal: users can change the colours of the graticule and shoreline layers to suit the imagery beneath them, their eyes or a screenshot.

**Already in place**: every vector layer already has a style in its layer definition: fill colour, outline colour (with alpha) and outline width (`VectorStyle`, the layer's `"style"` object). It is saved in the project, and the graticule draws with the outline colour and width. What's missing is a way for users to change it.

**Deliverables**

- **Layer Style… dialog**, opened from the layer tree's context menu, by double-clicking a vector layer, or from Layer → Style…:
  - **Graticule:** line colour, line opacity and line width.
  - **Shoreline:** outline colour, opacity and width, and, for the filled shoreline, the land fill colour and opacity.
  - **Colour picker:** the system colour dialog with alpha, plus a row of recent colours.
  - **Live preview:** changes show on the globe and the map as they are made. OK keeps them, Cancel restores the previous style, and Reset returns the layer to its built-in default.
- **Presets**: a few named styles for common needs:
  - **Default.**
  - **Light on dark imagery:** today's look, for the Blue Marble.
  - **Dark on light imagery:** for pale WMS maps.
  - **High contrast:** for accessibility, ready for Phase 25's high-contrast check.
  - **Print:** black lines, no fill.
- **Saved with the project**: the style is part of the layer definition, so projects keep it, as they already keep terrain exaggeration and hillshade.
- **New-project default**: "Use as default for new projects", saved in the user's settings, so the user's preferred colours apply without restyling every project.
- **Undo**: a style change can be undone (Edit → Undo) if the program gains an undo stack by then (Phase 16b brings one), or else through Reset.
- **For every vector layer**: the dialog works on any vector layer, so Phase 22's files, Phase 16's KML (where a document doesn't set its own styles) and Phase 13's contours reuse it.
- **Layer list**: a small colour swatch beside each vector layer's name shows its current colours.

**Technical documentation**

- `docs/user/layers.md`: changing a layer's colours, presets and the default
- `docs/design/layer-model.md`: the style object, defaults and how the user default is applied to new projects

**Regression tests**

- **Style model:**
  - Every style value round-trips through the layer definition JSON and the project file.
  - Invalid or missing colours fall back to the layer's default.
- **Dialog (UI test):**
  - Changing each colour, opacity and width updates the layer definition, and the view redraws.
  - Cancel restores the previous style, and Reset restores the built-in default.
  - Each preset applies its values.
- **Rendering:** golden images of the graticule and the filled and outline shoreline in non-default colours and widths, on the globe and the map, with the default style still matching the existing golden images.
- **Defaults:** "Use as default" applies to a new project and doesn't change existing projects.

**Exit criteria**: the graticule and shoreline colours, opacity and widths can be changed, previewed, reset, saved with the project and set as defaults, and every existing golden image is unchanged at the default style.

## Phase 20 – On-screen navigation controls

Goal: an overlay of controls on the view (the globe and the flat map) to rotate, pan, zoom and tilt with the mouse or a touch screen, without needing to know the mouse buttons and keys.

**Status (2026-11-21):** delivered (20a to 20e on one branch, one pull request): the flat map turns and tilts (20a), tile selection by distance with the far and tilt limits (20b), relief, haze and sky on a tilted map (20c), the navigation controls with View → Navigation Controls (20d), and the docs and tests (20e). Design: `docs/design/camera-and-precision.md`, `docs/design/tiling.md`, `docs/design/morph.md`; for users: `docs/user/getting-around.md`. Differences from the plan below:
- **Going to the map** still starts north-up and straight down (the default); the map is turned and tilted once there, and going back to the globe keeps its heading and tilt.
- **Golden images of the overlay** are not stored: its symbols and the "N" are drawn with the system's fonts and anti-aliasing, which differ between Linux and Windows. Its colours are checked for 3:1 contrast analytically over the listed backgrounds and the pixels of a busy image, its placement in each corner and its sizes by UI tests, and screenshots are saved with `TS_UI_SCREENSHOTS` for review. The turned and tilted map has golden images.
- **Picking on a map in relief** finds the point on the plane, not on the mountainside; markers and labels stay on the plane.
- The flat map's far limit is in eye heights (40, at most 10 000 km) rather than derived from each quality tier, which don't exist yet.

**Decisions (2026-11-17)**: the flat map can be turned and tilted, like the globe. The rotate and tilt controls work on both views, and north-up and straight down stay the defaults.

**Already in place**: the camera already moves in all of these ways from the keyboard. `CameraController` has motions for moving, zooming, turning and tilting, with inertia and smooth flights. The view already draws overlays (the scale bar and the attributions). On the flat map, though, turning and tilting are not possible today ("a map isn't turned or tilted"). This phase adds them there, as decided (see below).

**Deliverables**

- **The overlay**: a compact group of controls in a corner of the view.
  - **Placement:** top right by default, clear of the attributions (bottom right) and the scale bar. It can be moved to another corner.
  - **Look:** the controls are semi-transparent until the pointer is over them, never below a minimum opacity that keeps them readable (see below).
  - **Show and hide, from the View menu**: **View → Navigation Controls** is a checkable item beside View → Scale Bar and View → Attributions, and works like them:
    - checked shows the overlay, unchecked hides it completely, and its clicks then go to the view;
    - shown by default;
    - the choice is saved in the user's settings and restored at the next start;
    - it works in both the globe and the flat map, and stays as set when switching between them.
  - **Readable on light and dark backgrounds**: the view can be anything from snow, desert and pale WMS maps to dark ocean, night imagery and the black of space, so the controls never rely on the background for contrast:
    - each control sits on its own backing plate, a translucent dark disc or panel with a thin light edge;
    - its symbols are light, with a dark outline (halo);
    - so one of plate edge, symbol or halo always stands out, whatever is behind it;
    - the hover and pressed states change the plate's brightness, not just its transparency, so they show on any background;
    - The system's high-contrast setting, and Phase 25's high-contrast mode once it exists, switch to opaque plates and the system's high-contrast colours.
  - **The controls:**
    - **Rotate**: a compass ring showing north. Dragging the ring turns the view, the arrows on either side turn it while held, and clicking the "N" turns it back to north-up with a smooth flight.
    - **Pan**: four arrows (or a small joystick) that move the view while held, accelerating the longer they are held. A click moves by a set fraction of the view.
    - **Zoom**: + and − buttons, and a slider showing the current height (eye height on the globe, scale on the map). Clicking or dragging the slider flies to that height.
    - **Tilt**: up and down buttons, and a small arc slider from straight down to the horizon limit, showing the current tilt. Double-clicking it returns to straight down.
  - **Reset**: a home button, the same as View → Reset View.
- **Behaviour:**
  - The controls drive the existing `CameraController` motions, so they share their speeds, inertia, limits and terrain clearance.
  - Holding a button is the same as holding the matching key.
  - Clicks on the overlay don't reach the globe, and clicks elsewhere aren't affected.
- **On the flat map:**
  - **Rotation**: the map can be turned away from north-up, as the globe can. The graticule, labels, picking, the scale bar and region drawing follow the rotation.
  - **Tilt**: a perspective view of the flat map, looking towards the horizon. Terrain draws in relief when it is on.
  - Both are smooth through the 3D/2D morph, and "N" or Reset View returns the map to north-up and straight down.
  - Both work in every map projection: the original three, and all of Phase 11's (azimuthal, conic and others).
  - The right mouse button, and the turn and tilt keys, then work on the map as on the globe.
- **Sizes and input:**
  - The controls scale with the screen's DPI and the system font size.
  - They are large enough for touch screens (at least 9 mm targets).
  - They respond to touch as well as the mouse.
- **Accessibility:**
  - Each control has a tooltip, an accessible name, and its keyboard shortcut in the tooltip.
  - The overlay can be reached from the keyboard (Tab into the view, then arrow keys and Enter), ready for Phase 25's accessibility work.
- **Performance:** the overlay costs nothing measurable when idle, and the view still stops redrawing when nothing moves.
- **Tile detail by distance from the camera, and performance when tilted:** a tilted view, especially near the horizon, shows far more of the Earth than a view straight down. Detail must therefore fall off with distance, or the tile count, downloads and frame time grow without limit.
  - **On the globe:** this already holds. A tile's screen-space error uses the distance from the eye to the tile (`docs/design/tiling.md`), so distant tiles are coarser. This phase checks it at the new tilt limits and keeps it.
  - **On the flat map:** today a tile's error depends only on its latitude and the zoom, not on distance. That is right for a map seen straight down, but wrong once the map tilts. The map's tile selection changes to the same perspective screen-space error as the globe: the tile's texel size projected through the camera at its distance from the eye. Near tiles get fine detail, and tiles towards the horizon get coarser levels. At no tilt the result equals today's selection, so existing views and golden images don't change.
  - **The horizon on a tilted map:** a tilted flat map never reaches a horizon of its own, so it needs limits:
    - a far limit, past which nothing is drawn, set so that the coarsest level still meets the error limit there;
    - a tilt limit, so the view can't look along the map's surface;
    - a haze or sky band at the far limit, shared with Phase 14's atmosphere if that exists, to fade the edge rather than cut it.
  - **Terrain:** height meshes use the same distance-based levels, so distant terrain uses coarser meshes. Skirts keep tiles of different levels crack-free.
  - **Loading order:** requests are ordered by screen-space error and then by distance from the camera, so near tiles load first. Tiles beyond the far limit are never requested, including from WMS, WMTS and web terrain servers.
  - **Budgets:**
    - **Frame time:** a fully tilted view of the globe and of the flat map, at the tilt limit over a detailed area with terrain on, stays within the frame-rate budgets on the reference PC and under software rendering.
    - **Tile counts:** the number of tiles drawn stays within a fixed limit, measured and recorded in `docs/design/tiling.md` with the existing selection budgets.
    - **GPU memory:** the GPU tile budget (96 MB) is unchanged.
    - **Lower quality tiers:** they raise the error limit, which also shortens the far limit.

**Technical documentation**

- `docs/design/camera-and-precision.md`: the controls' mapping to camera motions, and the flat map's new rotation and tilt
- `docs/design/tiling.md`: the flat map's distance-based screen-space error, the far and tilt limits, request ordering, and the tilted-view budgets
- `docs/design/morph.md`: rotation and tilt through the morph
- `docs/user/getting-around.md`: the navigation controls, and turning and tilting the map

**Regression tests**

- **Each control (UI test with simulated mouse and touch events):**
  - rotate, pan, zoom and tilt move the camera in the expected direction and by the expected amount;
  - holding a button keeps moving and releasing it stops, with inertia;
  - "N" returns the heading to 0, and the home button returns to the start view.
- **Limits:**
  - zoom and tilt stop at the camera's limits;
  - terrain clearance still holds;
  - the sliders show the camera's current state after moving by mouse, keys or controls.
- **Hit-testing:** clicks on the overlay don't move or pick on the globe, and clicks beside it do.
- **Flat map rotation and tilt:**
  - picking, the graticule and the scale bar are correct on a turned and tilted map;
  - the morph is continuous from a turned, tilted globe to the map and back;
  - north-up and straight down remain the default.
- **View menu:**
  - View → Navigation Controls is checkable, sits beside Scale Bar and Attributions, and is checked by default;
  - unchecking it hides the overlay on the globe and the map, and clicks where it was reach the view;
  - the setting, and the overlay's corner, are saved and restored across a restart.
- **Contrast on light and dark backgrounds:** the overlay is drawn over solid white, solid black, mid-grey and busy imagery (Blue Marble cloud and desert, a pale WMS fixture). For each control, the symbol's contrast against what surrounds it meets WCAG 2.1's non-text contrast (at least 3:1), in the normal, hover, pressed and least-opaque states.
- **Golden images:** the overlay in each corner, at normal and high DPI, over light and dark backgrounds, in high-contrast mode, and a turned and tilted flat map.
- **Idle:** with the overlay shown and nothing moving, the view stops drawing.
- **Tile selection by distance (`tests/tiling`):**
  - for tilted cameras on the globe and the flat map (in each projection, Phase 11's included), every drawn tile's screen-space error, computed with its distance from the eye, is within the limit;
  - along a ray from the camera to the horizon, tile levels never get finer with distance;
  - every visible pixel is covered;
  - fixed tilted cameras select fixed tile sets;
  - with no tilt, the flat map's selection is identical to today's.
- **Far limit:** on a tilted map, no tile beyond the far limit is selected or requested, from any provider (checked with the mock WMS and tile servers).
- **Request order:** near tiles are requested before far ones in a tilted view.
- **Performance (`tests/perf`):**
  - frame time, tiles drawn, requests and GPU memory at the tilt limit, on the globe and the flat map, with and without terrain, against the budgets on the reference PC and under llvmpipe;
  - a tilted-map selection time added to the measured budgets beside the existing tilted city view.

**Exit criteria**: every control works by mouse, touch and keyboard on the globe and the flat map, and stays readable (3:1 contrast) on light and dark backgrounds. Tile detail falls off with distance from the camera on both views, and tilted views stay within the frame, tile and memory budgets. View → Navigation Controls shows and hides the overlay and is remembered. The flat map can be turned and tilted, the overlay can be moved, and the idle and frame budgets hold.

## Phase 21 – VMAP0 data pack

Goal: NGA's Vector Map Level 0 (VMAP0), a world vector map at 1:1,000,000, as downloadable data packs. The packs are prepared with the packager, tested like the existing packs, and installed from the Data Packs window.

**About the source**: VMAP0 (edition 5) covers the world in four libraries:
- `noamer`: North America;
- `eurnasia`: Europe and North Asia;
- `soamafr`: South America and Africa;
- `sasaus`: South Asia and Australia.

They are published in the Vector Product Format (VPF, MIL-STD-2407). Its themes are:
- boundaries
- elevation (contours and spot heights)
- hydrography (rivers, lakes, coastlines)
- industry
- physiography
- population (built-up areas and place names)
- transportation (roads, railways, airports)
- utilities
- vegetation
- data quality

It is a US Government (NGA) product approved for public release. The exact terms of the edition used are confirmed and recorded before publishing, as they were for every existing pack (see open question 9).

**Approach (proposed)**:
- **Packager only:** the work is in the packager (`tools/packager`), not the program. The program already installs packs and draws vector layers.
- **Reading VPF:** the packager reads VPF itself. GDAL's VPF support needs the OGDI library, which isn't in our dependencies, and VPF is fully specified.
  - It reads the tables, the feature classes, and the primitives (edges, faces and nodes) with their topology, joining features to their attributes.
  - The reader is a packager subsection with its own unit tests.
- **Converting:** the packager converts VMAP0 into the packs' existing vector tile format, with simplification per level (as for the GSHHG shoreline), clipping at tile edges and attributes kept for picking.

**Deliverables**

- **Source acquisition, recorded:**
  - the edition, where it was downloaded from, the size and SHA-256 of every file;
  - a script that fetches and verifies them, as for the ETOPO and geoid sources;
  - the source isn't stored in the repository.
- **VPF reader** in the packager:
  - the database, library and coverage structure;
  - the feature and attribute tables, and the thematic indexes;
  - edge, face and node primitives, with the winged-edge topology turned into polygons with holes;
  - tiled libraries (VMAP0's 15° tiles) and features crossing tile edges joined;
  - the library reference coverage (`libref`) and the data quality coverage.
- **Packs, one per theme group**, so users download only what they want, each with its own layer collection and default styles:
  - **`vmap0-boundaries`**: international and administrative boundaries, with disputed boundaries styled differently.
  - **`vmap0-hydrography`**: rivers (perennial and intermittent), lakes, reservoirs, canals and coastlines.
  - **`vmap0-transportation`**: roads by class, railways, airports and ferry routes.
  - **`vmap0-population`**: built-up areas, and place names once labels exist.
  - **`vmap0-elevation`**: contours and spot heights (where Terra Soar's own DEM and Phase 13's contours don't already cover the need).
  - **`vmap0-physiography`, `vmap0-vegetation`, `vmap0-industry` and `vmap0-utilities`**: smaller packs, or one "other themes" pack, decided by size.
  - **Pack index:** each has a manifest, licence and attribution ("Vector Map Level 0, NGA"), and a version (`vmap0-<theme>-5.<n>`), in the pack index in `marutan/terrasoar-packs`.
- **Sizes:** measured per pack and recorded in `docs/data/`, with a target set before building (VMAP0 is about 1 GB as VPF). A pack too big to download comfortably is split by region along VMAP0's libraries.
- **Styles:**
  - default styles per feature class, roughly following VMAP0's own symbology and readable over Blue Marble and pale WMS maps;
  - restyled through Phase 19's dialog if that exists;
  - zoom ranges per class, so the globe isn't cluttered at a whole-Earth view.
- **Attributes:** clicking a feature shows its feature class and attributes (names, road class, boundary type and so on), decoded from VMAP0's codes to readable text using its value description tables.
- **Notices:** VMAP0 added to `THIRD_PARTY_NOTICES`, the About window's data list and `check_notices.py`'s audit.
- **Publishing:** the packs are built with the packager and checked by its build checks and report. They are published to `marutan/terrasoar-packs` only with the project owner's approval, as for every pack so far. Each is then installed live from the published index and checked.

**Technical documentation**

- `docs/data/vmap0.md`: the source and edition, its terms, the themes included and left out, the conversion, simplification levels, sizes and known data issues (VMAP0's age and known errors)
- `docs/tools/packager.md`: the VPF reader and the `vmap0-*` pack configurations
- `docs/user/offline-use.md`: the VMAP0 packs in the Data Packs window

**Regression tests**

- **VPF reader (`tests/packager`):**
  - Fixtures are a small synthetic VPF database, written for the tests, plus a clipped real VMAP0 tile (public release, stored with its source noted).
  - Table parsing (all column types, variable-length fields, both byte orders), feature-to-primitive joins, and polygons with holes built from the winged-edge topology.
  - Tile-crossing features are joined.
  - Malformed tables are reported with their file and row.
- **Conversion:**
  - Feature counts per theme and feature class match the source.
  - Geometries are valid (no self-intersections after simplification), and simplification stays within its tolerance per level.
  - Tile clipping leaves no gaps or overlaps.
  - Attribute codes decode to the right text.
- **Packs:**
  - each manifest validates;
  - the packager's build checks and report pass;
  - the pack installs, upgrades and uninstalls through the pack manager against the mock pack server (as the existing pack tests do).
- **Rendering:**
  - golden images of each theme over Blue Marble, on the globe and the map, at a whole-Earth view and a regional view;
  - boundaries lined up with the GSHHG shoreline within VMAP0's stated accuracy.
- **Performance:** all VMAP0 packs shown at once stay within the frame and memory budgets.
- **Live check, after publishing:** each published pack downloads, verifies its checksum and installs from the real index (a manual step, as for the geoid packs, not in CI).

**Exit criteria**: the VMAP0 packs build from the recorded source with every check passing. They are tested, published with approval and installable from the Data Packs window, and draw correctly and within budget on the globe and the map.

## Phase 22 – Local data files

Goal: users can open their own vector and imagery files, as they already can DEMs.

**Deliverables**

- Vector files through GDAL/OGR (already linked):
  - GeoJSON, ESRI shapefile, GPX and GeoPackage (KML and KMZ have their own phase, 16)
  - Any CRS, reprojected with PROJ
  - Drawn with the existing vector renderer, draped on terrain
- A simple style per layer (line colour and width, fill, point symbol), kept in the project file.
- Feature picking: clicking a feature shows its attributes.
- GeoTIFF and other raster imagery through GDAL, tiled on the fly into the EPSG:4326 quadtree, with overviews used as for DEMs.
- File → Add Data File… with drag and drop onto the globe.

**Technical documentation**

- `docs/design/vector-files.md`: reading, tiling and styling local vectors
- `docs/user/layers.md`: opening files

**Regression tests**

- Each format reads a fixture file, and feature counts, geometry and attributes match.
- Reprojection of a fixture in British National Grid lands on known positions.
- Golden images of styled fixtures on the globe and the map.
- Picking returns the right feature at known screen positions.
- Large-file budget: a 100 MB shapefile opens and draws within the memory budget.

**Exit criteria**: every listed format opens, draws, styles and picks correctly, within the budgets.

## Phase 23 – Commercial sources

Goal: sources that need an account, without putting keys at risk or breaking their terms.

**Deliverables**

- API keys stored in the system keychain (Windows Credential Manager, Secret Service on Linux), never in project files
- Catalogue entries for services that need a key, which ask for it when first used
- Each service's terms enforced in code where they set rules: attribution, caching limits, and whether offline regions are allowed

**Technical documentation**

- `docs/design/api-keys.md`: storage, per-service rules

**Regression tests**

- Keys are never written to project files, logs or the state report.
- Caching and offline rules are enforced per service (mock servers).

**Exit criteria**: at least one commercial source works end to end with its terms respected.

## Phase 24 – Rendering backend

Goal: a modern graphics API where OpenGL 3.3 is weak, behind the existing backend abstraction.

**Deliverables**

- A Qt 6 RHI backend: Vulkan on Linux and Direct3D 11 on Windows, chosen at start with OpenGL as a fallback
- Shaders written once and converted with Qt Shader Tools
- Qt 5 builds keep the OpenGL backend

**Technical documentation**

- `docs/design/renderer-backend.md`: the RHI backend and how the backend is chosen

**Regression tests**

- The full golden-image suite passes on each backend, within the existing tolerances.
- Backend selection and fallback when a backend fails to start.

**Exit criteria**: the RHI backend matches OpenGL's output and is at least as fast on the reference PC.

**Note**: this phase overlaps the question of when Qt 5 support ends (see [Open questions](#open-questions)).

## Phase 25 – Road to 1.0

Goal: Terra Soar 1.0, a release users can trust to install, update and use in their language.

**Deliverables**

- **Windows code signing.** Buy a certificate and add it as the `WINDOWS_SIGNING_CERTIFICATE`/`WINDOWS_SIGNING_PASSWORD` secrets. The release workflow's signing step already exists and switches itself on. Without a signature, SmartScreen warns on every download.
- **Flathub.** Submit the existing Flatpak manifest (`io.github.marutan.TerraSoar`) to Flathub, so Linux users get updates through their software centre.
- **Translations.** The first languages beyond English, chosen by who can review them, through Qt Linguist. The `.ts` workflow and the pseudo-translation test already exist.
- **Accessibility.**
  - Every action reachable from the keyboard.
  - Accessible names on custom widgets: the globe view, the profile chart and the layer tree.
  - A high-contrast check.
  - Text that scales with the system font size.
- **Update notice.** The program optionally checks the GitHub releases feed once a day and says when a newer version exists. It is off unless the user agrees, and it never downloads anything itself.
- **Fix the bugs reported against 0.0.x** and finish the user guide.

**Technical documentation**

- `docs/release/process.md`: signing with a real certificate, and Flathub updates
- `docs/translating.md`: how to add or update a language

**Regression tests**

- The signed installer's signature is verified in CI (`signtool verify`).
- Every translation loads, and no user-visible string is untranslated (extending the pseudo-translation test).
- Accessibility: every menu action has a shortcut or is reachable by the keyboard, and custom widgets expose names (Qt Test).
- The update check runs against a mock releases feed and is silent when the user hasn't agreed to it.

**Exit criteria**: a signed 1.0 is published, the Flathub listing is live, and at least one translation is complete.

## Items deferred from 0.0.1

These came up during Phases 6–8 and were left out on purpose. They are small enough to fit into any phase.

- **EGM2008 1′ geoid pack.** It isn't published on the PROJ CDN (about 1 GB from NGA), and it differs from the 2.5′ pack by centimetres outside high mountains. Build it if users ask.
- **Code signing and Flathub**: Phase 25.
- **Coverage report per subsection.** The first roadmap set an 80% line target for `core/`. Publish the numbers from CI and close the gaps.

## Ideas (not yet planned)

Candidates to discuss, with no phase or commitment:

- Bookmarks and saved views, with a fly-through tour between them
- Place search (geocoding), online through a service like Nominatim and offline through a downloadable gazetteer pack
- Distance and area measurement (if Phase 13 is postponed)
- Export the view as an image at a chosen size, and printing
- More downloadable packs: higher-resolution regional imagery and DEMs where licences allow
- A command-line or scripting interface (e.g. `--open`, `--fly-to`, render to file) for automation
- Annotations: user-placed points, lines and labels, saved in the project

## Open questions

These need a decision before the phases they affect:

1. **Phase order and scope**: is the order above right, and is every phase wanted?
2. **1.0 criteria**: with the road to 1.0 (Phase 25) as the last phase, 1.0 includes every phase before it. Is that the intent, or should 1.0 come sooner, with some phases after it?
3. **Qt 5 support**: Qt 5.15 is past its open-source end of life. When does Terra Soar drop Qt 5? Dropping it simplifies Phase 24.
4. **Translations**: which languages first, and who reviews them?
5. **Update notice**: is a version check (opt-in, no automatic download) wanted at all?
6. ~~**SGP4 implementation** (Phase 15)~~: decided 2026-11-17. Terra Soar writes its own, with a full test suite (see Phase 15's Decisions).
7. **airplanes.live access** (Phase 17): its API asks programs to contact airplanes.live (contact@airplanes.live) with a description of the project before use. Contact them and confirm their terms for an open-source desktop program: request rate, storing recordings and attribution. Only the project's owner can do this. Phase 17 was delivered without it (2026-11-21); it is built once this is settled.
8. ~~**Turning and tilting the flat map** (Phase 20)~~: decided 2026-11-17. The flat map turns and tilts (see Phase 20's Decisions).
9. **VMAP0 terms and hosting** (Phase 21): confirm the release terms of the VMAP0 edition used (public release, any NGA disclaimers on boundaries and names), and approve the pack sizes before publishing to `marutan/terrasoar-packs`.
