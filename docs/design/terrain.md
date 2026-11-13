# Terrain

How heights are stored, read, sampled and drawn. Phase 7b covered the
data (the base pack's global DEM, the elevation provider, height
sampling), Phase 7c the drawing (displaced tile meshes, skirts,
exaggeration, the morph, features on the terrain, picking), Phase 7d
further sources: DEM files through GDAL, priority between sources and
the height readout (7d-1), web terrain tiles and remote GeoTIFFs (7d-2),
hillshade on the map and elevation profiles (7d-3).
For users: [terrain.md](../user/terrain.md).
Heights above the geoid and above the ellipsoid are explained in
[vertical-datums.md](vertical-datums.md). Code:
`core/include/ts/elevation/`, the interface in
`core/include/ts/providers/tileprovider.h`.

```
elevation.sqlite (base pack)              egm2008.tif (base pack)
  tiles of 65 × 65 heights, levels 0–6      geoid undulation N, 5′
  above EGM2008, encoded                          │
        │                                         │ GeoidModel (PROJ)
        ▼                                         ▼
  SqliteElevationProvider::loadHeights(key) ── decode ── deeper than 6?
        interpolate from the level-6 ancestor ── + N per sample
        ▼
  HeightTile: samples² heights above the WGS 84 ellipsoid
        ▼
  sampleHeightM(tile, lat, lon): bilinear
```

## Height tiles

A `ts::HeightTile` is a square grid of samples over a quadtree tile
(`ts::TileKey`, the same EPSG:4326 quadtree as imagery): row 0 on the
north edge, column 0 on the west edge, **edges included**, so neighbouring
tiles share their edge samples exactly and meshes built from them meet
without gaps. Heights are metres above the WGS 84 ellipsoid, as floats.

`ts::ElevationProvider::loadHeights()` returns a tile for any key where
the source has data. A source stores some levels; `SqliteElevationProvider`
interpolates keys deeper than its deepest level from their ancestor there
(`resampleHeightTile()`), so the renderer can ask for whatever level its
imagery is at. `sampleHeightM()` interpolates a tile bilinearly; together
with `heightSamplePoint()` (where sample (row, column) lies) that is the
basis for the readout, profiles and picking.

## DEM files

`ts::GdalElevationProvider` reads the first band of any raster GDAL
opens (GeoTIFF, Cloud Optimized GeoTIFF, VRT, SRTM `.hgt`, ASCII grids,
…) in any CRS GDAL can convert latitude and longitude into, geographic or
projected. For a tile it

1. turns the 65 × 65 sample points into raster pixel coordinates
   (`OCTTransformEx`, then the inverse geotransform);
2. picks a decimation `f`, the largest power of two not above the pixels
   between neighbouring samples, and reads the window around the points as
   whole `f × f` blocks of a grid fixed to the raster (`GDALRasterIOEx`
   with `GRIORA_Average`, which uses the file's overviews where it has
   them). Because the blocks don't depend on the tile, **neighbouring
   tiles get identical edge samples** however far out the view is;
3. interpolates bilinearly between block centres, from the valid ones
   only (no-data values and NaN never pull a height down), applies the
   band's scale, offset and unit (feet), and turns the heights into
   ellipsoidal ones with the geoid of the file's datum;
4. leaves samples outside the raster or in no-data holes NaN.

A file without overviews isn't read for tiles more than 64 pixels to a
sample (they would read most of the file for a handful of samples): the
sources below take over there, and `gdaladdo` makes it available at every
level. Each loader thread uses a dataset of its own from a small pool
(GDAL datasets are not thread-safe). `maximumLevel()` is the level whose
sample spacing first matches the raster's pixels.

**Vertical datum.** A file's source names its datum (`egm2008`, `egm96`
or `ellipsoid`). Add Elevation File reads it from a compound CRS
(e.g. EPSG:4326+3855) where the file has one (`detectDemVerticalDatum()`)
and otherwise asks, suggesting EGM2008. The datum's geoid also gives sea
level; for ellipsoidal heights the base pack's EGM2008 does.

## Web terrain tiles

