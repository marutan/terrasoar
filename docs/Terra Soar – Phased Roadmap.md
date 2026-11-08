# Terra Soar – Phased Roadmap

Nov 6, 2026 · @Peter

## Overview

Terra Soar is built in ten phases (0, 0.5 and 1–8), each ending with working software, technical documentation and a regression test suite that every later phase must keep green.

Key decisions this plan builds on: Qt 5.15 and Qt 6 with qmake; Windows 10+ and Linux; GPL-2.0-or-later; own OpenGL 3.3 renderer behind a backend abstraction; WGS 84 ellipsoid; geographic EPSG:4326 quadtree tiling; layer tree with ordering and alpha; WVS, Blue Marble and WMS as the first sources; 2D Mercator and Miller with an animated 3D/2D morph.

Every phase has the same parts:

- **Deliverables**: the features that ship.
- **Technical documentation**: design notes in `docs/` (Markdown, versioned with the code) plus Doxygen API docs for public classes.
- **Regression tests**: Qt Test cases added to `tests/`, run by CI on every commit across all OS × Qt combinations.
- **Exit criteria**: what must be true before the next phase starts.

* **Unit tests**: every subsection the phase adds has its own unit test project, written with the code.
* **Performance checks**: the phase meets the frame-rate and memory budgets on the reference PC (Core i3-4130, HD Graphics 4400, 4 GB RAM) and under software rendering.

A phase is done only when its tests pass on all CI configurations (Windows and Linux × Qt5/Qt6), its docs are merged, and no earlier test has been disabled.

## Performance requirements (all phases)

The program must run well on integrated graphics and stay usable over remote desktop (RDP, xrdp) and with no hardware acceleration at all.

| Environment | Target | Notes |
| --- | --- | --- |
| Reference PC: Core i3-4130, HD Graphics 4400, 4 GB RAM | At least 20 fps while navigating | Full quality |
| Remote desktop | At least 10 fps | Frames sent over the network; redraw only on change |
| Software rendering (Mesa llvmpipe, WARP) | At least 10 fps | Reduced quality tier chosen automatically |
| Idle (no input, no loading) | \~0% CPU/GPU | No continuous render loop |

**Frame-rate cap:** never render faster than the screen's refresh rate. Use vsync (`QSurfaceFormat::setSwapInterval(1)`); where vsync isn't available (remote desktop, some software renderers), limit frames with a timer set from `QScreen::refreshRate()` of the screen the window is on.

**Memory budget:** under **256 MB** total process memory in common use, never more than **1 GB**. On integrated graphics, GPU textures come out of the same system RAM, so they count towards this.

| Item | Budget (common use) |
| --- | --- |
| Qt, GDAL, PROJ, GEOS and code | \~70 MB |
| GPU tile textures and meshes | \~96 MB |
| CPU-side decoded tile cache | \~48 MB |
| Vector data (WVS LODs in view) | \~24 MB |
| Network buffers, UI, headroom | \~18 MB |
| **Total** | **\~256 MB** |

The reference CPU has 2 cores and 4 threads, so the worker pool defaults to 2–3 threads. The disk cache sits outside the memory budget.

**Design rules**

- Render on demand: redraw only when the camera, data or animation changes.
- Quality tiers (High / Medium / Low / Software), picked at start-up from the detected renderer and changeable by the user: screen-space-error threshold, render scale, MSAA, tile mesh density, texture size, effects.
- Keep the GPU baseline at OpenGL 3.3 core, which Mesa's software renderer supports. On Windows, ship a software OpenGL fallback (Qt 5's `opengl32sw.dll`; a Mesa build for Qt 6), because the system fallback over RDP is only OpenGL 1.1.
- Detect remote sessions and software renderers; pick a lighter tier and shorter or optional morph animation automatically.
- Strict budgets: GPU memory, tile cache size, uploads per frame, network requests in flight.
- No work on the UI thread beyond input and draw submission; all loading and decoding on workers.
- Cheap draw paths: batched draws, few state changes, shared index buffers, culling before upload.

