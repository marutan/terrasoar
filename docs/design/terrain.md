# Terrain

How heights are stored, read, sampled and drawn. Phase 7b covered the
data (the base pack's global DEM, the elevation provider, height
sampling), Phase 7c the drawing (displaced tile meshes, skirts,
exaggeration, the morph, features on the terrain, picking). Further
sources (web terrain tiles, GeoTIFF/COG files, priority between sources)
are Phase 7d. For users: [terrain.md](../user/terrain.md).
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
`tests/packager` on a real pack: every tile decodes, neighbours (across
the antimeridian too) share their edges, heights at eight known places are
in range, ellipsoidal minus stored height is N.
