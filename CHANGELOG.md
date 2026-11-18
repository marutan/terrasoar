# Changelog

All notable changes to Terra Soar. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/) (see
[docs/release/process.md](docs/release/process.md)).

## [Unreleased]

### Added

- WMTS layers: File → Add WMTS Layer… (or Add Layer → WMTS Layer… in the
  Layers panel) adds a layer from a Web Map Tile Service: national base
  maps, aerial photographs, NASA's satellite imagery and more, from a list
  of open services or any address (KVP or `WMTSCapabilities.xml`). Choose
  the style, image format, tile matrix set and dates or other dimensions;
  the service's terms and credit are shown before adding. Tile matrix sets
  in latitude/longitude, in Web Mercator and in national grids (any
  coordinate system with an EPSG code, such as the British National Grid)
  are drawn, on the globe, the maps and through the change between them. WMTS layers cache their tiles,
  work offline from saved tiles and can be kept as offline regions, like
  WMS layers.
- The user guide inside the program: Help → User Guide (F1) opens it in a
  window of its own, offline and matching the installed version, with the
  pages down the side, search across the pages, find in page, Back and
  Forward, and links to other documents opened on GitHub at this
  version's tag. F1 in the Add WMS Layer, Add Web Terrain, Offline
  Regions, New Offline Region, Data Packs and Elevation Profile windows
  opens the guide at their section, and the About window links to it.

### Fixed

- Zooming, panning and tilting with an imagery layer such as a WMS layer
  no longer makes the detail drop to a coarse level and come back
  (flicker): what the view relies on is no longer evicted from the GPU.
- Close to the ground and looking towards the horizon (terrain off), the
  globe's plain blue no longer shows through the imagery in the distance.
- Changing between the globe and the map while zoomed in no longer makes
  the detail of the map flicker during the change: the tiles picked on
  the way are the ones actually in view, not thousands more.

## [0.0.1] – 2026-11-14

The first public build: everything below is new.

### The globe and the map

- A 3D globe on the WGS 84 ellipsoid, drawn with OpenGL 3.3, precise from
  space down to a metre above the ground (relative-to-eye rendering with
  split doubles).
- Mouse and keyboard navigation with inertia and smooth flights; double-click
  to fly in; Home flies back.
- A flat map in Mercator, Web Mercator or Miller Cylindrical, morphing
  smoothly to and from the globe with every layer intact, wrapping round
  east to west.
- The pointer's position and the eye height in the status bar; an optional
  scale bar; a debug overlay (F3).

### Layers

- A layer list with collections, opacity, ordering by drag and drop, and
  collections composited as one picture; projects saved as `.terrasoar`
  files.
- Built in, offline (the base pack, 63 MB): NASA Blue Marble, the GSHHG
  shoreline and land, a graticule, Natural Earth country outlines, the
  ETOPO 2022 global DEM and the EGM2008 and EGM96 geoids.
- WMS layers (1.3.0 and 1.1.1) from a catalogue of public services or any
  address, in the server's geographic or Web Mercator CRS, with credits
  shown on screen.

### Online and offline

- A disk cache for web tiles with HTTP revalidation; when a server can't be
  reached, saved tiles are shown and the layer says it is offline.
- Offline regions: keep an area of a WMS layer (a rectangle, a polygon,
  the current view or a country) on the computer, with a size estimate
  and pause/resume.
- Data packs, downloaded from inside the program: Blue Marble and the
  shoreline at full resolution, EGM2008 at 2.5′, OSGM15 for Great Britain.

### Terrain

- Terrain from the built-in DEM, off by default: draped imagery and
  shorelines, vertical exaggeration, crack-free tiles, picking and a camera
  that stays above the ground; it flattens on the map, where it is
  hillshaded instead.
- Your own DEM files (GeoTIFF/COG and anything GDAL reads, in any CRS),
  Terrarium and Terrain-RGB web tiles, and Cloud Optimized GeoTIFFs over
  HTTP; every visible elevation layer is used, the upper ones first.
- Heights converted between datums: EGM2008, EGM96, Ordnance Datum Newlyn
  (with the OSGM15 pack) and the ellipsoid; the height under the pointer
  above sea level or the ellipsoid.
- Elevation profiles along a line drawn on the globe, with CSV export.

### Program

- Windows 10+ installer and Linux AppImage, Flatpak bundle and source
  tarball, each with the base pack.
- An About window with the data in use, its sources, licences and credits,
  and the third-party notices.
- English text throughout, ready for translation (Qt Linguist).

[Unreleased]: https://github.com/marutan/terrasoar/compare/v0.0.1...HEAD
[0.0.1]: https://github.com/marutan/terrasoar/releases/tag/v0.0.1
