# Terra Soar

A 3D globe and 2D map viewer for Windows and Linux, built with Qt 5.15 or
Qt 6 and its own OpenGL 3.3 renderer. It runs well on integrated graphics,
over remote desktop and with software rendering.

Terra Soar is in early development: Phase 0 (project foundations) of the
[roadmap](docs/Terra%20Soar%20–%20Phased%20Roadmap.md) gives an empty main
window with the build, tests, checks and documentation in place.

## Building

```sh
mkdir -p build/release && cd build/release
qmake ../../terrasoar.pro
make -j"$(nproc)"
QT_QPA_PLATFORM=offscreen make check
./app/terrasoar
```

You need Qt 5.15 or Qt 6 (with the linguist tools), a C++17 compiler, and
GDAL, PROJ, GEOS and SQLite. [docs/building.md](docs/building.md) has the
full instructions for each OS.

## Documentation

- [Building](docs/building.md)
- [Architecture](docs/architecture.md)
- [Coding standards](docs/coding-standards.md)
- [Testing](docs/testing.md)
- API reference: `make docs` (or `doxygen Doxyfile`), then open
  `build/docs/api/index.html`

## Licence

Terra Soar is free software under the GNU General Public License, version 2
or (at your option) any later version; see [COPYING](COPYING). Third-party
components and their licences are listed in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
