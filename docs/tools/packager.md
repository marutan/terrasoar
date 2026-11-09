# The packager

`terrasoar-packager` builds the base pack
([docs/data/base-pack.md](../data/base-pack.md)) from downloaded sources.
It runs at build time, never in the application.

## Rebuilding the pack

From a build directory, after `make`:

```sh
make basepack
```

This runs `tools/packager/build_basepack.py`, which

1. downloads the sources listed in `tools/packager/basepack.json` into
   `$TS_BASEPACK_SOURCES` (default `<build>/basepack-sources`), about
   176 MB, and checks each against its pinned SHA-256; files already there
   with the right checksum are kept;
2. runs the packager into `<build>/basepack.new`;
3. replaces `<build>/basepack` with it if the packager succeeded.

The application in `<build>/app` finds `<build>/basepack` by itself. The
build takes about 40 s and 400 MB of memory on one core.

Run the steps separately to use other directories:

```sh
python3 tools/packager/fetch_sources.py ~/terrasoar-sources
<build>/tools/packager/terrasoar-packager \
    --config tools/packager/basepack.json \
    --sources ~/terrasoar-sources --output /tmp/basepack --report report.json
```

`fetch_sources.py` uses `gh release download` for GitHub release assets
when the GitHub CLI is installed (authenticated, so CI is not rate
limited), and plain HTTPS otherwise. A download whose checksum differs is
deleted and the script fails: a changed upstream file never reaches the
pack silently.

## What it does

```
basepack.json ──► check sources' SHA-256
                  │
                  ├─ imagery ──► GDAL reads the 21600 × 10800 JPEG into the
                  │              8192 × 4096 level-4 mosaic (area average)
                  │              ► cut 256-px tiles; halve with a 2 × 2 box
                  │                for each level up to 0 ► JPEG q85
                  │              ► bluemarble.sqlite
                  │
                  ├─ shoreline ► per resolution (c, l, i, h): GDAL reads
                  │              GSHHS_<r>_L1..L5 straight from the zip
                  │              (/vsizip/) ► GEOS: repair invalid polygons
                  │              ► cut down the quadtree to the storage
                  │                level (each level clips its parent's
                  │                pieces, so big polygons are cut once per
                  │                level, not once per tile)
                  │              ► per tile: land = L1 ∪ L5 − L2 ∪ L3 − L4
                  │              ► shoreline.gpkg (GDAL, R-tree, VACUUM)
                  │
                  ├─ licence notices from the zip and tools/packager/notices
                  ├─ manifest.json: every file with size and SHA-256
                  └─ checks: area, size budget
```

### Checks that fail the build

- **Area**: for every level of detail, the land area written must be
  within 0.1 % of the source polygons' (L1 + L5 − L2 + L3 − L4) as read.
  Areas are on the WGS 84 ellipsoid with rhumb-line edges
  (`GeographicLib::PolygonAreaRhumb`), the right model for tile cuts along
  parallels. In practice cutting and merging lose less than 0.001 %. The
  report also gives GSHHG's own area total; its fields at coarse
  resolutions sometimes describe the full-resolution polygon (crude is
  0.44 % off; high 0.007 %), so it is information, not a check. GSHHG
  gives both halves of a polygon split at the antimeridian (Eurasia,
  Antarctica) the whole polygon's area; the report counts it once.
- **Size**: the pack must fit `sizeLimitBytes − reservedBytes`.
- **Sources**: their SHA-256 must match `basepack.json`
  (`--no-verify-sources` skips this, for development only).

### The report

`--report` writes, and the packager prints, JSON with the imagery tile
count and bytes, and per shoreline resolution the source polygons,
repaired polygons, tiles, vertices, the land area written, the source
area and GSHHG's area.

## Configuration

`tools/packager/basepack.json`:

| Key | Meaning |
| --- | --- |
| `id`, `version` | Copied to the manifest |
| `sizeLimitBytes`, `reservedBytes` | Size budget (75 MB, 20 MB kept for the DEM) |
| `sources[]` | `id`, `file` (name in the sources directory), `url`, `sha256`, `name`, `version`; `notices` (files inside the zip to ship), `localNotices` (files relative to the config to ship) |
| `imagery` | `dataset`, `source`, `name`, `licence`, `attribution`, `maxLevel`, `tileSize`, `jpegQuality` |
| `shoreline` | `dataset`, `source`, `name`, `licence`, `attribution`, `minimumAreaKm2` (drop smaller polygons; 0 keeps all), `lods[]` with `resolution`, `storageLevel`, `minLevel`, `maxLevel` (−1: every deeper level) |

### Changing a source

1. Put the new URL and its SHA-256 (`sha256sum`) in `basepack.json`.
2. `make basepack`, and check the report: areas, sizes.
3. Look at the result in the application (and with
   `TS_BASEPACK_DIR=<build>/basepack tests/packager/tst_packager basePack`).
4. Update [base-pack.md](../data/base-pack.md) (contents, sizes, sources,
   licences) and `THIRD_PARTY_NOTICES`.

## Code and tests

The packager's code is in `tools/packager/src/` (`imagerytiler`,
`shorelinebuilder`, `packbuilder`; `main.cpp` is only the command line);
`packager.pri` shares it with `tests/packager`. It links `tscore` for the
tile store and GeoPackage writers, the manifest and the dependency
wrappers.

`tests/packager` builds generated fixtures — shapefiles with an island
across the prime meridian and the equator holding a lake with an islet, a
polygon split at the antimeridian, an Antarctica-like cap down to the
pole, and test images — and checks that the land polygons are valid
(GEOS `isValid`), closed and inside their tiles, that the areas match,
that every tile of the pyramid exists, decodes and is its exact square of
the mosaic, and that a whole pack builds, verifies, refuses a full output
directory and fails over budget.

With `TS_BASEPACK_DIR` set it also checks a real pack: manifest and
checksums, the budget, the complete pyramid with tiles lining up with
their neighbours (across the antimeridian too), every polygon of every
table valid and inside its tile, and spot checks at levels 0–8 (London is
land; the North Sea, the Pacific and the Caspian Sea are not; the South
Pole is; Wrangel Island at the antimeridian is). CI's base pack job runs
it on the pack it builds.
