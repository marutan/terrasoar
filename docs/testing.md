# Testing

Every phase ends with a regression suite that all later phases must keep
green. A phase is done only when its tests pass on every CI configuration
(Windows and Linux × Qt 5/Qt 6), and no earlier test has been disabled.

## Running the tests

From a build directory:

```sh
QT_QPA_PLATFORM=offscreen make check
```

`make check` builds and runs every test project in `tests/`. Run one
project's tests with `make -C tests/<name> check`, or run its binary
directly. Qt Test options work as usual:

```sh
./tests/deps/tst_deps                       # all cases
./tests/deps/tst_deps projGeodeticToEcef    # one case
./tests/perf/tst_perf -functions            # list cases
./tests/perf/tst_perf -o result.xml,xml     # JUnit-style output
```

### Environment variables

| Variable | Effect |
| --- | --- |
| `QT_QPA_PLATFORM=offscreen` | Run without a display. Tests that need one skip |
| `TS_REQUIRE_GL=1` | A missing OpenGL 3.3 context is a failure, not a skip (CI) |
| `TS_BASEPACK_DIR=<dir>` | The application and `tests/ui` use the base pack in `<dir>` (`tests/ui` sets it to a fixture pack itself); `tests/packager` also checks the real pack there |
| `TS_EXPECT_SOFTWARE_GL=1` | The GL context must be a software rasteriser such as llvmpipe (CI) |
| `LIBGL_ALWAYS_SOFTWARE=1` | Make Mesa use llvmpipe |
| `QT_OPENGL=software` | Windows: Qt loads `opengl32sw.dll` instead of the system OpenGL |
| `GALLIUM_DRIVER=llvmpipe` | Windows Mesa: use llvmpipe rather than another Gallium driver |
| `TSAN_OPTIONS=suppressions=tests/tsan.supp` | ThreadSanitizer suppressions for third-party code |
| `TS_UPDATE_GOLDEN=1` | Golden-image tests write the rendered images as the new golden images instead of comparing |
| `TS_GOLDEN_OUTPUT=<dir>` | Golden-image tests that fail save `<name>-actual.png` and `<name>-diff.png` there (CI uploads them as an artefact) |

