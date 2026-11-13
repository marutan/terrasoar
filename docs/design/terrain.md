# Terrain

How heights are stored, read and sampled. Phase 7b covers the data:
the base pack's global DEM, the elevation provider and height sampling.
Drawing the terrain (displaced tile meshes, skirts, exaggeration, the
morph) is Phase 7c and further sources (web terrain tiles, GeoTIFF/COG
files, priority between sources) Phase 7d; this document grows with them.
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

## Tests

`tests/elevation`: the quantization steps; encoding round trips (and
damaged, truncated or mismatched data refused); bilinear sampling and
resampling exact on a plane; a fixture geoid through PROJ; the provider
adding N, interpolating deeper keys, refusing a store without its geoid
or of images; the registry finding the elevation and geoid datasets.
`tests/packager` on a real pack: every tile decodes, neighbours (across
the antimeridian too) share their edges, heights at eight known places are
in range, ellipsoidal minus stored height is N.
