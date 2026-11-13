# Vertical datums

Heights mean different things depending on what they are measured from.
Terra Soar keeps every height above the **WGS 84 ellipsoid** internally,
and turns heights from sources measured from somewhere else into that
form as they are read. Code: `core/include/ts/elevation/geoidmodel.h`,
`sqliteelevationprovider.h`. The terrain itself is in
[terrain.md](terrain.md).

## Ellipsoidal and orthometric heights

```
        terrain ──────╮      h = H + N
                      │ H    h  height above the ellipsoid (ellipsoidal)
  geoid (sea level) ──┤      H  height above the geoid ("above sea level",
                      │ N       orthometric)
  WGS 84 ellipsoid ───╯      N  the geoid's height above the ellipsoid
                                (undulation), −107 m to +86 m
```

- The **ellipsoid** is the smooth mathematical surface the globe is drawn
  on: positions, the camera and the tile meshes are all relative to it,
  and GNSS receivers report heights above it.
- The **geoid** is the surface the oceans would take at rest, where
  "above sea level" is measured from. It rises and falls with the Earth's
  gravity field: 46 m above the ellipsoid in London, 107 m below it south
  of India, 86 m above it in New Guinea.
- Maps, DEMs and people mostly give heights above the geoid. ETOPO 2022,
  the base pack's DEM, is relative to EGM2008.

Mixing them up puts the terrain tens of metres too high or too low:
London would be drawn 46 m under the sea.

## Geoid models

| Model | Grid | Where | Size | Accuracy of the grid |
| --- | --- | --- | --- | --- |
| EGM2008 | 5′ | base pack, `egm2008.tif` | 5.4 MB | within 0.4 m of the 2.5′ grid at the reference points; up to about 0.5 m in high mountains |
| EGM96 | 15′ | base pack, `egm96.tif` | 0.9 MB | within 6 cm of NGA's published test values |
| EGM2008 | 2.5′, 1′ | downloadable packs (Phase 7d) | | |
| OSGM15 | | downloadable pack (Phase 7d): Ordnance Datum Newlyn, Great Britain | | |

The grids are PROJ's GeoTIFF grids (`us_nga_egm08_25.tif`,
`us_nga_egm96_15.tif` from cdn.proj.org, pinned in
`tools/packager/basepack.json`), reduced by the packager: EGM2008 to every
other sample (2.5′ → 5′), both stored as 16-bit integer centimetres with a
scale of 0.01, which PROJ reads like floats. That makes EGM2008 5.4 MB
instead of 80 MB, and costs at most half a centimetre.

`ts::GeoidModel` evaluates a grid with PROJ's `vgridshift`
(bilinear interpolation), which is what "via PROJ" in the roadmap asks
for. Two details:

- The grid is found by file name through the context's search path (the
  pack's directory, then PROJ's own for `proj.db`), never by a path inside
  the PROJ string: the Windows install directory, `C:\Program Files\Terra
  Soar\…`, has spaces, and PROJ strings split on them.
- Forward, `vgridshift` with `+multiplier=1` adds the grid's value, so
  transforming a height of 0 gives N. One PJ object is shared behind a
  mutex; a tile's 4225 samples go through in one `proj_trans_array()`
  call (about 1 ms).

## Where datums are converted

- **Elevation sources** declare their datum. A height grid store has a
  `vertical_datum` row in its metadata naming a geoid dataset of the same
  pack (`egm2008` for the base pack's DEM), or none for heights already
  above the ellipsoid. `ts::SqliteElevationProvider` adds N to every
  sample as it loads a tile, so `ts::HeightTile` is always ellipsoidal,
  as the renderer and picking need. `ProviderRegistry::elevation()` finds
  the geoid dataset the store names.
- **DEM files** (`ts::GdalElevationProvider`) name their datum in the
  layer's source: `egm2008` or `egm96` (N added from the base pack's
  grid) or `ellipsoid` (left as they are). Add Elevation File takes it
  from the file's compound CRS where there is one, else asks. Files on a
  national datum (e.g. Ordnance Datum Newlyn) are within a metre or so of
  EGM2008; the OSGM15 pack (Phase 7d) will convert them exactly.
- **What the user sees**: the pointer readout gives heights above sea
  level (the tile's height minus its sea level, N), or above the
  ellipsoid with View → Heights Above the Ellipsoid. `ProviderRegistry::geoid()`
  gives the model to subtract N where a tile doesn't carry it.

## Tests

`tests/elevation` checks a fixture geoid grid of a known function through
PROJ, from a directory with a space in its name, and that the provider
adds N to stored heights. `tests/packager`, on a real base pack
(`TS_BASEPACK_DIR`), checks NGA's six EGM96 test points (within 0.1 m),
nine EGM2008 points against the full 2.5′ grid (within 0.4 m), and that
ellipsoidal height minus orthometric height is N at known places.
