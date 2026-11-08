# Building Terra Soar

Terra Soar builds with qmake on Windows 10+ and Linux, against Qt 5.15 or
Qt 6. This page is all a new developer needs to go from a fresh clone to
passing tests.

## Requirements

| Item | Version | Notes |
| --- | --- | --- |
| Qt | 5.15 or 6.x | Modules: Core, Gui, Widgets, OpenGL (Qt 6 also OpenGLWidgets), Test; plus the **linguist tools** (`lupdate`, `lrelease`) |
| C++ compiler | C++17 | GCC ≥ 9, Clang ≥ 10, or MSVC 2019/2022 |
| GDAL | ≥ 3.4 | C API |
| PROJ | ≥ 8 | |
| GEOS | ≥ 3.10 | C API (`geos_c`) |
| SQLite | ≥ 3.31 | |
| GeographicLib | ≥ 1.52 | C++ library; geodesic distances and azimuths |
| Python | ≥ 3.8 | Style checks, translation and performance scripts |
| clang-format | 18 | Only for the style checks; CI pins this version |
| clang-tidy, clazy, cppcheck | 18, 1.11, 2.13 | Only for the static analysis (Linux); the Ubuntu 24.04 versions, which CI uses |
| Doxygen + Graphviz | ≥ 1.9 | Only for the API docs |

CI uses Qt 5.15.2 and Qt 6.8.3 (see `.github/workflows/ci.yml`).

## Linux

### Ubuntu 24.04 / Debian 13

```sh
sudo apt install build-essential python3 \
    libgdal-dev libproj-dev libgeos-dev libsqlite3-dev libgeographiclib-dev \
    libgl1-mesa-dev libgl1-mesa-dri \
    clang-format-18 doxygen graphviz

# Qt 6 …
sudo apt install qt6-base-dev libqt6opengl6-dev qt6-l10n-tools
# … or Qt 5
sudo apt install qtbase5-dev qttools5-dev-tools

# Static analysis (optional; needs Qt 6 and Clang)
sudo apt install clang clang-tidy-18 clazy cppcheck
```

`qt6-l10n-tools` / `qttools5-dev-tools` provide `lupdate` and `lrelease`.
The build fails without them, because the English translation is compiled
into the application.

### Fedora

```sh
sudo dnf install gcc-c++ python3 gdal-devel proj-devel geos-devel \
    sqlite-devel GeographicLib-devel mesa-libGL-devel mesa-dri-drivers clang-tools-extra doxygen \
    qt6-qtbase-devel qt6-linguist
```

### Build

Build out of source, in a directory under `build/` (ignored by git):

```sh
mkdir -p build/release && cd build/release
qmake6 ../../terrasoar.pro        # or: qmake (Qt 5)
make -j"$(nproc)"
QT_QPA_PLATFORM=offscreen make check
./app/terrasoar
```

The dependencies are found with `pkg-config` (packages `gdal`, `proj`,
`geos`, `sqlite3`). To use a private prefix, set `PKG_CONFIG_PATH` before
running qmake.

Linux builds link with `--as-needed`, so the application only loads the
libraries it calls into, not the hundred or so that GDAL depends on.

## Windows

1. Install Qt with the online installer or `aqtinstall`: pick an MSVC
   64-bit kit (Qt 5.15.2: `msvc2019_64`; Qt 6: `msvc2022_64`). The kit
   includes the linguist tools.