The offscreen platform cannot show OpenGL views (and without a display it
can't create contexts at all), so the GPU tests skip there and the
application shows a message instead of the globe. To run the GPU tests
without a display, use a virtual X server:

```sh
LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb TS_REQUIRE_GL=1 \
    xvfb-run -a -s "-screen 0 1280x1024x24" make check
```

## Test projects

Each core subsection has its own project. App-level projects test the
application as a whole.

| Project | Tests |
| --- | --- |
| `tests/base` | Build information, version, compiler, program name |
| `tests/deps` | GDAL, PROJ, GEOS and SQLite: versions, opening the sample files, error reporting, concurrent use with own handles, a PROJ benchmark |
| `tests/perf` | Frame-time statistics and percentiles, process memory and CPU readings, benchmarks |
| `tests/i18n` | English fallback, pseudo-translation rules, placeholders and plurals survive, concurrency |
| `tests/gl` | Software-renderer detection, OpenGL 3.3 core context creation |
| `tests/maths` | Vectors, matrices, rays, the high/low float split |
| `tests/geo` | WGS 84, geodetic ↔ ECEF round trips and against PROJ, normals, ray picking, geodesics and rhumb lines against reference data |
| `tests/render` | (GPU) The OpenGL 3.3 backend: draws, viewport orientation, instancing, uniform offsets, textures, depth, blending, error reporting |
| `tests/camera` | Pose limits, frames, pixel ↔ ray ↔ globe round trips, pan and zoom keep the point under the pointer, frame-rate independence at 10/20/60/144 fps, inertia, flights, stalls |
| `tests/tiling` | Tile keys, bounds and parent/child at the antimeridian and poles, meshes (positions, skirts, winding), selection (coverage without overlap, culling, expected tile sets, refinement order), LRU cache, loader (cancellation, cache, a many-tile stress test while the camera moves) |
| `tests/layers` | Layer tree model under `QAbstractItemModelTester`; insert, remove, move between collections, drag and drop; effective opacity and visibility; the draw stack (composite groups, nested groups, vectors above imagery); JSON; project round trip and every older format version (fixtures in `tests/data/projects`); provider sharing |
| `tests/providers` | Solid-colour and checkerboard test imagery: sizes, determinism, labels, cancellation, a shared instance on many threads. SQLite tile stores and tiled GeoPackages: write and read back, missing and undecodable tiles, bad metadata, ancestors and attributes, every truncation and byte change of a geometry blob (under ASan), a shared provider on many threads |
| `tests/packs` | Pack manifests: round trip, refused input (unsafe paths, newer formats, bad checksums), verification finding changed, truncated and missing files, concurrent verification; where the base pack is found |
| `tests/packager` | The packager on generated fixtures: valid, closed land polygons inside their tiles, lakes and islands in lakes, the antimeridian and the pole, area within 0.1 %, the tile pyramid matching the mosaic pixel for pixel, a whole pack build with its manifest, notices and size budget. With `TS_BASEPACK_DIR`: the real pack (checksums, budget, every tile and polygon, spot checks) |
| `tests/globe` | (GPU) Golden images (plain and checkerboard globe; two layers at 50 %; a group composited and multiplied), shared imagery sources, silhouette against CPU picking, horizon and marker jitter 1 m above the surface, high-DPI |
| `tests/smoke` | Launches the built `terrasoar`: clean start and exit (headless), pseudo and other locales, `--version`, usage errors, start-up measurement, idle CPU, GL 3.3 under software rendering, the globe window on the real platform |
| `tests/ui` | Main window: opens and closes, quit action, English texts, pseudo-locale at creation and at run time, no truncated text; the base pack found, damaged (reported, still usable) and missing (test imagery); layer dock (visibility, opacity, add and remove), project save and open; globe view or its fallback, drag and wheel, status bar readout, debug overlay and Reset View, no frames while idle |

Shared helpers are in `tests/support/`: `ts::test::runConcurrently()`
starts many threads and releases them together; `ts::test::OffscreenGl`
gives a GPU test an OpenGL 3.3 context and render device
(`TS_TEST_REQUIRE_GL` skips, or fails under `TS_REQUIRE_GL=1`);
`ts::test::checkGolden()` compares an image with its golden image.
Sample files are in `tests/data/` (see its README). Test code
finds them through `TS_TEST_DATA_DIR`.

### Golden images

`tests/globe` renders fixed views offscreen and compares them with
`tests/data/golden/*.png`. Rendering differs slightly between drivers, so
the comparison runs only on Mesa llvmpipe, the renderer every CI job uses,
and allows small differences between llvmpipe versions: at most 0.5 % of
the pixels may differ by more than 12 levels in any channel. On other
renderers the test renders (and checks for errors) but skips the
comparison.

After an intended change to the picture, regenerate the images on
llvmpipe, look at every changed one, and commit them with the change:

```sh
LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb TS_UPDATE_GOLDEN=1 \
    xvfb-run -a ./tests/globe/tst_globe goldenImages
git diff --stat tests/data/golden
```

When CI fails a comparison it uploads the actual and difference images
(`golden-output-*` artefacts).

### Application exit codes

The smoke tests check these. They are part of the command-line contract:

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Failure (e.g. `--gl-check` found no OpenGL 3.3, a measurement file could not be written) |
| 2 | Usage error (bad option value) |

## Adding a test project

Every new subsection gets its own test project, written with the code:

```sh
python3 tools/new_test_project.py <subsection>
```

This creates `tests/<subsection>/` from `tests/template/` and adds it to
`tests/tests.pro`. The generated file has three sections to fill in:

1. **Behaviour:** one private slot per case. Use `_data()` slots for
   tables of inputs. Cover edge cases explicitly: poles, the antimeridian,
   empty input, invalid input, error paths.
2. **Thread safety:** for a reentrant class, a test where many threads use
   their own instances (`runConcurrently` with
   `ts::test::concurrentThreadCount`). For a thread-safe service, a stress
   test where they share one instance.
3. **Performance:** a `QBENCHMARK` for each hot path (coordinate
   conversion, tile selection, decoding, …).

Use `QTEST_GUILESS_MAIN` for core tests. Use `QTEST_MAIN` only when the
test needs widgets. Tests needing the app's sources include
`$$TS_ROOT/app/app.pri`, as `tests/ui/ui.pro` does.

Rules:

- Tests are deterministic and need no network. Network tests (Phase 4)
  use a local mock server.
- A test that can't run in an environment calls `QSKIP` with the reason
  and the variable that turns the skip into a failure. It never passes
  silently.
- Never disable or weaken an existing test to make a change pass.

## ThreadSanitizer

```sh
mkdir -p build/tsan && cd build/tsan
qmake6 -spec linux-clang ../../terrasoar.pro CONFIG+=tsan
make -j"$(nproc)"
TSAN_OPTIONS="halt_on_error=1 suppressions=$PWD/../../tests/tsan.supp" \
    QT_QPA_PLATFORM=offscreen make check
```

TSan builds run the unit and stress tests. They leave out `tests/smoke`,
because the smoke tests launch the app with `QProcess`, and the TSan runtime
aborts on its vfork-based process start. Smoke tests run in every other
configuration.

Any report in Terra Soar's own code is a bug: fix it. Suppressions are only
for uninstrumented third-party code, each with a reason
(see `tests/tsan.supp`).

## AddressSanitizer and UBSan

```sh
mkdir -p build/asan && cd build/asan
qmake6 -spec linux-clang ../../terrasoar.pro CONFIG+=asan
make -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1 QT_QPA_PLATFORM=offscreen make check
```

The `asan` build finds memory errors and leaks (AddressSanitizer) and
undefined behaviour (UBSan). Undefined behaviour stops the program, so the
test fails. CI runs it on every push with no reports allowed. Like the TSan
job it uses the offscreen platform, because Mesa's llvmpipe isn't
instrumented.

## Static analysis

```sh
make analyze                                            # from a build directory
python3 tools/checks/run_static_analysis.py --changed   # only your changes
```

Runs clang-tidy, clazy and cppcheck (see
[building.md](building.md#static-analysis) and
[coding-standards.md](coding-standards.md#warnings-and-static-analysis)).
`--self-test` checks that each tool still reports the known-bad code in
`tests/static_analysis/`; CI runs it before every analysis.

## Before opening a pull request

1. `make check` in a normal build (with `CONFIG+=werror`).
2. `python3 tools/checks/run_all.py` for the style and documentation checks.
3. `make analyze`, or `run_static_analysis.py --changed`, for the static
   analysis.
4. For threading or memory changes, the TSan and ASan builds above.

CI runs all of these; running them first saves a round trip.

## Style and documentation checks

```sh
python3 tools/checks/run_all.py
```

These run clang-format, the include-order check, the no-`auto` check, the
translatable-string check, the translation-file check and Doxygen
(see [coding-standards.md](coding-standards.md)). CI fails on any of them.

## Translation tests

- `update_translations.py --check` fails if a translatable string is
  missing from `translations/terrasoar_en.ts`, if the file has stale
  entries, or if any entry is unfinished.
- `check_translatable.py` fails if a literal reaches a user-visible API
  without being wrapped.
- `tests/ui` and `tests/smoke` run the UI under the generated
  pseudo-locale (`--pseudo-locale`, about 30% longer, accented). They fail
  if any text is untranslated or truncated.

## Performance

**Benchmarks** are `QBENCHMARK` cases in the unit test projects. The
nightly workflow runs them and keeps the XML results as an artefact.

**Start-up, memory and idle CPU** are measured by running the app with
`--measure`:

```sh
python3 tools/perf/measure_startup.py build/release/app/terrasoar
```

It runs the app five times, takes the medians, and fails if:

- peak memory is over the 256 MB budget,
- idle CPU over 10 s is above 1% (the idle period starts once start-up
  work in the background, the base pack's checksum check, has finished), or
- start-up time or peak memory is more than 10% worse than the baseline
  (start-up also gets 50 ms of absolute slack for noisy runners).

Baselines are JSON files in `tools/perf/baselines/`:

| File | Recorded on |
| --- | --- |
| `empty-app.json` | A developer machine (Qt 6.4, GCC 13, offscreen); the default for local runs |
| `ci-linux-llvmpipe.json` | The nightly CI runner (Mesa llvmpipe under Xvfb). Not recorded yet; until it is, the nightly checks only the fixed budgets |
| `reference-pc.json` | The self-hosted reference PC (Core i3-4130, HD Graphics 4400, 4 GB) |

To record or update a baseline, use `--update-baseline --baseline <file>`
on the machine it describes, and commit the file with a note on why it
changed.

Later phases add scripted camera flights, frame-time and memory checks
during the flights, and a fake-clock test that replays the same input at
10, 20, 60 and 144 fps.

## Continuous integration

| Workflow | When | Jobs |
| --- | --- | --- |
| `ci.yml` | Every push and pull request | Style checks; Linux GCC and Clang × Qt 5.15/6 (GL tests under Xvfb + llvmpipe); Windows MSVC × Qt 5.15/6; ThreadSanitizer; AddressSanitizer + UBSan; static analysis of the changed files (whole tree on `main`), after the tooling self-test |
| `nightly.yml` | Nightly, on `v*` tags, on demand | Benchmarks, start-up/memory/idle checks, API docs artefact, optional reference-PC run; full-tree static analysis and MSVC `/analyze`, with reports as artefacts; on tags, source tarball published to GitHub Releases (after the analysis jobs pass) |

On Windows, CI installs Mesa llvmpipe (from
[mesa-dist-win](https://github.com/pal1000/mesa-dist-win), version and
SHA-256 pinned in `ci.yml`) as Qt's `opengl32sw.dll`, and the GL tests
require a software OpenGL 3.3 context on both Qt 5 and Qt 6. Qt 5's own
`opengl32sw.dll` (Mesa 12) only offers OpenGL 3.0, so it is replaced.

Qt Test's console output doesn't reach the log on the Windows runners. CI
sets `TESTARGS="-o -,txt -o testresults.txt,txt"`, so each test also writes
`testresults.txt` in its build directory, and prints those files when a
test fails.

Golden-image tests (Phase 1 on), network mock-server tests (Phase 4) and
install tests (Phase 8) join these workflows in the phases that introduce
them.
