# Changelog

All notable changes to Terra Soar. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/) (see
[docs/release/process.md](docs/release/process.md)).

## [Unreleased]

### Changed

- On the globe the view tilts up to 180°: past 85° you stay where you
  are and look up, through the horizon to straight up at the sky (the
  flat map still tilts to 70°). Keys, dragging and the tilt arc all go
  that far.
- Map tiles, terrain heights and vector tiles load from the middle of
  the view outwards, instead of in an order that could start at an edge
  of the screen.
- Fewer map tiles are downloaded: none for a layer hidden under an
  opaque layer or set fully transparent, no rough preview where a finer
  image already shows, and, while the view moves fast, only rough
  previews until it slows down.
- A tile that leaves the view while it is arriving finishes downloading
  and is kept in the cache, instead of being cut off: the connection to
  the server stays open for the next tile.
- Layers from different servers load side by side: up to 6 requests to
  each server and 16 in all, instead of 6 in all.
- Map, terrain and WFS tiles use HTTP/2 where the server offers it, so
  one connection carries many tiles at once. Data pack downloads keep to
  HTTP/1.1.
- WMS layers ask for four tiles at a time (one image twice the size, cut
  in four), for a quarter of the requests to the server.

### Fixed

- Windows: `--software-gl` makes the bundled software renderer use
  llvmpipe, as tested. Without that, on a machine with a graphics card it
  could pick a hardware driver instead, and the program could close at
  start without a message.

## [0.0.3] – 2026-11-22

### Added

- Google Maps (road map, satellite and hybrid) in the XYZ catalogue
  (Add Layer → XYZ Tile Layer). They use Google's own tile server, not a
  published service: check Google's terms before use. No offline
  regions of them.
- NGA's Vector Map Level 0 (VMAP0, 1:1,000,000) as data packs, in File →
  Data Packs: international boundaries (disputed ones drawn differently),
  hydrography (rivers, lakes, coastlines, canals), transportation (roads
  by class, railways, ferries, airports with their names), population
  (built-up areas, settlements named on the view), elevation (contours,
  spot heights, depth contours), vegetation, and ice, ground, industry
  and utilities. Hydrography, elevation and vegetation come as four
  regional packs each, read together. First-order administrative
  boundaries and areas, which ESRI owns and licenses for non-commercial
  use only, are a pack of their own. An installed pack's layers are in
  Add Layer → Data Packs (one by one, or All Layers as a collection), with
  default styles and each class shown from its own zoom level; What Is
  Here shows a feature's class and attributes in plain words.
- Navigation controls on the view (View → Navigation Controls, shown by
  default; View → Navigation Controls Corner): a compass to turn the view
  and back to north up, arrows to move it (faster the longer they are
  held), Home, zoom buttons and a height slider, and tilt buttons and an
  arc. They work by mouse, touch and keyboard (Tab into them), name their
  keys in tooltips, are at least 9 mm across, grow with the system font,
  stay readable on any background (3:1 contrast), and follow the system's
  high-contrast setting. Terra Soar remembers whether they are shown and
  where.
- The flat map can be turned and tilted (right drag, Q / E and R / F, or
  the controls), up to 70°: towards its horizon detail falls off with
  distance, the map fades into a haze with a sky beyond, and with
  terrain on the mountains stand up in relief. It still starts north up
  and straight down; N and Reset View go back.
- Ships from AIS (File → Import AIS Data…, and dragging a file onto the
  window): NMEA logs (`!AIVDM`/`!AIVDO`, with tag-block, prefixed or no
  times) and CSV (NOAA MarineCadastre's columns recognised, others
  chosen), decoded by Terra Soar's own AIS decoder (Class A and B
  positions and static data, base stations, SAR aircraft, aids to
  navigation, long-range reports). Ships are drawn turned to their
  heading (or course) and coloured by type, moored and anchored ships
  differently, hulls to scale when close in, with names (and speeds and
  destinations), course vectors and trails: off, a time span before the
  clock, or all the data, coloured by type, the layer's colour or speed.
  Recordings play on the clock; ships stay drawn for a time that depends
  on their class and speed. A list to find, show, hide and give ships
  their own trails; details on click; following a ship; filters by name
  or MMSI, type, status, speed and area; tracks exported as KML, GPX or
  CSV.
