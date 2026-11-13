# Packaging

How the release packages are built and tested. What a release publishes
and when is in [process.md](process.md).

## The installed layout

`make install` (with `INSTALL_ROOT=<dir>` to stage it) lays out a Linux
install under `PREFIX` (default `/usr/local`; the packages use `/usr` and
`/app`), from the rules in `app/app.pro`:

| Path | What |
| --- | --- |
| `bin/terrasoar` | The program |
| `share/terrasoar/basepack/` | The base pack (from `BASEPACK_DIR`, by default the `make basepack` one); the program looks here, `../share/terrasoar/basepack` from its own directory |
| `share/applications/io.github.marutan.TerraSoar.desktop` | Desktop entry |
| `share/metainfo/io.github.marutan.TerraSoar.metainfo.xml` | AppStream metadata (software centres) |
| `share/mime/packages/io.github.marutan.TerraSoar.xml` | The `.terrasoar` project type |
| `share/icons/hicolor/*/apps/io.github.marutan.TerraSoar.*` | Icons, SVG and PNG from 16 to 512 px |
| `share/doc/terrasoar/` | `THIRD_PARTY_NOTICES`, `COPYING`, `CHANGELOG.md` |

The application id `io.github.marutan.TerraSoar` names the desktop entry,
icons and metadata, and `QGuiApplication::desktopFileName()`, so desktops
and Wayland match the window to its launcher.

The icon is `packaging/icons/terrasoar.svg`; `render_icons.py` renders
the PNGs (committed, used for the window icon too) with librsvg.

## AppImage

`packaging/linux/build_appimage.py --build-dir <build> --basepack <dir>
--output-dir <dir> [--qmake <qmake>]` builds
`TerraSoar-<version>-x86_64.AppImage` and its `.sha256` from a release
build:

1. `make install` of the program into a fresh AppDir, `PREFIX=/usr`;
2. linuxdeploy and its Qt plugin (pinned builds, checked by SHA-256,
   downloaded into `<build>/appimage-tools`) copy Qt with its plugins
   (the Wayland platform plugins too when the Qt has them) and every
   library the program needs;
3. **the bundled libraries are audited**: each must come from a Debian
   package, whose copyright file goes into `usr/share/doc/<package>/`, or
   from the Qt in use, whose LGPL v3 and GPL v3 texts go into
   `usr/share/doc/terrasoar/licences/`. The list is
   `usr/share/doc/terrasoar/bundled-libraries.txt`. A library from
   anywhere else fails the build;
4. linuxdeploy packs it.

**Where it runs.** An AppImage runs on Linux systems at least as new as
the one it is built on: CI builds on Ubuntu 24.04 (glibc 2.39), because
Terra Soar needs GDAL 3.6 or later (Ubuntu 22.04 has 3.4). So it runs on
Ubuntu 24.04 and later, Fedora 40 and later, Debian 13 and so on; older
systems can use the Flatpak, which brings its own libraries.

## Flatpak

`packaging/flatpak/io.github.marutan.TerraSoar.yml`, on the KDE runtime
6.11 (Qt 6.11), builds GEOS, PROJ, GeographicLib and GDAL from their release
tarballs (pinned by SHA-256; GDAL with its internal TIFF and GeoTIFF),
then Terra Soar from the source tree (`CONFIG+=ts_app_only`: the program
without the packager and the tests) with the base pack, which must be at
`basepack/` in the tree first. The bundle,
`TerraSoar-<version>.flatpak`, is made with `flatpak build-bundle`.

Permissions: X11 and Wayland, the GPU, the network (WMS, web terrain,
data packs) and the home directory (elevation files and projects, which
refer to files by their path). Its files live under
`~/.var/app/io.github.marutan.TerraSoar/`.

## Testing the packages

`.github/workflows/packages.yml` builds the base pack, the AppImage and
the Flatpak, and runs `packaging/linux/test_package.py` on each, under
Xvfb with a fresh `HOME`:

