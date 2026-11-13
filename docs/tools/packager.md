# The packager

`terrasoar-packager` builds the base pack
([docs/data/base-pack.md](../data/base-pack.md)) and the extra data packs
([docs/design/packs.md](../design/packs.md)) from downloaded sources. It
runs at build time, never in the application.

## Rebuilding the pack

From a build directory, after `make`:

```sh
make basepack
```

This runs `tools/packager/build_basepack.py`, which

1. downloads the sources listed in `tools/packager/basepack.json` into
   `$TS_BASEPACK_SOURCES` (default `<build>/basepack-sources`), about
   740 MB (466 MB of it the ETOPO 2022 GeoTIFF), and checks each against its pinned SHA-256; files already there
   with the right checksum are kept;
2. runs the packager into `<build>/basepack.new`;
3. replaces `<build>/basepack` with it if the packager succeeded.

The application in `<build>/app` finds `<build>/basepack` by itself. The
build takes about 50 s and 900 MB of memory on one core.

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

## Extra packs

The extra packs' configurations are `tools/packager/packs/*.json`. From a
build directory, after `make`:

```sh
make packs
```

runs `tools/packager/build_pack.py` for each. It fetches the sources into
the same directory as the base pack's (they are the same files), builds
the pack into `<build>/packs/<id>`, and writes `<build>/packs/<id>-release`:
the pack's files under flat names a GitHub release accepts
(`licences/gshhg/LICENSE.TXT` becomes `licences.gshhg.LICENSE.TXT`), the
manifest, and `index-entry.json`, the pack's entry for the index. Its
URLs point at the release `<id>-<version>` of `marutan/terrasoar-packs`
(`--base-url` changes that). One pack at a time, with the index updated:

```sh
python3 tools/packager/build_pack.py --build-dir <build> \
    --config tools/packager/packs/shoreline-full.json \
    --index ~/terrasoar-packs/index.json
```

`--index` puts the entry into that `index.json`, replacing the pack's
earlier entry. Nothing is uploaded. To publish a version:

1. Raise `version` in the pack's configuration (versions are compared for
   equality only; any change is an update), and build.
2. Create the release `<id>-<version>` in `marutan/terrasoar-packs` with
   every file of `<build>/packs/<id>-release` except `index-entry.json`
   attached, for example
   `gh release create <id>-<version> --repo marutan/terrasoar-packs <files>`.
3. Commit the updated `index.json` to that repository's main branch. The
   application reads it from there; until then nobody sees the release.

A pack's configuration has the base pack's keys, and `index` (`name`,
`description` and `licence` for the index entry); its `imagery`,
`shoreline` and `geoids` take `replaces`, the base pack dataset they
replace. Each may be left out.

| Pack | Build time | Memory | Size |
| --- | --- | --- | --- |
| `bluemarble-full` | 40 s | 2.3 GB | 67 MB |
| `shoreline-full` | 2 min 15 s | 750 MB | 241 MB |
| `geoid-egm2008-fine` | 25 s | 450 MB | 13.4 MB |
| `geoid-osgm15` | < 1 s | — | 1.5 MB |

The full-resolution Blue Marble is tiled to level 6, a 32768 × 16384
mosaic: a little more than the source's 21600 pixels across, so that no
detail of the source is lost to the tiling.

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
                  ├─ shoreline ► per level of detail (c, l, i, i): GDAL reads
                  │              GSHHS_<r>_L1..L5 straight from the zip
                  │              (/vsizip/) ► rebuild Antarctica's closure
                  │              along the pole ► GEOS: repair invalid polygons
                  │              ► cut down the quadtree to the storage
                  │                level (each level clips its parent's
                  │                pieces, so big polygons are cut once per
                  │                level, not once per tile)
                  │              ► per tile: land = L1 ∪ L5 − L2 ∪ L3 − L4
                  │              ► shoreline.gpkg (GDAL, R-tree, VACUUM)
                  │
                  ├─ countries ► Natural Earth admin-0 (GeoJSON): join the
                  │              halves of countries across 180° ► GEOS:
                  │              widen by 0.05°, simplify within 0.02°,
                  │              check it covers the country ► countries.gpkg
                  │
                  ├─ geoids ───► PROJ's EGM2008 2.5′ and EGM96 15′ grids:
                  │              every step-th sample as int16 cm with a
                  │              scale of 0.01 ► egm2008.tif, egm96.tif
                  │
                  ├─ elevation ► ETOPO 2022 60″ GeoTIFF, row by row: each
                  │              level-6 sample the area-weighted mean of
                  │              the cells within half a spacing ► coarser
                  │              levels by [1 2 1] filter and every other
                  │              sample ► 65 × 65 tiles, quantized and
                  │              encoded ► elevation.sqlite
                  │
                  ├─ licence notices from the zip and tools/packager/notices
                  ├─ manifest.json: every file with size and SHA-256
                  └─ checks: area, coverage, size budget
