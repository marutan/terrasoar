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
| `TS_EXPECT_SOFTWARE_GL=1` | The GL context must be a software rasteriser such as llvmpipe (CI) |
| `LIBGL_ALWAYS_SOFTWARE=1` | Make Mesa use llvmpipe |
| `TSAN_OPTIONS=suppressions=tests/tsan.supp` | ThreadSanitizer suppressions for third-party code |

The offscreen platform cannot create OpenGL contexts. To run the GL tests
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
| `tests/smoke` | Launches the built `terrasoar`: clean start and exit, pseudo and other locales, `--version`, usage errors, start-up measurement, idle CPU, GL 3.3 under software rendering |
| `tests/ui` | Main window: opens and closes, quit action, English texts, pseudo-locale at creation and at run time, no truncated text |

Shared helpers are in `tests/support/`. The main one is
`ts::test::runConcurrently()`, which starts many threads and releases them
together. Sample files are in `tests/data/` (see its README). Test code
finds them through `TS_TEST_DATA_DIR`.

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
- idle CPU over 10 s is above 1%, or
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
| `ci.yml` | Every push and pull request | Style checks; Linux GCC and Clang × Qt 5.15/6 (GL tests under Xvfb + llvmpipe); Windows MSVC × Qt 5.15/6; ThreadSanitizer |
| `nightly.yml` | Nightly, on `v*` tags, on demand | Benchmarks, start-up/memory/idle checks, API docs artefact, optional reference-PC run; on tags, source tarball published to GitHub Releases |

On Windows, GL tests require a context only on Qt 5, which bundles
`opengl32sw.dll`. A software fallback for Qt 6 (a Mesa build) is still to
come.

Golden-image tests (Phase 1 on), network mock-server tests (Phase 4) and
install tests (Phase 8) join these workflows in the phases that introduce
them.
