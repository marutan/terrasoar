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
| 9 | Road to 1.0 | Code signing, Flathub, translations, accessibility, 1.0 release |
| 10 | Local data files | Vector files (GeoJSON, shapefile, GPX, GeoPackage) and GeoTIFF imagery |
| 11 | More web services | XYZ/TMS, WFS; WMS TIME and GetFeatureInfo |
| 12 | More projections | Plate Carrée, UTM/British National Grid, LCC, polar stereographic, world projections |
| 13 | Terrain analysis | Slope/aspect, contours, line of sight, viewshed |
| 14 | Realism | Atmosphere, sun lighting, glTF models |
| 15 | Rendering backend | Qt 6 RHI (Vulkan, Direct3D 11) |
| 16 | Commercial sources | API-key services, key storage, terms handling |
| 17 | User guide in the program | The `docs/user` guide in a window from the Help menu |
| 18 | Satellite orbits | Satellites from TLEs, with past and future tracks over chosen time spans |
| 19 | KML | 19a: import KML/KMZ and draw it on the globe and map; 19b: create KML, add elements, export |
| 20 | WMTS | WMTS layers added, browsed, cached and taken offline like WMS layers |
| 21 | Aircraft from ADS-B | Recorded (and live) ADS-B data as aircraft in 3D above the globe, with trails: off, a time span, or all |

The phases are mostly independent. Phase 9 should come first because it finishes the release work. Phases 10 and 11 share the vector pipeline, so 10 should come before 11's WFS. Phase 14's lighting is easier after Phase 15, if both are wanted. Phase 17 depends on nothing else and could be done at any time, including before 1.0. Phase 18 needs a program-wide clock, which Phase 11's WMS time slider and Phase 14's sun position also need. Whichever of those phases comes first should build the clock for all three. Phase 19 draws through Phase 10's vector pipeline (feature styles, picking, draping), so it comes after Phase 10. KML's time elements use the same clock. Phase 20 (WMTS) reuses the WMS machinery and needs nothing else, so it can come at any time. If it comes before Phase 11, Phase 11's XYZ tiles reuse its Web Mercator reprojection. Phase 21 (aircraft) plays recordings on the program-wide clock and shares Phase 18's way of drawing moving objects and their tracks, so it is easiest after Phase 18. It can use Phase 14's 3D models if they exist, and falls back to simple shapes if not.

## Phase 9 – Road to 1.0

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

## Phase 10 – Local data files

Goal: users can open their own vector and imagery files, as they already can DEMs.

**Deliverables**

- Vector files through GDAL/OGR (already linked):
  - GeoJSON, ESRI shapefile, GPX and GeoPackage (KML and KMZ have their own phase, 19)
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

## Phase 11 – More web services

Goal: the common web map services beyond WMS.

**Deliverables**

- XYZ/TMS tile services (`{z}/{x}/{y}` templates), reprojected from Web Mercator onto the quadtree, through `CachedTileFetcher` (disk cache, revalidation, offline regions).
- (WMTS has its own phase, 20.)
- WFS 2.0: features in the visible area, with paging, into the Phase 10 vector pipeline.
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

## Phase 12 – More projections

Goal: the flat map in the projections people need for regional and world maps.

**Deliverables**

- Plate Carrée
- Transverse Mercator, with UTM zones and the British National Grid
- Lambert Conformal Conic
- Polar stereographic (north and south)
- World maps: Robinson, Mollweide, Winkel Tripel
- The morph and hillshade working in each. Non-cylindrical projections need the morph to handle curved outlines and interrupted edges.
- A graticule for projected grids (UTM, BNG) with coordinate readout in that grid

**Technical documentation**

- `docs/design/projections.md`: each projection, its domain, and how the morph handles it

**Regression tests**

- Forward and inverse transforms against PROJ at reference points, for each projection.
- Golden images of the world and a regional map in each projection.
- The morph is continuous (no frame jumps) for each projection.

**Exit criteria**: every listed projection draws, morphs and reads coordinates correctly.

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

## Phase 15 – Rendering backend

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

## Phase 16 – Commercial sources

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

## Phase 17 – User guide in the program

Goal: the user guide in `docs/user/` can be read inside Terra Soar, offline, from the Help menu, so it matches the installed version.

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

## Phase 18 – Satellite orbits

Goal: satellites shown where they are, where they have been and where they are going, computed from two-line element sets (TLEs).

**Decisions (2026-11-17)**: SGP4/SDP4 is Terra Soar's own code, written from the published reference (Spacetrack Report #3 as revised by Vallado et al., 2006), not a linked library. This keeps the licence GPL-2.0-or-later. The code has a full test suite of its own (see Regression tests).

**Deliverables**

- **TLE sources:**
  - A `.tle`/`.txt` file, or text pasted in (two-line and three-line forms, with checksums checked)
  - CelesTrak's published groups (stations, GPS, weather, Starlink and so on), and its OMM/JSON form, fetched over HTTP. Fetches go through the disk cache, so the last download works offline.
  - Space-Track, which needs an account, comes with Phase 16's key storage.
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

## Phase 19 – KML

Goal: Terra Soar reads KML and KMZ as faithfully as the programs people make them in, and can create, edit and export KML of its own. Phase 19a (import and display) is useful alone. Phase 19b (authoring and export) builds on it.