`ts::WebTerrainProvider` reads XYZ tile sets of PNGs in Web Mercator whose
colours hold heights (a layer source `{"kind": "web", "url":
".../{z}/{x}/{y}.png", "encoding": "terrarium" | "terrain-rgb",
"maximumZoom", "tileSize", "verticalDatum", "attribution"}`):

| Encoding | Height, metres | Used by |
| --- | --- | --- |
| Terrarium | R·256 + G + B/256 − 32768 | Mapzen / AWS Terrain Tiles |
| Terrain-RGB | −10000 + (R·65536 + G·256 + B)·0.1 | Mapbox, MapTiler and others |

Transparent pixels are no data. A quadtree tile of level L is sampled from
zoom L − 1 (L − 2 for 512-pixel tiles, at most the source's deepest): web
zoom z has 360° / (256 · 2^z) per pixel, level L 180° / (64 · 2^L) per
sample, so at the equator they match and towards the poles the web pixels
are finer. Each sample is bilinear between the centres of the four pixels
around it on that zoom's **global** pixel grid (wrapping in longitude),
so neighbouring tiles of a level read the same pixels for their shared
edge. Beyond ±85.05°, where Mercator stops, samples are NaN and the
sources below (the base pack's DEM) fill them.

Fetching goes through `ts::CachedTileFetcher`, the same as WMS: the disk
cache (per source, in the WMS cache directory), freshness and
revalidation, leaving an unreachable server alone, and the layer's status
(Offline, Error with the server's answer). 404 and 204 mean no tile there
(for good); decoded web tiles are kept in a small memory cache (64 tiles),
since neighbouring height tiles share them. Six loader threads load
heights while a network source is in use.

**Failed tiles.** A web tile that couldn't be had (no answer, an error)
leaves its samples NaN and marks the height tile **incomplete**
(`HeightTile::incomplete`); the composite fills it from the sources below
and passes the mark on. The renderer shows it, and asks for it again when
it is still wanted 30 s later (`TileRendererSettings::heightsRetryDelay`);
a mesh is rebuilt when its tile's heights are replaced (each mesh keeps
the heights it was built from to tell).

**Remote GeoTIFFs.** A Cloud Optimized GeoTIFF on a web server is a `file`
source whose path is `/vsicurl/<address>`: GDAL reads the parts it needs
with range requests, using the file's overviews when zoomed out. That
needs a GDAL built with curl (`AddWebTerrainDialog::canReadRemoteFiles()`
checks); GDAL caches what it reads in memory only.

**Adding them.** File → Add Web Terrain… (or the layer list's add menu)
offers the AWS Terrain Tiles (Terrarium, to zoom 15, heights above EGM96,
with the attribution they ask for), or any tile address, encoding, deepest
zoom, tile size, datum, name and attribution, or a GeoTIFF's address.

## Hillshade on the map

On the flat map the terrain is flat (`terrain.x = 1 − t`), so its relief
is shown by shading instead. The tile vertex shader passes the mesh's
slopes (`TileVertex::slope`, towards east and north) to the fragment
shader, which shades like a GIS hillshade: the ground's normal from the
slopes, lit by a sun in the north-west 45° above the horizon, the
brightness divided by that of flat ground and held to 0.3–1.3, then
multiplied into the colour (premultiplied, so held to its alpha).

- `FrameBlock::terrain.z` is the strength (1, or 0 when the topmost
  elevation layer's style says `"hillshade": false`; View → Hillshade on
  the Map). It is multiplied by the morph, so the shading fades in as the
  terrain flattens and the globe itself is never hillshaded.
- `terrain.w` multiplies the slopes (`GlobeRenderer::hillshadeSlopeFactor`,
  4): the meshes smooth the terrain to their vertices, and the slopes are
  also multiplied by the exaggeration.
- Without terrain shown the map is exactly as before: all the map golden
  images are unchanged.

## Elevation profiles

View → Elevation Profile… opens a window and lets the user click points
on the globe or map (`GlobeView::RegionDrawing::Line`, finished with a
double-click or Enter; the same keys as drawing a region). For the line,
`computeElevationProfile()`, on a worker thread:

1. spaces 512 points evenly along it, each leg a geodesic on WGS 84
   (`samplePath()`, GeographicLib);
2. picks the level whose height samples are as close as the points
   (`profileLevel()`, at most the source's deepest), so a long line reads
   coarse tiles and a short one fine ones;
3. loads each tile under the points once from the elevation source the
   terrain uses (all visible elevation layers combined, see above) and
   samples it bilinearly: height above the ellipsoid and sea level;
4. sums the lowest and highest point, the climb and the descent (above sea
   level).

It reads the source directly, not the tiles loaded for the view, so the
profile doesn't depend on what is on screen. The window charts height
against distance; pointing at the chart marks the point on the globe
(`GlobeView::setProfileLine()`), and Save as CSV writes distance,
latitude, longitude and both heights per point. Closing the window or
drawing another line cancels a profile still being computed.

## Several sources

All **visible** elevation layers are drawn as one terrain: each layer has
priority over those below it in the layer list, so a detailed DEM placed
above the Terrain layer is used where it has heights and the global DEM
around it. `elevationDrawForStack()` hands their providers, topmost first,
to `ProviderRegistry::elevationComposite()`, which returns a shared
`ts::CompositeElevationProvider` (the same layers give the same object,
so the renderer keeps its loaded heights). For each tile it loads the
sources in turn and fills each sample from the first source with data
there (resampling a source with another sample count onto the first
one's grid), stopping once every sample has a height and sea level is
known. Samples no source has are sea level.

Every sample of a tile is on one grid whichever source it comes from, so
**a source's edge can't open a crack**: at most the heights step there,
by however much the sources differ. If no layer is visible, the topmost
layer alone keeps its heights loaded (switching it on is immediate). The
exaggeration is the topmost visible layer's.

## The height readout

`TileRenderer::groundHeight()` samples the finest loaded heights at a
point, shown or not, unexaggerated and with the sea floor. GlobeView
reads it at the picked point each time the pointer readout updates (also
after each frame, so it fills in as heights arrive) and the status bar
shows the height above sea level (ellipsoidal height minus the sea level
of the same tile), or above the ellipsoid with View → Heights Above the
Ellipsoid. It is as exact as the heights loaded at that moment: zoomed
out, a coarse level's averages.

## The base pack's DEM

| | |
| --- | --- |
| Source | ETOPO 2022 v1, 60 arc-second surface elevation (NOAA NCEI, public domain): land, ice surface and bathymetry, above EGM2008 |
| Grid | quadtree levels 0–6, 65 × 65 samples a tile: 2.64′ (4.9 km at the equator) at level 6 |
| Tiles | 10 922 (8192 at level 6) |
| Size | 29.3 MB (`elevation.sqlite`) |
| Heights | −10 637 m to 7336 m at level 6 |

**Sampling.** Each level-6 sample is the mean of the source cells within
half a sample spacing of it, each cell weighted by how much of it lies in
that box (the source has 2.64 cells per sample). Longitudes wrap at 180°.
Each coarser level is the finer one filtered with [1 2 1] / 4 in both
directions and taken at every other sample. The two pole rows each hold
one value, the row's mean: the pole is one point. A 5 km grid smooths
peaks and trenches: Ben Nevis (1345 m) is 616 m, Everest 7197 m, the
Challenger Deep −10 139 m; flat places come out close (the Dead Sea
−430 m, the South Pole 2832 m, Denver 1602 m).

**Encoding** (`ts::encodeHeightGrid()`). Heights are kept to the nearest
2 m down to −100 m and to the nearest 10 m below it: ETOPO is accurate to
metres on land but only to tens or hundreds of metres at sea, so the deep
sea, 70 % of the samples, gets the coarse step, while land, land below
sea level (the Netherlands) and shallow water keep the fine one. Each
value is predicted from its west, north and north-west neighbours
(w + n − nw), the residuals are written as zig-zag varints and the lot is
compressed with zlib (Qt's `qCompress`, so no codec beyond what every
platform has). The first byte is the encoding's version (1). That comes to
about 4.8 bits a sample. Keeping 1 m everywhere would have needed 35 MB
even with an ideal entropy coder; LERC, which does this best, is not
certain to be in every platform's GDAL.

The store is the same SQLite tile store as the imagery's, with `format`
`heights`, `tile_size` the samples per side and `vertical_datum`
`egm2008` ([base-pack.md](../data/base-pack.md#file-formats)). Its pages
are 64 KB: at 8 KB, tiles of 2–3 KB left a quarter of every page empty.

## Drawing the terrain

```
Terrain layer (elevation, style {"exaggeration": k})
  └─ elevationDrawForStack(): provider, visible, k
       GlobeRenderer::setElevationProvider() / setTerrain()
         TileRenderer: HeightTileLoader (2 threads) → LRU of HeightTile (64 MB)
           mesh of a tile built with its heights (or an ancestor's for now)
         VectorTileLoader: heights under every feature vertex
       Frame block: terrain = (shown × (1 − morph), k)
       vertex shaders: raise along the ellipsoid's normal
```

**Always loaded, shown on demand.** While a project has an elevation
layer, even unticked, its heights are loaded for every tile drawn and
built into the meshes. Showing the terrain, hiding it and changing the
exaggeration only change two uniforms (`FrameBlock::terrain`), so they
are instant. `LayerStack::elevation` therefore lists hidden elevation
layers too; the topmost visible one is drawn, or, if none is visible, the
topmost keeps its heights loaded.

**Meshes.** A tile mesh's vertices stay on the ellipsoid
(`TileVertex::offsetM`) and carry the terrain beside them
(`TileVertex::terrain`: height above the ellipsoid h, sea level N, a
skirt's drop d; `slope`: the terrain's slope towards east and north). The
vertex shader puts the vertex at

    N + (max(h − N, 0) − d) × k            along the ellipsoid's normal

times the uniform's first component (0 when off, falling to 0 as the view
becomes the map). Two choices are in that formula:

- **The exaggeration applies above sea level**, not above the ellipsoid:
  exaggerating h would also exaggerate the geoid (London's 46 m, Sri
  Lanka's −107 m).
- **The sea floor is flattened to sea level** (`max(h − N, 0)`): with
  bathymetry the oceans would be pits 4 km deep under imagery of the sea
  surface, and coastlines walls. Land below sea level (the Dead Sea, the
  Netherlands) is flattened too; a land mask could keep it (Phase 7d).

The tile's mesh is built with the heights of its own key once they have
arrived, with the finest ancestor's meanwhile (interpolated, so the
terrain is there at once and only sharpens), and rebuilt when its own
arrive (`GpuMesh::heightsLevel`).

**Shading.** The tile shader shades imagery by the surface normal. With
terrain the normal is tilted by the slopes (`tsTerrainNormal()`), scaled
by the exaggeration, so relief is lit by the same sun as the globe.

**Skirts and cracks.** Neighbouring tiles of different levels sample the
terrain at different spacings, so their shared edges differ. Every
tile's skirt already hung below its border by the chord's sag; with
terrain it hangs further, by the range of the tile's drawn heights plus
1 m (`d`, scaled by the exaggeration in the shader), which covers any
neighbour. `tests/globe` draws steep exaggerated terrain against a
magenta sky and checks that no sky shows below the horizon; without the
drop it finds the cracks.

**The ray-cast ellipsoid.** On the globe the surface colour and the
graticule are normally ray-cast against the ellipsoid in one full-screen
pass ([camera-and-precision.md](camera-and-precision.md)). The terrain is
not on the ellipsoid, so while it is shown the tiles carry the surface
colour themselves (the base pass, as on the map) and the graticule is
drawn on the tiles.

**Selection.** A tile behind the ellipsoid's horizon or just outside the
view may carry a mountain that is in view. While terrain is shown, the
tile and vector selections widen each tile's bounds by 9000 m × k
(`TileSelectionSettings::terrainHeightM`), and the horizon test allows
for it.

**Features.** Shorelines, the land fill and other features are meshed as
before, then every vertex gets the terrain's height and sea level under
it (`VectorVertex::terrain`), from height tiles three levels below the
stored tile's (at most the DEM's deepest level), and the vector shaders
raise it like a tile vertex. Their depth is pulled towards the eye by 1 %
of their distance instead of 10 % (`depthParams.w`), so they lie on the
ground but a mountain still hides what is behind it.

**Picking and the camera.** `Camera::setSurfaceHeight()` gives the camera
the terrain as drawn (`TileRenderer::terrainHeightM()`: the finest loaded
heights, exaggerated, the sea floor flattened). On the globe
`pickGeodetic()` then marches along the ray (steps of half its height
above the surface, at least a thousandth of the distance) and bisects
where it goes below, so zooming to the pointer, dragging and the pointer
readout use the terrain; and `setPose()` keeps the eye 20 m above it.
Where no heights are loaded the ellipsoid stands in.

**The morph.** The terrain flattens as the view becomes the map
(`terrain.x = 1 − t`). On the map itself (t = 1) the meshes are exactly
the old ones, so all map golden images are unchanged; so are all globe
golden images with terrain off.

## Tests

`tests/tiling`: mesh terrain values, slopes and skirt drop; the
selection keeping mountains past the horizon. `tests/camera`: picking a
raised surface, the sky still the sky, the eye kept above it. `tests/layers`:
elevation layers in the stack, hidden or not. `tests/globe`: no cracks
(steep synthetic terrain, magenta sky), terrain off identical to none,
golden images of the Alps, Everest, Mount Erebus by the antimeridian and
the Alps half-way to the map from the real base pack. `tests/ui`: the
Terrain layer off by default, ticking it, the exaggeration menu.
`tests/elevation`: the quantization steps; encoding round trips (and
damaged, truncated or mismatched data refused); bilinear sampling and
resampling exact on a plane; a fixture geoid through PROJ; the provider
adding N, interpolating deeper keys, refusing a store without its geoid
or of images; the registry finding the elevation and geoid datasets.
`tests/elevation` also covers DEM files: a plane read exactly at full
resolution and averaged, identical edge samples between neighbours, NaN
outside the file and in no-data holes (never pulled towards the no-data
value), a projected (UTM) file, nothing for tiles too coarse without
overviews, a broken file refused, datums found in compound CRSs, geoid
conversion, loads from eight threads at once; the composite (a DEM over
the global heights across its corner, no NaN, sea level where nothing
has data); the registry's `file` sources and shared composites.
`tests/elevation` for web terrain: both encodings decoded; the source's
JSON form and its errors; a mock server's Terrarium tiles read exactly (a
plane in global pixels), edges shared, web tiles fetched once for
neighbours, no data beyond the Mercator limit and for 404s, an incomplete
tile while the server fails and a complete one after, the incomplete mark
passed through the composite over a DEM; the registry's `web` sources with
EGM96 heights, attribution and errors. `tests/globe`: a source failing the
first time for each tile is asked again and its full heights drawn.
`tests/ui`: the Add Web Terrain dialog's preset, own tiles, refused
address, GeoTIFF address and translations.
For hillshade and profiles: `tests/elevation` checks the path sampling
(even spacing, geodesic legs, exact ends), the level choice, and a
profile across the plane DEM matching GDAL's heights to 5 cm with the
right minimum, maximum, climb and descent; `tests/globe` that the map is
shaded lighter and darker with the hillshade on and unchanged with it off
or the terrain hidden, and golden images of the Alps and Everest on the
map; `tests/ui` the hillshade menu item and style, drawing a line with
clicks and Enter, a profile over a DEM, its summary and CSV, pointing at
the chart, and closing the window clearing the line.
`tests/layers`: visible layers combined top first, the topmost
exaggeration, a hidden DEM left out, a missing file reported.
`tests/ui`: Add Elevation File's layer above the terrain and the readout
in both datums.
`tests/packager` on a real pack: every tile decodes, neighbours (across
the antimeridian too) share their edges, heights at eight known places are
in range, ellipsoidal minus stored height is N.
