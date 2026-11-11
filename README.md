# Terra Soar

A 3D globe and 2D map viewer for Windows and Linux, built with Qt 5.15 or
Qt 6 and its own OpenGL 3.3 renderer. It runs well on integrated graphics,
over remote desktop and with software rendering.

Terra Soar is in early development. Following the
[roadmap](docs/Terra%20Soar%20–%20Phased%20Roadmap.md), it shows a shaded
WGS 84 globe with a graticule that you can pan, turn, tilt and zoom (down
to a metre above the surface), with the latitude and longitude under the
pointer in the status bar. The surface is drawn from a geographic tile
quadtree. Layers and collections are managed in the Layers dock
(order, visibility, opacity, composite groups) and saved with the view in
project files. A new project shows NASA's Blue Marble with GSHHG
shorelines and a graticule, all from the built-in base pack, which works
offline and credits its sources in the view; `make basepack` downloads
its sources and builds it (see [docs/tools/packager.md](docs/tools/packager.md)).
View → 2D Map (Ctrl+M) unrolls the globe into a flat map in the Mercator,
Web Mercator or Miller projection and back, with every layer intact and a
scale bar that holds in both. WMS layers over the network come next.

## Building

```sh
mkdir -p build/release && cd build/release
qmake ../../terrasoar.pro
make -j"$(nproc)"
QT_QPA_PLATFORM=offscreen make check
make basepack      # downloads about 176 MB once; builds the 52 MB base pack
./app/terrasoar
```

You need Qt 5.15 or Qt 6 (with the linguist tools), a C++17 compiler,
OpenGL 3.3 (Mesa's llvmpipe will do), and GDAL, PROJ, GEOS, SQLite and
GeographicLib. [docs/building.md](docs/building.md) has the
full instructions for each OS.

## Documentation

- [Building](docs/building.md)
- [Architecture](docs/architecture.md)
- [Coding standards](docs/coding-standards.md)
- [Testing](docs/testing.md)
- Design notes: [coordinates](docs/design/coordinates.md),
  [renderer backend](docs/design/renderer-backend.md),
  [camera and precision](docs/design/camera-and-precision.md),
  [tiling](docs/design/tiling.md), [providers](docs/design/providers.md),
  [layer model](docs/design/layer-model.md),
  [vector rendering](docs/design/vector-rendering.md),
  [projections](docs/design/projections.md),
  [the 2D map and the morph](docs/design/morph.md)
- Data: [the base pack](docs/data/base-pack.md),
  [the packager](docs/tools/packager.md)
- API reference: `make docs` (or `doxygen Doxyfile`), then open
  `build/docs/api/index.html`

## Licence

Terra Soar is free software under the GNU General Public License, version 2
or (at your option) any later version; see [COPYING](COPYING). Third-party
components and their licences are listed in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
