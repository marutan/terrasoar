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

The NSIS installer is [Phase 8c](../Terra%20Soar%20–%20Phased%20Roadmap.md#phase-8--packaging-and-first-release).

## Signing

Not yet (see [process.md](process.md#signing)); the Linux packages are
published with their SHA-256 checksums.