- Live ships (File → Add AIS Source…): from a receiver (AIS-catcher,
  rtl-ais, a ship's AIS) over TCP or UDP, recorded for replay if wanted,
  or from open feeds: Fintraffic's Digitraffic (CC BY 4.0, credited) and
  the Norwegian Coastal Administration's stream (NLOD).
- Aircraft from ADS-B (File → Import ADS-B Data…, and dragging a file
  onto the window): SBS, raw Mode S messages (AVR, timed), Beast,
  dump1090/readsb aircraft.json, tar1090 traces and CSV with chosen
  columns, decoded by Terra Soar's own Mode S/ADS-B decoder. Aircraft are
  drawn at their height above the globe, turned to their heading, with
  callsigns (and altitudes and speeds), optional lines to the ground, and
  trails: off, a time span before the clock, or all the data, coloured by
  aircraft, altitude (with a legend) or vertical rate, broken at gaps.
  Recordings play on the clock, with the time slider spanning them. A
  list to find, show, hide and give aircraft their own trails; details on
  click; following an aircraft; filters by callsign, address, altitude
  and area; tracks exported as KML, GPX or CSV.
- Live aircraft from a receiver (File → Connect to ADS-B Receiver…):
  dump1090 or readsb over SBS, raw AVR or Beast, or their aircraft.json,
  reconnecting when the receiver drops, and recording the session for
  replay.
- Satellites (File → Add Satellites…): CelesTrak's groups (kept for use
  offline), TLE and OMM files, or TLEs pasted in, shown where they are
  at the clock's time with their names, past tracks fading with age and
  dashed future tracks over chosen spans (in orbits or minutes, per layer
  or per satellite), optional ground tracks and footprints; on the maps as
  ground tracks. A list to show, hide and filter them; details and the
  next pass on click; old elements marked, with Refresh Elements.
  Positions come from Terra Soar's own SGP4/SDP4, checked against the
  reference test set. Whole constellations such as Starlink can be shown.
- A clock in the status bar (and View → Clock): real time at first
  (shown to the minute, so an idle program stays idle; to the second when
  paused or at another speed), play and pause, faster and slower
  (backwards too), steps, and jumping to a date and time. Projects keep
  it.
- KML and KMZ (File → Open KML or KMZ…, dragging files onto the window,
  or Open With): folders as collections in the Layers panel, places with
  their icons and names, paths and areas in their styles and at their
  heights (with walls where the file asks), pictures on the ground and on
  the screen, places and tracks shown by the program's clock, network
  links (refreshed, and offline from the last copy), regions, and a
  balloon for each place with Fly There. Documents of your own (KML →
  New KML Document) and opened files can be edited: placemarks, paths,
  polygons and holes drawn on the view, properties, undo and redo, the
  clipboard; saved as KML or as a KMZ with its pictures, and kept in
  projects. The elevation profile and offline regions export as KML.
- File → Add XYZ Tile Layer…: web maps served as `{z}/{x}/{y}` tiles
  (OpenStreetMap, OpenTopoMap, USGS imagery, NASA GIBS, EMODnet), TMS row
  order and subdomains, from a catalogue or typed in; cached and kept
  offline like WMS layers where their terms allow it.
- A program clock and the time slider (View → Time Slider): WMS and WMTS
  layers with dates (daily satellite pictures, forecasts, radar) follow
  it, with back, forward and play; each layer can instead keep a time of
  its own (its Time menu in the Layers panel). Projects save the time.
- Blue Marble month by month: a data pack with NASA's picture for every
  month, so the seasons follow the clock.
- What Is Here: clicking the globe or the map lists what the WMS layers
  that can be asked, and WFS layers, have at that point (View → What Is
  Here on Click).
- File → Add WFS Layer…: points, lines and areas from web feature
  services (WFS 2.0, GeoJSON or GML) with their attributes, loaded for
  the area in view, cached and kept offline; features drawn in their own
  colours where the data or style rules give them.