- **AppImage**: started (`APPIMAGE_EXTRACT_AND_RUN=1`, no FUSE) with
  `--measure`; it must report the base pack found and verified (`ok`),
  the globe and imagery drawn; nothing but its own data and cache in
  `HOME`.
- **Flatpak**: installed from the bundle (`--user`, with the KDE runtime
  from Flathub), run the same way, uninstalled, and checked to be gone.

The desktop entry and AppStream metadata are validated
(`desktop-file-validate`, `appstreamcli validate`) first. The workflow
runs on changes to `packaging/`, the project files or itself, nightly,
by hand, and from the release workflow, which publishes what it uploads.

## Windows

`TerraSoar-<version>-setup.exe`, an NSIS installer, built by the packages
workflow's Windows job (MSVC 2022, Qt 6.8, vcpkg's GDAL, PROJ, GEOS,
SQLite and GeographicLib) in two steps:

1. `packaging/windows/deploy.py` stages the installation in a new
   directory: `terrasoar.exe` (with the icon and version information from
   `app.pro`); Qt and its plugins (`windeployqt`); the vcpkg DLLs the
   program imports, and theirs (followed with `dumpbin /dependents`); the
   Microsoft C++ runtime, app-local; Mesa llvmpipe as `opengl32sw.dll`
   (for `--software-gl`; Qt 6 ships none) from mesa-dist-win, pinned by
   SHA-256; PROJ's `share/proj/proj.db`; the base pack in `basepack/`; the
   notices and `licences/` (each vcpkg package's copyright file, Qt's LGPL,
   Mesa's and LLVM's licences). **Every DLL is audited**: it must come
   from vcpkg (by vcpkg's own lists of installed files), Qt, the C++
   runtime or Mesa; the list is `licences/bundled-libraries.txt`. It also
   writes `uninstall.nsh`, the exact list of staged files for the
   uninstaller.
2. `makensis packaging/windows/terrasoar.nsi` packs it:
   - for all users into `Program Files\Terra Soar` (administrator
     rights), with the GPL as the licence page;
   - a Start menu entry, an optional desktop shortcut (off by default);
   - `.terrasoar` project files opened by Terra Soar;
   - an entry in Settings → Apps (publisher Peter Howkins, version, size);
   - over an earlier installation, its uninstaller runs first;
   - the uninstaller removes exactly the installed files (never a folder
     wholesale), the shortcuts, the association and the entry. Users'
     data (`%LOCALAPPDATA%\Terra Soar`) is never touched.

   Silent use: `setup.exe /S [/D=<folder>]`, `uninstall.exe /S`.

`packaging/windows/test_installer.py` then installs it silently into a
folder with spaces in its path, checks the files, the Settings → Apps
entry, the association and the shortcut, and starts the installed program
with its own software OpenGL and PROJ's own places emptied: the base pack
must be found and verified, imagery drawn, PROJ's database usable (the
installed `proj.db`). It installs again over it with users' data in place
(an upgrade: the data must survive), then uninstalls and checks that
everything installed is gone and the users' data is still there.

## PROJ's database

GDAL needs PROJ's `proj.db` to read coordinate reference systems by code
(a UTM GeoTIFF, for example), and libproj looks for it where it was built.
So the packages ship it: `share/proj/` next to the program on Windows,
`usr/share/proj/` in the AppImage (the Flatpak's PROJ is built for its own
prefix). At start-up the program points PROJ and GDAL there
(`ts::useProjData()`) when it finds one. The state report's
`crs_database` says whether it works, and the install tests require
`ok` with PROJ's own places emptied.

## Signing

Not yet (see [process.md](process.md#signing)). The Windows job signs
`terrasoar.exe` before staging and the installer after building it, with
`signtool` and a timestamp, when the repository has the secrets
`WINDOWS_SIGNING_CERTIFICATE` (a PFX, base64) and
`WINDOWS_SIGNING_PASSWORD`; without them those steps are skipped. The
Linux packages are published with their SHA-256 checksums.
