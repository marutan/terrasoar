# Architecture

This page describes how the code is split into modules, what each may
depend on, and the threading rules that hold across all of them. Design
notes for individual subsystems go under `docs/design/` as the phases add
them.

## Repository layout

```
terrasoar.pro      top-level qmake subdirs project
common.pri         settings every project includes (Qt switch, C++17, warnings)
deps.pri           GDAL, PROJ, GEOS, SQLite, GeographicLib (pkg-config or DEPS_ROOT)
core/              tscore: the Qt-widget-free library
  include/ts/<subsection>/   public headers
  src/<subsection>/          implementation
  core.pri                   include this to link tscore
app/               terrasoar: the desktop application shell
  src/
  app.pri                    the shell's sources without main()
tools/             build-time tools and scripts
  checks/                    style checks run by CI
  i18n/                      translation maintenance
  perf/                      performance harness and baselines
  new_test_project.py        creates tests/<subsection>/ from the template
tests/             Qt Test projects, one per subsection
  support/                   shared test helpers
  data/                      small sample files
  template/                  starting point for a new test project
translations/      terrasoar_<locale>.ts
docs/              Markdown documentation (this directory)
```

## Modules

### core (`tscore`)

A static library holding everything that is not user interface: maths,
coordinates, tiling, providers, rendering backends, caches, format parsers,
and the wrappers around third-party libraries.

- Uses Qt Core, Qt Gui and (on Qt 6) Qt OpenGL only. **No Qt Widgets**,
  so the core can be tested headless and reused by tools (such as the
  packager).
- Public headers live under `core/include/ts/<subsection>/` and are
  included as `"ts/<subsection>/<name>.h"`. Everything is in namespace
  `ts`.
- Each subsection has its own unit test project in `tests/<subsection>/`.

Subsections so far:

| Subsection | Contents |
| --- | --- |
| `base` | Build and version information, program name |
| `camera` | Camera pose, view and projection, picking rays; navigation controller (pan, rotate, zoom, keys, inertia, fly-to) driven by elapsed time (`docs/design/camera-and-precision.md`) |
| `deps` | Owning handles for GDAL datasets, PROJ contexts, GEOS contexts and SQLite connections; library versions |
| `geo` | WGS 84 constants, geodetic ↔ ECEF, normals and local frames, ray–ellipsoid picking, geodesics (`docs/design/coordinates.md`) |
| `gl` | OpenGL capability probe, software-renderer detection |
| `globe` | The globe renderer: ray-cast WGS 84 ellipsoid with graticule, imagery tiles, markers relative to the eye |
| `i18n` | Translation loading with English fallback, pseudo-translator |
| `layers` | Layer tree model (layers, nestable collections, visibility, opacity, composite groups), layer definitions, project files (`docs/design/layer-model.md`) |
| `maths` | Double-precision `Vec3`, `Vec4`, `Mat4`, `Ray`, angle conversion, high/low float split |
| `packs` | Data pack manifests (files with checksums, datasets with sources, licences and attribution), verification, finding the base pack (`docs/data/base-pack.md`) |
| `perf` | Frame-time statistics, process memory and CPU readings |
| `providers` | Tile source interfaces (`ImageryProvider`, `ElevationProvider`, `FeatureProvider`), the registry that shares providers between layers, generated test imagery, SQLite tile stores and tiled GeoPackages with their writers (`docs/design/providers.md`) |
| `render` | Renderer backend interface (`RenderDevice`) and its OpenGL 3.3 core implementation; frame pacing (`docs/design/renderer-backend.md`) |
| `tiling` | EPSG:4326 quadtree keys, tile meshes, level-of-detail selection with culling, worker-thread loader, LRU cache (`docs/design/tiling.md`) |

### app (`terrasoar`)

The desktop application: main window, menus, docks, dialogs, command line.
It owns no logic that a test or tool would need. Anything like that belongs
in core.

- `app.pri` lists the shell's sources without `main()`, so the UI tests in
  `tests/ui` build exactly the same code.
- `LayerTreeDock` shows the layer tree (`ts::LayerTreeModel`, owned by the
  main window with the provider registry); the main window turns the tree
  into what the globe draws and opens and saves projects.