2. Install Visual Studio 2022 (or Build Tools) with the C++ workload, and
   optionally [jom](https://wiki.qt.io/Jom) for parallel builds.
3. Build the dependencies with vcpkg:

   ```bat
   vcpkg install --triplet x64-windows --x-install-root=C:\deps-vcpkg ^
       gdal proj geos sqlite3 geographiclib
   ```

   Any prefix with `include\`, `lib\` and `bin\` works; that directory is
   `DEPS_ROOT`.
4. In an *x64 Native Tools Command Prompt*:

   ```bat
   mkdir build\release && cd build\release
   C:\Qt\6.8.3\msvc2022_64\bin\qmake ..\..\terrasoar.pro ^
       DEPS_ROOT=C:\deps-vcpkg\x64-windows
   jom          &rem or: nmake
   jom check
   ```

   `DEPS_ROOT` may also be set as an environment variable. If your import
   libraries have other names, override them with `GDAL_LIB`, `PROJ_LIB`,
   `GEOS_LIB` and `SQLITE_LIB` (defaults `gdal`, `proj`, `geos_c`,
   `sqlite3`).
5. Before running, put `%DEPS_ROOT%\bin` and Qt's `bin` on `PATH` (or run
   `windeployqt app\release\terrasoar.exe`).

## Build options

Pass these to qmake as `CONFIG+=<option>`:

| Option | Effect |
| --- | --- |
| `werror` | Warnings are errors (CI uses this) |
| `debug` / `release` | Build type |
| `tsan` | ThreadSanitizer build (Linux, GCC or Clang); leaves out the smoke tests, see [testing.md](testing.md) |
| `asan` | AddressSanitizer + UBSan build; undefined behaviour stops the program |
| `compiledb` | Clang only: each object also writes a compile-database fragment (`-MJ`), for the static analysis |
| `msvc_analyze` | MSVC only: code analysis (`/analyze`) of our own code; with `werror` its warnings are errors |
| `coverage` | gcov / llvm-cov instrumentation |

Use `-spec linux-clang` to build with Clang on Linux.

## Make targets

Run from the build directory:

| Target | What it does |
| --- | --- |
| `make` | Builds the core library, application and tests |
| `make check` | Runs every test (see [testing.md](testing.md)) |
| `make lupdate` | Refreshes `translations/terrasoar_en.ts` from the sources |
| `make docs` | Builds the API docs into `build/docs/api` |
| `make analyze` | Static analysis: clang-tidy, clazy and cppcheck (Linux), see below |

The style checks run from the repository root:

```sh
python3 tools/checks/run_all.py
```

Set `CLANG_FORMAT`, `LUPDATE`, `QMAKE` or `DOXYGEN` to choose a particular
binary, e.g. `CLANG_FORMAT=clang-format-18`.

## Static analysis

`make analyze` (from any build directory) or the script itself runs the
same static analysis as CI:

```sh
python3 tools/checks/run_static_analysis.py              # whole tree
python3 tools/checks/run_static_analysis.py --changed    # only what differs from origin/main
python3 tools/checks/run_static_analysis.py --self-test  # do the tools still work?
python3 tools/checks/run_static_analysis.py --tools clang-tidy
```

It builds the project with Qt 6 and Clang (`CONFIG+=compiledb`) in its own
directory (`build-analyze/`, or `analyze/` under the build directory for
`make analyze`), then runs each tool on our sources. It needs Qt 6, Clang
and the three tools; set `QMAKE`, `CLANG_TIDY`, `CLAZY` or `CPPCHECK` to
pick particular binaries. Any finding fails it. To fix or suppress one, see
[coding-standards.md](coding-standards.md#warnings-and-static-analysis).

clazy is built against its own LLVM version. If it reports missing built-in
headers such as `stddef.h`, point it at that LLVM's resource directory:

```sh
export CLAZY_RESOURCE_DIR="$(clang-15 -print-resource-dir)"
```

## Running without a display or GPU

- `QT_QPA_PLATFORM=offscreen ./app/terrasoar --smoke-test` opens and closes
  the main window without a display.
- `./app/terrasoar --software-gl` asks Qt for software OpenGL. On Linux,
  set `LIBGL_ALWAYS_SOFTWARE=1` to use Mesa llvmpipe. On Windows, Qt loads
  `opengl32sw.dll` from its `bin` directory; Qt 5's bundled one only offers
  OpenGL 3.0 and Qt 6 ships none, so install Mesa llvmpipe: from the
  MinGW x64 build of [mesa-dist-win](https://github.com/pal1000/mesa-dist-win)
  26.2.6 (the version CI uses), copy `x64/opengl32.dll` to Qt's `bin` as
  `opengl32sw.dll`, and copy `x64/libgallium_wgl.dll` next to it.
- `./app/terrasoar --gl-check` reports whether an OpenGL 3.3 core context
  can be created and prints the renderer as JSON.

The offscreen platform has no OpenGL, so the GL tests skip under it. To
run them without a display, use a virtual X server:
`LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb xvfb-run -a make check`.

## Troubleshooting

- **`lrelease: No such file or directory`**: the Qt linguist tools are
  missing; install them (see above). As a stop-gap, a Qt 5 build can use
  Qt 6's `lrelease` (the `.qm` format is the same):
  `qmake QT_TOOL.lrelease.binary=/usr/lib/qt6/bin/lrelease ...`, and the
  translation check can use Qt 6's `lupdate` via
  `LUPDATE=/usr/lib/qt6/bin/lupdate`.
- **`Package gdal was not found`**: install the `-dev` packages, or set
  `PKG_CONFIG_PATH` to your prefix's `lib/pkgconfig`.
- **`DEPS_ROOT is not set`** (Windows): pass `DEPS_ROOT=...` to qmake.
- **Programs crash before `main()` inside `ld-linux`
  (`_dl_relocate_object`), or the compiler or Doxygen segfaults**: a system
  library is damaged on disk. Find it with
  `LD_DEBUG=reloc ./tst_x 2>&1 | grep 'relocation processing' | tail -1`,
  confirm with `dpkg --verify <package>`, and reinstall that package.