- Three more projections for the flat map (View → Map Projection):
  Equidistant Cylindrical (Plate Carrée), Gall Stereographic and
  Gall–Peters (Gall Orthographic, equal-area).
- View → Map Projection → Projection Settings…: a projection's settings,
  such as Equidistant Cylindrical's standard parallel. Projects remember
  them.
- Transverse Mercator, Universal Transverse Mercator (any zone) and
  Cassini–Soldner, with their central meridian, scale factor or zone in
  Projection Settings. These maps end at the edge of the area they show
  (40° or 30° from the central meridian) instead of wrapping round.
- Robinson, Mollweide and Winkel Tripel world maps.
- The ARC zones of the Raster Product Format (CADRG charts, CIB imagery):
  each zone's band, pixel for pixel at the product's scale, following the
  view by default, and the polar zones.
- File → Add Imagery File…: a georeferenced image from your computer
  (GeoTIFF, JPEG 2000, …) as a layer, and RPF charts and imagery (CADRG,
  CIB) through their A.TOC.
- Azimuthal Equidistant (centred on any point, with polar presets),
  Gnomonic, and Polar Stereographic for the Arctic and Antarctic (as
  EPSG:3413 and EPSG:3031).
- Lambert Conformal Conic (one or two standard parallels; it starts as
  Europe's EPSG:3034) and the American Polyconic.
- Rotated Mercator (Oblique Mercator): Mercator about a centre line of
  your choice, set by its centre and azimuth or through two places, for
  long, narrow areas that don't run north–south. The map ends cleanly at
  the edge of the area it can show.
- The UK National Grid (Ordnance Survey, OSGB36) as a map projection,
  with the grid reference under the pointer in the status bar. Positions
  convert from WGS 84 to within a few metres, or about 10 cm with the
  OSTN15 transformation, which the updated "UK grids" data pack
  (formerly the OSGM15 geoid pack) now includes.

### Fixed

- The flat map chooses its detail by the larger of a tile's height and
  width, so maps whose tiles are wider than tall (Gall–Peters near the
  poles) aren't drawn too coarse.

## [0.0.2] – 2026-11-19

### Added

- WMTS layers: File → Add WMTS Layer… (or Add Layer → WMTS Layer… in the
  Layers panel) adds a layer from a Web Map Tile Service: national base
  maps, aerial photographs, NASA's satellite imagery and more, from a list
  of open services or any address (KVP or `WMTSCapabilities.xml`). Choose
  the style, image format, tile matrix set and dates or other dimensions;
  the service's terms and credit are shown before adding. Tile matrix sets
  in latitude/longitude, in Web Mercator and in national grids (any
  coordinate system with an EPSG code, such as the British National Grid)
  are drawn, on the globe, the maps and through the change between them.
  WMTS layers cache their tiles, work offline from saved tiles and can be
  kept as offline regions, like WMS layers.
- The user guide inside the program: Help → User Guide (F1) opens it in a
  window of its own, offline and matching the installed version, with the
  pages down the side, search across the pages, find in page, Back and
  Forward, and links to other documents opened on GitHub at this
  version's tag. F1 in the Add WMS Layer, Add Web Terrain, Offline
  Regions, New Offline Region, Data Packs and Elevation Profile windows
  opens the guide at their section, and the About window links to it.
- View → Attributions shows or hides the credits in the bottom-right
  corner of the view (shown by default), next to View → Scale Bar.

### Changed

- The About window shows the copyright notice under the version. The
  separate Help → About Qt entry is gone; Qt's licence is with the other
  third-party notices in the About window.

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

[Unreleased]: https://github.com/marutan/terrasoar/compare/v0.0.3...HEAD
[0.0.3]: https://github.com/marutan/terrasoar/compare/v0.0.2...v0.0.3
[0.0.2]: https://github.com/marutan/terrasoar/compare/v0.0.1...v0.0.2
[0.0.1]: https://github.com/marutan/terrasoar/releases/tag/v0.0.1