**Frame-rate-independent motion:** pan, zoom, rotate and animation speeds are the same at 10, 20 or 144 fps.

- All motion is driven by elapsed time from a monotonic clock (`QElapsedTimer`), never by frame count.
- Mouse drags keep the grabbed point under the cursor, so they follow the hand at any frame rate.
- Keyboard and button movement is defined per second (e.g. degrees per second, zoom factor per second).
- Smoothing and inertia use time-based decay (`1 - exp(-k × dt)`), not a fixed fraction per frame.
- Fly-to and the 3D/2D morph have durations in seconds; at low frame rates frames are skipped, not slowed.
- Very long frames (a stall) are clamped so the camera never jumps wildly.
- Regression test: a fake clock replays the same input at 10, 20, 60 and 144 fps; the final camera position must match within a small tolerance.

**Performance regression tests**

- Benchmark suite (`QBENCHMARK`) for hot paths: coordinate conversion, tile selection, triangulation, decoding.
- Scripted camera flights recorded on Mesa llvmpipe in CI; frame time, tiles loaded and memory compared with stored baselines; CI fails on a regression beyond a set margin (e.g. 10%).
- Idle test: CPU use after 10 s without input stays near zero.
- Start-up time and memory ceiling checks.
- Each phase adds benchmarks for the subsections it introduces.

* Memory tests: peak and steady-state memory measured during the scripted flights; CI fails above 256 MB in the standard scenario or 1 GB in the stress scenario.

## Phase 0 – Project foundations

Goal: an empty but fully built, tested and documented project on all CI configurations.

**Deliverables**