- `GlobeView` (a `QOpenGLWidget`) owns the camera controller and the
  globe renderer, maps mouse and keyboard input to the controller, paces
  frames and draws the debug overlay. Where the platform can't show an
  OpenGL view (the headless `offscreen` platform), the main window shows a
  message instead and everything else still works.
- `main.cpp` handles the command line, installs translators and creates
  the main window. Its test and measurement switches (`--smoke-test`,
  `--measure`, `--gl-check`, `--pseudo-locale`, `--software-gl`) are what
  the smoke and performance tests drive.

### tools

Build-time programs and scripts. They are never shipped. Compiled tools
link tscore through `core.pri`. Python scripts need only the standard
library.

### Dependency rules

```
app ──► core ──► Qt Core/Gui/OpenGL, GDAL, PROJ, GEOS, SQLite, GeographicLib
tools ─► core
tests ─► core (+ app sources for UI tests)
```

- core never includes anything from app, tools or tests.
- Third-party headers are included only in core. The public `ts/` headers
  hide them where practical (`GeosContext::get()` returns `void*`, for
  example), so app code needs no third-party include paths.
- New third-party libraries need a decision recorded in the roadmap and an
  entry in `THIRD_PARTY_NOTICES`.

## Threading model

The roadmap's thread-safety rules apply to every module:

- Core classes, maths, coordinate code and parsers are at least
  **reentrant**: safe when each thread uses its own instances.
- Shared services (caches, provider registry, tile scheduler) are
  **thread-safe**: one instance may be shared.
- No mutable global or static state. Shared read-only data is immutable
  after construction (`std::shared_ptr<const T>`). One-time initialisation
  uses `std::call_once` or function-local statics (see
  `ts::initialiseGdal()`).
- OpenGL calls happen only on the render thread. QObjects are used only
  from their owning thread. Widgets are used only on the GUI thread.
- The UI thread does nothing beyond input handling and submitting draws.
  All loading and decoding runs on worker threads.

Every public class states its level in Doxygen with `@reentrant`,
`@threadsafe` or `@guithread` (see [coding-standards.md](coding-standards.md)).

### Third-party libraries

| Library | Rule | Wrapper |
| --- | --- | --- |
| GDAL | One dataset handle per thread; drivers registered once | `ts::GdalDataset`, `ts::initialiseGdal()` |
| PROJ | One `PJ_CONTEXT` per thread; `PJ` objects stay on their context's thread | `ts::ProjContext` |
| GEOS | Reentrant `_r` API, one context per thread. **The WKT reader and writer are not thread-safe** in GEOS 3.12 (they switch the process-wide C locale), so geometry passed between threads, or read on workers, uses WKB | `ts::GeosContext` |
| SQLite | One connection per thread | `ts::SqliteConnection` |

When a library turns out to have hidden global state, like GEOS's WKT
locale switch, write the rule here and on the wrapper class, and add a
concurrent test that would catch it under ThreadSanitizer.

## Rendering and performance

The renderer sits behind a backend interface with an OpenGL 3.3 core
implementation (`docs/design/renderer-backend.md`); the camera and the
precision techniques are in `docs/design/camera-and-precision.md`. The
ground rules:

- Render on demand only. With no input and no loading, the app uses about
  0% CPU (checked by `tst_smoke` and `tools/perf/measure_startup.py`).
- Stay within the 256 MB common-use memory budget. Headless, the
  application uses about 35 MB (the baseline in `tools/perf/baselines/`);
  with the globe on Mesa llvmpipe about 170 MB, most of it the software
  rasteriser itself.
- Run on OpenGL 3.3 core, including Mesa llvmpipe and, on Windows, a
  bundled software fallback. `ts::probeOpenGl()` detects software renderers
  so that a lighter quality tier can be picked.
- All motion is driven by elapsed time from `QElapsedTimer`, never by
  frame count, and frames never come faster than the screen's refresh
  rate (`ts::FramePacer`).

## Translation

All user-visible text goes through Qt's translation system from the start.
English is the source language, and `translations/terrasoar_en.ts` is
compiled into the application as `:/i18n/terrasoar_en.qm`.
`ts::loadTranslation()` picks the system locale's translation and falls
back to English. `ts::PseudoTranslator` provides a generated test locale
(accented, about 30% longer) that the UI tests use to catch truncated or
untranslated text.