```

### Antarctica at full resolution

GSHHG gives Antarctica (L5 ice front, L6 grounding line) as two halves,
`-E` and `-W`, each closed along the antimeridian, the South Pole and the
prime meridian. In GSHHG 2.3.7's full resolution two of them are
malformed: `GSHHS_f_L5` 4-E goes down to the pole and back up at 0°
instead of along 180° and −90°, so its last edge cuts straight across the
continent and most of East Antarctica would be lost; 4-W ends at −0.002°
instead of 0°, which would leave a 70 m crack from the coast to the pole.
For every half reaching the pole the packager therefore keeps the
coastline (the vertices off the pole, from one boundary meridian to the
other, snapped onto it within 0.01°) and rebuilds the closure along the
meridians and −90° (`closeHalfAtPole()`). Well-formed halves, all of
crude to high, come out the same.

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
- **Coverage**: every country's outline must be a valid polygon that
  covers the country's own polygons (GEOS `covers`): the outline is what
  an offline region by country downloads, and a missed island would be
  missing offline.
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
| `sizeLimitBytes`, `reservedBytes` | Size budget (75 MB, nothing reserved) |
| `sources[]` | `id`, `file` (name in the sources directory), `url`, `sha256`, `name`, `version`; `notices` (files inside the zip to ship), `localNotices` (files relative to the config to ship) |
| `imagery` | `dataset`, `source`, `name`, `licence`, `attribution`, `replaces` (extra packs), `maxLevel` (at most 8), `tileSize`, `jpegQuality` |
| `shoreline` | `dataset`, `source`, `name`, `licence`, `attribution`, `replaces` (extra packs), `minimumAreaKm2` (drop smaller polygons; 0 keeps all), `lods[]` with `resolution`, `storageLevel`, `minLevel`, `maxLevel` (−1: every deeper level) |
| `countries` | `dataset`, `source` (Natural Earth admin-0 countries, with `ADM0_A3`, `ADMIN`, `ISO_A2_EH`, `ISO_A3_EH`), `name`, `licence`, `attribution`, `bufferDeg` (0.05), `simplifyDeg` (0.02; must be smaller), `exclude` (`ADM0_A3` codes left out: `ATA`) |
| `geoids[]` | `dataset`, `source` (a PROJ GeoTIFF geoid grid), `name`, `licence`, `attribution`, `step` (keep every step-th sample), `replaces` (extra packs: the base pack geoid it is a finer grid of), `copy` (true: ship the grid as it is, for small regional grids with no-data cells; otherwise it is written as 16-bit centimetres) |
| `elevation` | `dataset`, `source` (a global geographic DEM), `name`, `licence`, `attribution`, `maxLevel` (6), `samples` (65 per tile side), `verticalDatum` (a geoid dataset of `geoids`, written to the store) |

### Changing a source

1. Put the new URL and its SHA-256 (`sha256sum`) in `basepack.json`.
2. `make basepack`, and check the report: areas, sizes.
3. Look at the result in the application (and with
   `TS_BASEPACK_DIR=<build>/basepack tests/packager/tst_packager basePack`).
4. Update [base-pack.md](../data/base-pack.md) (contents, sizes, sources,
   licences) and `THIRD_PARTY_NOTICES`.

## Code and tests

The packager's code is in `tools/packager/src/` (`imagerytiler`,
`shorelinebuilder`, `countrybuilder`, `packbuilder`; `main.cpp` is only the command line);
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
directory and fails over budget. A GeoJSON of four countries (one with a
lake, one split at the antimeridian, one without ISO codes, and
Antarctica) checks the country outlines: read back with
`ts::CountryAreas`, the lake is still a hole, the split country is one
piece, and the coast is covered.

With `TS_BASEPACK_DIR` set it also checks a real pack: manifest and
checksums, the budget, the complete pyramid with tiles lining up with
their neighbours (across the antimeridian too), every polygon of every
table valid and inside its tile, and spot checks at levels 0–8 (London is
land; the North Sea, the Pacific and the Caspian Sea are not; the South
Pole is; Wrangel Island at the antimeridian is) and, for the countries,
places inside and outside known countries (Shetland, Réunion and French
Guiana, Chukotka east of 180°, Fiji and the Aleutians across it, Lesotho
not in South Africa), with the time to count a few big countries' tiles
in the log. `TS_PACK_CONFIG` names another configuration, to check an
extra pack the same way. CI's base pack job runs it on the pack it
builds.