- qmake `subdirs` tree: `core/`, `app/`, `tools/`, `tests/`, `docs/`, with `common.pri` and `deps.pri`
- Qt5/Qt6 switch (`openglwidgets` on Qt6), deprecated APIs disabled, C++17
- GDAL, PROJ, GEOS and SQLite linked via pkg-config (Linux) and `DEPS_ROOT` (Windows)
- GitHub Actions CI (\`.github/workflows/\`), matrix: Windows (MSVC), Linux (GCC, Clang) × Qt 5.15 / Qt 6
- GitHub Actions setup: Qt installed with `install-qt-action`/`aqtinstall`; GDAL/PROJ/GEOS cached between runs; fast checks on every push and pull request; full suite (golden images, performance flights, memory, install tests) nightly and on release tags; optional self-hosted runner on the reference PC for real-hardware performance runs; release artefacts published to GitHub Releases
- Licence files: `COPYING` (GPL v2), `THIRD_PARTY_NOTICES`
- Empty main window that opens and closes cleanly

**Code standards and tooling**

- The user's `.clang-format` at the repo root (2-space indent, 80 columns, Linux braces), with clang-format pinned to one version (≥ 15) in CI
- Qt-style naming; Doxygen `/** */` comments on every public item; units and frames in names and docs
- Include order: own header, standard library, Qt, third-party (GDAL/PROJ/GEOS), project
- No `auto` keyword; C++17
- `Doxyfile` with warnings as errors
- Unit test project template for new subsections (`tests/<subsection>/`)
- Performance harness: frame-time, memory and start-up measurement, running on Mesa llvmpipe in CI

**Naming and translation**

- Project and program name: **Terra Soar** (`terrasoar.pro`, executable `terrasoar`, namespace `ts::`)
- All user-visible static strings translatable from day one: `tr()` in QObject classes, `QCoreApplication::translate()` elsewhere, `QT_TR_NOOP` / `QT_TRANSLATE_NOOP` for static tables
- Placeholders (`%1`, `%n` for plurals) instead of string concatenation; numbers, dates and units formatted with `QLocale`
- `TRANSLATIONS = translations/terrasoar_en.ts` in qmake; `lupdate`/`lrelease` in the build; `QTranslator` loaded from the system locale with English fallback
- Only English shipped at first

**Thread safety and reentrancy**

- All core libraries, maths, coordinate code and format parsers are at least **reentrant** (safe when each thread uses its own instances); shared services (caches, provider registry, tile scheduler) are **thread-safe** (safe when one instance is shared).
- No mutable global or static state; shared read-only data is immutable after construction (e.g. `std::shared_ptr<const T>`).
- No locale- or global-state-dependent C functions in parsers (`strtok`, `setlocale`, `atof`); parse numbers with `QLocale::c()` or `std::from_chars`.
- Third-party libraries used in their thread-safe forms: one GDAL dataset handle per thread, a `PJ_CONTEXT` per thread for PROJ, GEOS reentrant `_r` API with a context per thread, one SQLite connection per thread.
- OpenGL calls only on the render thread; QObjects used only from their owning thread.
- Every public class documents its thread-safety level in Doxygen (custom `@reentrant` / `@threadsafe` aliases).

**Technical documentation**

- `docs/building.md`: per-OS build instructions and dependency setup
- `docs/architecture.md`: module boundaries (core is Qt-widget-free, app is the UI shell)
- `docs/coding-standards.md` and `docs/testing.md`: style, naming, how to add a test
- Doxygen config generating API docs in CI

**Regression tests**

- Test harness: Qt Test (`CONFIG += testcase`), run with `make check`
- Build smoke test: app starts headless (`QT_QPA_PLATFORM=offscreen`) and exits with code 0
- Dependency test: GDAL, PROJ and GEOS report their versions and open a sample file

* Style checks fail CI: `clang-format --dry-run --Werror`, include-order script, `auto` keyword check, Doxygen warnings
* Baseline measurement of the empty app (memory, start-up time), so later phases can see how much of the 256 MB budget is left
* Software-rendering smoke test: app starts with Mesa llvmpipe (and the bundled Windows software OpenGL fallback) and creates an OpenGL 3.3 context

- Translation checks: `lupdate` runs in CI and fails if a user-visible string is not wrapped for translation (no new untranslated entries in `terrasoar_en.ts`)
- Pseudo-translation test: a generated test locale (accented, 30% longer strings) loads and the UI shows no truncated or untranslated text

* Thread-safety checks: ThreadSanitizer build of the unit and stress tests on Linux CI; each parser and core class has a concurrent test (many threads, own instances) and each thread-safe service a shared-instance stress test

**Exit criteria**: all CI jobs green; a new developer can build from `docs/building.md` alone.

## Phase 0.5 – Compiler warnings and static analysis

Goal: every build, local and CI, compiles with a full warning set, and static analysis runs on all code before Phase 1 adds any, so the code base starts and stays at zero findings.

**Deliverables**

- Warning flags set explicitly in `common.pri` for every build, not only through qmake's `warn_on`: `-Wall -Wextra` (plus the existing `-Wpedantic -Wshadow -Wnon-virtual-dtor`) on GCC and Clang, `/W4` on MSVC
- Further warnings once the code base is clean with them: `-Wconversion`, `-Wsign-conversion`, `-Wold-style-cast`, `-Woverloaded-virtual`, `-Wnull-dereference`, `-Wdouble-promotion`, `-Wformat=2`, `-Wimplicit-fallthrough`
- Qt and third-party headers (GDAL, PROJ, GEOS, SQLite) passed as system includes (`-isystem`, MSVC `/external:I` with `/external:W0`), so warnings and analysis cover only our own code
- `CONFIG+=werror` stays on in CI; local builds show the same warnings but don't fail on them
- Static analysis tools, each with its config checked in (`.clang-tidy` at the repo root; clazy and cppcheck settings in `tools/checks/run_static_analysis.py`) and pinned to one version in CI:
  - **clang-tidy** (`.clang-tidy`): `bugprone-*`, `concurrency-*`, `cert-*`, `performance-*`, `portability-*`, and selected `cppcoreguidelines-*`, `modernize-*` and `readability-*` checks; `WarningsAsErrors: '*'`
  - **clazy** for Qt-specific problems (detached containers, connect mistakes, `QString` misuse, non-pod globals), at level 1 plus chosen level 2 checks
  - **cppcheck** with `--enable=warning,performance,portability --inline-suppr --error-exitcode=1`
  - **MSVC `/analyze`** in one Windows build
- `compile_commands.json` produced for the analysis builds (Clang's `-MJ` through `CONFIG+=compiledb`, so no extra tool is needed)
- `tools/checks/run_static_analysis.py`: runs clang-tidy, clazy and cppcheck on the whole tree, or only on changed files (`--changed`); used both locally and in CI
- `make analyze` target, so a developer runs the same checks as CI with one command
- Suppression rule: only inline (`// NOLINT(check-name): reason`, `// cppcheck-suppress id`), always with a reason; no global disables without a note in `docs/coding-standards.md`

**Build, test and CI integration**

- Build: all `core/`, `app/`, `tools/` and `tests/` code built with the full warning set on all six OS × compiler × Qt configurations
- Test: test code is checked too (warnings and clang-tidy); `make check` runs as before, and `make analyze` is listed in `docs/testing.md` as a step before opening a pull request
- CI, every push and pull request: new **Static analysis** job (Linux Clang / Qt 6) running clang-tidy, clazy and cppcheck on changed files; any finding fails the job
- CI, nightly: full-tree clang-tidy, clazy and cppcheck, plus the MSVC `/analyze` build, with reports saved as build artefacts
- Sanitizers: the existing `asan` (AddressSanitizer + UBSan) configuration added to CI next to ThreadSanitizer

**Technical documentation**

- `docs/coding-standards.md`: warning policy, enabled analysis checks and why each disabled check is off, how to suppress a finding
- `docs/building.md`: installing clang-tidy, clazy and cppcheck; the `analyze` target and `run_static_analysis.py`
- `docs/testing.md`: running the analysis and sanitizer builds before a pull request

**Regression tests**

- Tooling self-test: a small fixture under `tests/static_analysis/` with known bad code (unused variable, uninitialised member, Qt container detach, shadowed name) that each tool must flag; CI fails if a tool stops reporting it, so a broken config can't pass silently
- Zero warnings and zero analysis findings on the whole tree in every CI configuration
- AddressSanitizer + UBSan run of the unit tests on Linux CI with no reports

**Exit criteria**: all existing code is clean with the full warning set and all analysis tools; the static analysis and sanitizer jobs are green and required for merging; a developer can run every check locally from the docs alone.

## Phase 1 – WGS 84 globe core

Goal: a shaded WGS 84 ellipsoid the user can orbit, with an accurate lat/lon readout under the cursor.

**Deliverables**

- Maths library in `core/`: double-precision vectors and matrices, WGS 84 constants
- Geodetic ↔ ECEF conversions (closed-form forward, iterative or Vermeille inverse)
- Ellipsoid normals, ray–ellipsoid intersection for picking
- Geodesic distance and azimuth via GeographicLib
- Renderer backend interface (buffers, textures, shaders, pipeline state, draw) with an OpenGL 3.3 core implementation in `QOpenGLWidget`
- Camera: orbit, zoom, tilt, fly-to; relative-to-eye rendering; reversed-Z or logarithmic depth
- High-DPI handling; debug overlay (frame time, camera position)
- Status bar lat/lon/height readout

**Technical documentation**

- `docs/design/coordinates.md`: WGS 84 parameters, frames, conversion formulas and their accuracy
- `docs/design/renderer-backend.md`: backend interface and how a future RHI backend plugs in
- `docs/design/camera-and-precision.md`: relative-to-eye method, depth strategy

**Regression tests**

- Coordinate round-trips: geodetic → ECEF → geodetic within 1 mm over a global grid incl. poles and antimeridian
- Reference values checked against PROJ (EPSG:4326 ↔ EPSG:4978)
- Geodesic distances against GeographicLib published test data
- Ray-pick tests: known screen rays hit the expected lat/lon
- Render golden-image tests: offscreen render of the globe from fixed cameras compared with a stored image within a pixel tolerance (Mesa llvmpipe on Linux CI for determinism)
- Camera tests: no jitter (position stable to < 1 px) when zoomed to 1 m above the surface

**Exit criteria**: globe renders on Windows and Linux; coordinate tests pass to the stated tolerances.

## Phase 2 – Tiling engine and layer tree

Goal: the globe draws from an EPSG:4326 tile quadtree, and users manage layers and collections with ordering and alpha.

**Deliverables**

- Quadtree: level 0 = 2 tiles, tile keys (level, x, y), bounds and ECEF tile centres
- LOD selection by screen-space error; frustum and horizon culling; polar cap geometry
- Provider interfaces: `ImageryProvider`, `ElevationProvider`, `FeatureProvider`
- Worker thread pool for load/decode; GPU upload queue drained on the render thread; in-memory LRU tile cache
- Test providers: solid colour and a checkerboard with tile-key labels
- Layer model: layers and nestable collections, visibility, per-layer alpha, JSON layer definitions (`builtin | catalogue | user`)
- Imagery compositing: bottom-to-top blend in the tile fragment shader using layer alpha
- UI: dockable layer tree (`QAbstractItemModel` + `QTreeView`), drag-and-drop reordering, visibility checkboxes, opacity slider
- Project file save/load (layer tree + camera)

**Layer tree rules (decided)**

- Collection opacity: by default a collection's alpha multiplies into its children (no extra cost). Each collection also has a "Composite as group" option that draws it offscreen and fades it as one image; it costs an extra full-screen buffer and pass, so the Software quality tier warns or falls back to multiply.
- Mixed types: collections can hold imagery and vector layers together. Imagery is always drawn first (in tree order), then vector layers on top (in tree order); the UI makes this clear.
- Sharing: each tree entry has exactly one parent. Entries pointing at the same source (file, WMS layer) share one provider and cache automatically, so duplicates cost almost nothing.
- Blend modes: alpha only for now; the compositing code keeps room for multiply/screen later.

**Technical documentation**

- `docs/design/tiling.md`: quadtree layout, LOD metric, culling, poles
- `docs/design/providers.md`: provider interfaces and threading rules
- `docs/design/layer-model.md`: tree semantics, effective visibility and opacity, project file schema

**Regression tests**

- Tile maths: key ↔ bounds round-trips, parent/child relations, antimeridian and pole edge cases
- LOD selection: fixed camera positions select the expected tile set
- Thread safety: stress test loading thousands of tiles while moving the camera (run under ThreadSanitizer on Linux)
- Layer model: insert, remove, move between collections, effective opacity and visibility, model signals via `QAbstractItemModelTester`
- Project file: save → load round-trip is identical; older schema versions still load
- Golden images: checkerboard globe, and two overlapping layers at 50% alpha

* Layer rules: effective alpha = product of ancestors; "Composite as group" matches its golden image and multiply mode matches its own; vectors render above imagery whatever the tree order; two entries for the same source create only one provider and cache

**Exit criteria**: smooth navigation with test providers; reordering and alpha changes show on screen within one frame.

## Phase 3 – Built-in base pack (WVS + Blue Marble)

Goal: on first launch, offline, the user sees Blue Marble imagery with WVS land/sea fill and a graticule, all within a 75 MB base pack (about 20 MB of it reserved for the coarse global DEM added in Phase 7).

**Deliverables**

- Packaging tool (`tools/packager`), run at build time, not in the app:
  - WVS: join coastline lines into land polygons, repair with GEOS, simplify into LOD levels, write GeoPackage with spatial index
  - Blue Marble: cut the equirectangular source into EPSG:4326 quadtree JPEG tiles (about levels 0–4), stored in SQLite
  - Pack manifest (JSON): contents, versions, checksums, licences and attribution
- `GeoPackageFeatureProvider` and `SqliteTileImageryProvider`
- Vector polygon renderer: triangulation on the ellipsoid with edge subdivision, fill colour, outline colour, alpha
- Land/sea rendering: ocean colour on the globe, land polygons above; usable as a mask
- Graticule layer (generated)
- Attribution overlay in the viewport
- Default layer tree: Blue Marble, WVS shoreline, graticule

**Technical documentation**

- `docs/data/base-pack.md`: contents, sources, licences, size budget
- `docs/tools/packager.md`: how to rebuild the pack from source data
- `docs/design/vector-rendering.md`: triangulation, subdivision, LOD switching

**Regression tests**

- Packager: output polygons are valid (GEOS `isValid`), closed, and match a reference area total within 0.1%
- Pack size check: CI fails if the base pack exceeds 75 MB
- Manifest checksums verified on load; a corrupted pack is reported, not crashed on
- Tile pyramid: every expected tile exists and decodes; tile edges line up with neighbours
- Golden images: UK, Pacific antimeridian and both poles at several zoom levels
- Offline test: app starts with networking disabled and shows the base layers

**Exit criteria**: first-run globe looks correct offline on all OSes; pack is under 75 MB.

## Phase 4 – WMS over the network and disk cache

Goal: users add any WMS 1.1.1 or 1.3.0 layer by URL, and it streams, caches and blends like a built-in layer.

**Deliverables**

- Capabilities parser: layers, styles, formats, CRS list, bounding boxes, max image size
- `WmsImageryProvider`: one GetMap per quadtree tile in EPSG:4326 / CRS:84, with correct axis order per version; EPSG:3857 fallback reprojected in the shader
- PNG + `TRANSPARENT=TRUE` for overlays, JPEG for opaque layers
- Network layer on `QNetworkAccessManager`: concurrency limit, cancellation of off-screen tiles, retries with back-off, timeouts, proxy support
- Persistent disk cache (SQLite per provider): size limit, LRU eviction, HTTP cache headers honoured
- Online catalogue (JSON) of open WMS sources, editable without a release
- "Add WMS" dialog: URL entry, layer browser, style/format choice, adds to the layer tree
- Error and offline states shown per layer

**Technical documentation**

- `docs/design/wms.md`: request building, axis-order rules, reprojection fallback
- `docs/design/network-and-cache.md`: request lifecycle, cache schema, eviction
- `docs/user/adding-wms-layers.md`: user guide

**Regression tests**

- Capabilities parsing against a stored set of real-world capabilities documents (both versions, odd-but-valid examples)
- Axis order: generated BBOX strings for 1.1.1, 1.3.0 + EPSG:4326 and CRS:84 match expected values
- Mock WMS server (local `QTcpServer`) for deterministic tests: slow, failing, oversized and wrong-format responses
- Cancellation: tiles leaving the view are cancelled and never uploaded
- Cache: hit/miss behaviour, eviction at the size limit, survives restart, recovers from a corrupt database
- Golden image: mock WMS overlay at 50% alpha over Blue Marble

**Exit criteria**: a public WMS can be added and browsed smoothly; all network tests run without internet access.

## Phase 5 – 2D views and 3D/2D morph

Goal: one view that animates smoothly between the globe and a flat Mercator or Miller map, with all layers intact.

**Deliverables**

- Projection interface: forward/inverse, valid domain, central meridian, bounds, GLSL snippet
- Projections: ellipsoidal Mercator (WGS 84), spherical Web Mercator, Miller Cylindrical
- Dual vertex positions per tile (ECEF and projected), blended by morph factor t in the vertex shader
- Camera blend from globe orbit to top-down map, keeping the centre point; about 1 s ease-in-out
- 2D navigation: pan, zoom, horizontal world wrap; Mercator clipped at about ±85°
- Antimeridian splitting for vector geometry; edge densification so lines curve correctly
- 2D variant of LOD selection and culling
- Latitude-aware scale bar; graticule drawn in the current projection
- UI: 3D/2D toggle and projection selector

**Technical documentation**

- `docs/design/projections.md`: formulas, domains, how to add a new projection
- `docs/design/morph.md`: dual-position meshes, camera interpolation, picking during morph

**Regression tests**

- Projection accuracy: forward/inverse round-trips and comparison with PROJ (`+proj=merc`, `+proj=mill`) over a global grid
- GLSL vs C++ consistency: shader output read back and compared with CPU values
- Antimeridian: polygons crossing 180° render without streaks (golden images)
- Morph: golden images at t = 0, 0.5 and 1; centre point stays fixed through the animation
- Measurement: distances are identical in 3D, Mercator and Miller (still geodesic)
- Performance: morph holds the target frame rate (at least 20 fps) on reference hardware

**Exit criteria**: toggle works in both directions with every layer type; projection tests pass to 1 mm.

## Phase 6 – Offline regions and downloadable packs

Goal: users pre-download chosen areas for offline use and install extra data packs from inside the app.

**Deliverables**

- Cache-region tool: draw a rectangle or polygon (or pick a country from WVS/Natural Earth), choose zoom levels, see tile count and size estimate before starting
- Background download with progress, pause/resume and cancel
- Pinned regions protected from LRU eviction; region manager to list, refresh and delete
- Per-provider `allowBulkDownload` flag in the catalogue; bulk download blocked where the terms forbid it (e.g. OSM public tiles)
- Pack manager: reads the online pack manifest, downloads with resume, verifies checksums, installs, updates and removes
- First extra packs: full-resolution WVS, full-resolution Blue Marble, GSHHG (with LGPL notice)

**Technical documentation**

- `docs/design/offline-regions.md`: region geometry → tile set, estimation, storage
- `docs/design/packs.md`: manifest schema, versioning, install layout, hosting
- `docs/user/offline-use.md`: user guide

**Regression tests**

- Region → tile set: known polygons produce the expected tile keys and counts (incl. antimeridian-crossing regions)
- Size estimate within an agreed margin of the actual download (mock server)
- Pinned tiles survive cache pressure; unpinned tiles are evicted first
- Bulk-download flag enforced: blocked providers refuse region downloads
- Pack install: interrupted download resumes; bad checksum is rejected; uninstall leaves no files behind
- Offline golden image: downloaded region renders with networking disabled

**Exit criteria**: a UK region can be cached and viewed offline; packs install and update cleanly on all OSes.

## Phase 7 – Terrain support (optional layer)

Goal: terrain can be switched on from local or online elevation sources, and the globe falls back to the plain ellipsoid when it is off.

**Deliverables**

- Elevation tiles on the same EPSG:4326 quadtree; per-tile height grids displaced in the vertex shader; skirts to hide cracks
- Sources: coarse global DEM in the base pack (about 5 km resolution, roughly 20 MB compressed, from public-domain ETOPO or GEBCO data with bathymetry), Terrarium/Terrain-RGB web tiles, user GeoTIFF/COG DEMs via GDAL
- Multiple elevation sources with priority (high-res local over global) and no-data handling
- Vertical datums: each elevation source declares its datum and is converted to WGS 84 ellipsoidal height internally via PROJ. Built in: EGM2008 5′ grid (\~10 MB) and EGM96 15′ grid (a few MB) in the base pack; EGM2008 2.5′/1′ and OSGM15 (UK, Ordnance Datum Newlyn) as downloadable packs. UI shows height above sea level by default, with ellipsoidal height as an option
- Imagery and vector layers draped on terrain; picking against terrain
- Vertical exaggeration; hillshade for 2D mode; terrain flattens during the morph
- Elevation readout under the cursor; simple elevation profile tool

**Technical documentation**

- `docs/design/terrain.md`: tile format, mesh building, skirts, source priority
- `docs/design/vertical-datums.md`: ellipsoidal vs orthometric heights, geoid handling

**Regression tests**

- Height sampling: known points (e.g. Ben Nevis, Dead Sea, ocean) within the DEM's stated accuracy
- Geoid: undulation values checked against published EGM96/EGM2008 test points
- Source priority: overlapping DEMs give the high-res value inside, global outside, no seams at the boundary
- Crack test: no gaps between tiles at differing LOD (golden images at tile boundaries)
- Terrain off: rendering identical to the Phase 3 golden images
- Profile tool: profile along a known line matches GDAL-sampled heights

**Exit criteria**: terrain toggles on/off cleanly; heights correct to source accuracy; no visible cracks.

## Phase 8 – Packaging and first release

Goal: installable, signed builds for Windows 10+ and Linux, released as version 1.0.

**Deliverables**

- Windows: `windeployqt` + installer (NSIS, WiX or Qt Installer Framework), optionally code-signed
- Linux: AppImage and Flatpak; source tarball suitable for distro packaging
- Base pack bundled in every installer; packs and caches in `QStandardPaths` locations
- Release pipeline in CI: tag → build → test → package → publish with checksums
- About dialog with licences and data attributions

**Technical documentation**

- `docs/release/process.md`: versioning, release checklist, signing
- `docs/user/`: complete user guide (installation, navigation, layers, WMS, offline use, terrain)
- `CHANGELOG.md`

**Regression tests**

- Install tests: each package installs, launches (smoke test), and uninstalls cleanly on clean VMs
- Packaged app runs the full offline golden-image suite
- Upgrade test: user settings, projects, packs and caches from the previous build survive an upgrade
- Licence audit: every bundled library and dataset listed in `THIRD_PARTY_NOTICES`

**Exit criteria**: 1.0 installers published for Windows and Linux; full regression suite green on the release tag.

## Regression strategy, later work and open questions

**Regression strategy across phases**

- Every phase's tests stay in the suite permanently; a test is only changed with a written reason in the PR.
- Test layers: unit (maths, parsers, models), integration (providers with mock servers, cache, packs), rendering (golden images), and packaging (install tests).
- Golden images live in `tests/golden/` with a per-image tolerance; updates need a reviewed diff image.
- No test may need the internet: all network tests use the local mock WMS/tile server and stored capabilities files.
- CI runs the fast suite on every commit and the full suite (golden images, stress, install tests) nightly and on release tags.
- Bugs get a failing regression test before the fix is merged.

**Unit testing per subsection**

Every program subsection (maths, coordinates, tiling, providers, layer model, network, cache, projections, packager, renderer backend) has its own unit test project in `tests/<subsection>/`, written alongside the code, not after it.

- Each subsection is designed for testability: dependencies behind interfaces, so tests can substitute fakes (mock providers, mock network, in-memory cache, null render backend).
- No subsection is merged without unit tests covering its public API, including edge cases (poles, antimeridian, empty input, failures).
- Unit tests run in seconds and need no GPU, network or display.
- Each subsection's design doc lists what its unit tests cover.
- CI reports coverage per subsection (gcov/llvm-cov), with a target (e.g. 80% lines) for `core/`.

**Later work (after 1.0)**

- More projections: Plate Carrée, Transverse Mercator/UTM/British National Grid, Lambert Conformal Conic, Polar Stereographic, Robinson, Mollweide, Winkel Tripel
- More formats: GeoJSON, shapefile, KML/KMZ, GPX, GeoTIFF imagery, WMTS, WFS, XYZ/TMS
- WMS TIME dimension and GetFeatureInfo; Blue Marble monthly variants
- Analysis: viewshed, line of sight, slope/aspect, contours
- 3D models (glTF), atmosphere and lighting
- Qt6 RHI backend (Vulkan, or D3D11 on Windows)
- Commercial/API-key sources

**Open questions**

None at present; all planning decisions are recorded in the decisions log.
