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
| 10 | Local data files | Vector files (GeoJSON, shapefile, KML/KMZ, GPX) and GeoTIFF imagery |
| 11 | More web services | XYZ/TMS, WMTS, WFS; WMS TIME and GetFeatureInfo |
| 12 | More projections | Plate Carrée, UTM/British National Grid, LCC, polar stereographic, world projections |
| 13 | Terrain analysis | Slope/aspect, contours, line of sight, viewshed |
| 14 | Realism | Atmosphere, sun lighting, glTF models |
| 15 | Rendering backend | Qt 6 RHI (Vulkan, Direct3D 11) |
| 16 | Commercial sources | API-key services, key storage, terms handling |
| 17 | User guide in the program | The `docs/user` guide in a window from the Help menu |
| 18 | Satellite orbits | Satellites from TLEs, with past and future tracks over chosen time spans |

The phases are mostly independent. Phase 9 should come first because it finishes the release work. Phases 10 and 11 share the vector pipeline, so 10 should come before 11's WFS. Phase 14's lighting is easier after Phase 15, if both are wanted. Phase 17 depends on nothing else and could be done at any time, including before 1.0. Phase 18 needs a program-wide clock, which Phase 11's WMS time slider and Phase 14's sun position also need. Whichever of those phases comes first should build the clock for all three.

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
  - GeoJSON, ESRI shapefile, KML/KMZ, GPX and GeoPackage
  - Any CRS, reprojected with PROJ
  - Drawn with the existing vector renderer, draped on terrain
- A simple style per layer (line colour and width, fill, point symbol), kept in the project file; KML's own styles where present.
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
- WMTS: capabilities, tile matrix sets in EPSG:4326 and EPSG:3857, styles and dimensions.
- WFS 2.0: features in the visible area, with paging, into the Phase 10 vector pipeline.
- WMS extras:
  - The TIME dimension, with a time slider and a play button
  - GetFeatureInfo on click
  - Blue Marble's monthly variants as a TIME-like choice
- Entries for these services in the catalogue, with terms and attribution.

**Technical documentation**

- `docs/design/wmts.md` and `docs/design/wfs.md`; time handling added to `docs/design/wms.md`

**Regression tests**

- Mock servers for XYZ, WMTS and WFS, with stored capabilities from real services.
- TIME: the right request for each slider position, and cache keys that include the time.
- GetFeatureInfo parsing (HTML, XML, JSON responses).
- Offline regions work for XYZ and WMTS layers.

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

**Deliverables**

- **TLE sources:**
  - A `.tle`/`.txt` file, or text pasted in (two-line and three-line forms, with checksums checked)
  - CelesTrak's published groups (stations, GPS, weather, Starlink and so on), and its OMM/JSON form, fetched over HTTP. Fetches go through the disk cache, so the last download works offline.
  - Space-Track, which needs an account, comes with Phase 16's key storage.
- **Orbit propagation with SGP4/SDP4**, the model TLEs are made for:
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

- **Propagation**: SGP4/SDP4 matches the published verification cases (Vallado et al., "Revisiting Spacetrack Report #3", the SGP4-VER test set) to their stated tolerance, including deep-space and decaying orbits.
- **Frames**: the TEME to Earth-fixed conversion matches reference values at known epochs.
- **Parsing**: valid and invalid TLEs (bad checksum, wrong columns, three-line names), OMM JSON, and a whole CelesTrak group file.
- **Network**: CelesTrak fetches against a mock server, with caching and offline reuse of the last download.
- **Tracks**: the past and future spans cover exactly the chosen times, ground tracks split at the antimeridian, and changing the clock or a span updates them.
- **Golden images**: the ISS and a GPS satellite at a fixed epoch, on the globe and the map, with past and future tracks.
- **Performance**: the frame budget holds with a fixture of several thousand satellites shown.

**Exit criteria**: satellites from a file or CelesTrak show at positions matching the reference propagator, with correct past and future tracks for any chosen spans and clock time, within the frame budget.

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
7. **SGP4 implementation** (Phase 18): write our own from the published reference (Vallado's code, freely usable), or link a library? The common C++ one, `libsgp4`, is Apache-2.0. That is compatible with GPL-3.0 but not GPL-2.0-only, so linking it would make the combined program effectively GPL-3.0. Writing our own keeps the licence as it is, and the verification test set checks it either way.