**Approach (proposed)**: our own KML 2.2 reader and writer on `QXmlStreamReader`/`QXmlStreamWriter`, in a `core` subsection (`ts/kml`) with its own unit test project (`tests/kml`).
- GDAL's KML drivers flatten documents into layers and lose most styling and structure, which both display and round-tripping need.
- KMZ archives are read and written through GDAL's zip support (`/vsizip/`, `CPLCreateZip`), which is already linked.
- The `gx:` extensions that Google Earth writes are read where they matter (`gx:Track`, `gx:altitudeMode`).

### Phase 19a – Import and display

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
- **On the map:** everything draws on the flat map as on the globe, including split at the antimeridian, through Phase 10's vector pipeline.

### Phase 19b – Create, edit and export

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
- **Editing (19b):**
  - each edit, and its undo and redo, leaves the document as expected
  - vertex dragging changes only that vertex
- **Writing (19b):**
  - Exported files validate against the OGC KML 2.2 schema (`xmllint`, schema stored in the tests).
  - GDAL reads every exported file with the same geometry.
  - A round trip of each fixture through Terra Soar keeps every element it doesn't edit.
- **Performance:** the large-file fixture loads within the time and memory budgets and draws within the frame budget.

**Exit criteria**:
- **19a:** the fixture KML from Google Earth, QGIS and the OGC examples displays correctly on the globe and the map.
- **19b:** documents made in Terra Soar export to valid KML and KMZ that other programs open, and round trips lose nothing.

## Phase 20 – WMTS

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
- **Dimensions:** TIME follows the program's clock (see Phase 18), with the service's default when there is no clock. Other dimensions are chosen in the layer's properties. Cache keys include every dimension value.
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

## Phase 21 – Aircraft from ADS-B

Goal: ADS-B data imported from files (and, as a second step, from a live receiver) shows aircraft in 3D at their altitude above the globe, with optional trails behind them: none, over a chosen time span, or the whole of the data.

**Approach (proposed)**: a `core` subsection (`ts/adsb`) with its own unit test project (`tests/adsb`). It has:
- readers for the common ADS-B formats;
- our own Mode S/ADS-B message decoder for raw data, including CPR position decoding;
- a track store holding each aircraft's reports in time order.

Drawing shares Phase 18's machinery for moving objects and tracks, and the program-wide clock. ADS-B's geometric altitude is a height above the WGS 84 ellipsoid, which Terra Soar already uses internally. Barometric altitude is converted through the standard atmosphere and marked as approximate.

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
  - Online services such as OpenSky Network (free, with rate limits and terms) and services that need an account come with Phase 16's key and terms handling.
- **Details on click:** ICAO address, callsign, registration and type (from an optional, downloadable aircraft database pack, if a suitably licensed one exists), altitude (geometric and barometric), ground speed, track, vertical rate, squawk, and the time of the last report.
- **Follow mode:** the camera follows the selected aircraft.
- **Filters:** by callsign, ICAO address, altitude band and area.
- **Export:** a selected aircraft's track exported as KML (`gx:Track`, with Phase 19), GPX or CSV.
- **Performance:**
  - A busy live view (1,000 aircraft) and a full day's recording (several million reports) stay within the frame and memory budgets.
  - Trails are GPU line strips with a time per vertex, so the time span is applied in the shader, and moving the clock rebuilds nothing.
- **Privacy:** ADS-B is a public broadcast, but some operators ask for their aircraft not to be shown (FAA LADD/PIA). When an online service supplies such a block list, it is honoured. Users' own receiver data is theirs to view.

**Technical documentation**

- `docs/design/adsb.md`: the formats read, the decoder (CPR, CRC), altitude types and datum handling, the track store, interpolation, trail drawing and time filtering, and live connections
- `docs/user/aircraft.md`: importing data, trails, playback, live receivers

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
- **Golden images:** a fixture recording at a fixed clock time, on the globe in 3D with each trail mode, and on the map.
- **Performance:** the day-long fixture recording loads within the time and memory budgets and plays within the frame budget.

**Exit criteria**: imported recordings in every supported format play back with aircraft at the right positions and altitudes. Each trail mode is correct at any clock time, a local receiver works live, and the budgets hold.

## Items deferred from 0.0.1

These came up during Phases 6–8 and were left out on purpose. They are small enough to fit into any phase.

- **EGM2008 1′ geoid pack.** It isn't published on the PROJ CDN (about 1 GB from NGA), and it differs from the 2.5′ pack by centimetres outside high mountains. Build it if users ask.
- **Code signing and Flathub**: Phase 9.
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
2. **1.0 criteria**: is Phase 9 enough for 1.0, or should 1.0 also include Phase 10 (local data files) or Phase 17 (the user guide in the program)?
3. **Qt 5 support**: Qt 5.15 is past its open-source end of life. When does Terra Soar drop Qt 5? Dropping it simplifies Phase 15.
4. **macOS**: the first roadmap targets Windows and Linux only. Is macOS wanted? If so, it needs its own packaging phase (and OpenGL is deprecated there, which points to Phase 15's backend).
5. **Translations**: which languages first, and who reviews them?
6. **Update notice**: is a version check (opt-in, no automatic download) wanted at all?
7. ~~**SGP4 implementation** (Phase 18)~~: decided 2026-11-17. Terra Soar writes its own, with a full test suite (see Phase 18's Decisions).
