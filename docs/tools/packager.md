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

1. downloads the sources listed in `tools/packager/basepack.json` from
   `marutan/terrasoar-packs-sources` into `$TS_BASEPACK_SOURCES` (default
   `<build>/basepack-sources`), about 740 MB (466 MB of it the ETOPO 2022
   GeoTIFF), and checks each against its pinned SHA-256; files already
   there with the right checksum are kept;
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

### Where the sources come from

Every source in a configuration names two places:

```json
{"id": "gshhg", "file": "gshhg-shp-2.3.7.zip",
 "url": "https://github.com/GenericMappingTools/gshhg-gmt/releases/download/2.3.7/gshhg-shp-2.3.7.zip",
 "mirror": "https://github.com/marutan/terrasoar-packs-sources/releases/download/gshhg-2.3.7/gshhg-shp-2.3.7.zip",
 "sha256": "8dbbe7e0…", …}
```

`mirror` is the copy in `marutan/terrasoar-packs-sources`, the repository
that keeps every pack's raw source data with where it came from and its
licence; `fetch_sources.py` downloads only from there. `url` is where the
file was first published; the packager writes it into the pack's manifest
as the source, and `fetch_sources.py --upstream` downloads from it, to add
a new source to the repository. A source without a mirror there fails,
and `tools/checks/check_pack_sources.py` (part of `run_all.py`) checks
every configuration has one. The decision, and how to add a source, are in
[docs/design/packs.md](../design/packs.md#where-the-source-data-is-kept).

`fetch_sources.py` uses `gh release download` for GitHub release assets
when the GitHub CLI is installed (authenticated, so CI is not rate
limited), and plain HTTPS otherwise. A download whose checksum differs is
deleted and the script fails: a changed file never reaches the pack
silently.

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
| `vmap0-*` (17) | 12 s – 7 min 41 s each | up to 1 GB | 4–143 MB each, 1.52 GB in all; see [vmap0.md](../data/vmap0.md#sizes) |

The full-resolution Blue Marble is tiled to level 6, a 32768 × 16384
mosaic: a little more than the source's 21600 pixels across, so that no
detail of the source is lost to the tiling.

## VMAP0 packs (Phase 21)

The `vmap0-*` configurations turn NGA's Vector Map Level 0 into packs of
vector layers; what they hold, their sizes and VMAP0's terms are in
[docs/data/vmap0.md](../data/vmap0.md). They are written by
`tools/packager/make_vmap0_configs.py` from one description (seventeen
packs share their sources, levels and styles): edit it and run it rather
than editing the JSON. Built like any extra pack:

```sh
python3 tools/packager/build_pack.py --build-dir <build> \
    --config tools/packager/packs/vmap0-transportation.json \
    --sources ~/vmap0-sources --index ~/terrasoar-packs/index.json
```

The sources are the four VMAP0 CD-ROMs as zips (920 MB together), the
release `vmap0-5.0` of `marutan/terrasoar-packs-sources`;
`fetch_sources.py` downloads and checks them like every other source. The
packager reads them straight from the zips through GDAL's `/vsizip/`,
without unpacking.

### The VPF reader

`tools/packager/src/vpf/` reads the Vector Product Format (MIL-STD-2407)
itself: GDAL's VPF driver needs the OGDI library, which isn't among our
dependencies, and VPF is fully specified.

- `vpftable`: a table's header (byte order, description, narrative,
  columns) and rows, with every column type — text (`T`, `L`, `N`, `M`,
  fixed or variable length), `S`, `I`, `F`, `R` (one value or several),
  dates, triplet ids (`K`), coordinates in 2D and 3D, single and double
  precision (`C`, `B`, `Z`, `Y`), and `X` — in either byte order. A
  truncated or malformed table is reported with its file and row. The
  variable-length index (`*.?x`) isn't needed to read the rows in order;
  `checkIndex()` compares it with them. Thematic indexes (`*.?ti`,
  inverted lists) are read by `ThematicIndex`. Names are matched without
  regard to case: VMAP0's CD-ROMs are upper case, the standard lower.
- `vpfcoverage`: the database (`dht`, `lat`), each library (`cat`, and
  for a tiled library its tile reference coverage: `tileref.aft` names
  each tile's directory, its face's bounding rectangle in `fbr` gives the
  tile's extent), each coverage (`fcs`, the feature tables, the value
  description tables `char.vdt` and `int.vdt`) and the features of a
  class with their geometry from the primitives of their tile: edges for
  lines, entity or connected nodes for points, text primitives for text,
  and for areas the faces, whose rings (`rng`, outer ring first) are
  walked along the winged-edge topology: forward along an edge with the
  face on its right, then on to its right edge; backward along one with
  the face on its left, then on to its left edge; loop edges and
  dangles by the node they are reached from. A face whose ring doesn't
  close is reported and left out.

`terrasoar-packager --vpf-info <database> [--coverage <name>]` lists a
database's libraries, coverages and classes with the features read from
each, for example
`--vpf-info /vsizip/$HOME/vmap0-sources/soamafr.zip/VMAPLV0`.

### The conversion

`vmap0builder` turns a pack's classes into one GeoPackage (one dataset):

1. **Read** every library's features of the class (the world reference
   library `rference` is never read), keep those passing the class's
   `filters` (raw values; how the ESRI-owned features are kept out of the
   public packs and in `vmap0-admin`), and make their geometry: lines,
   points, polygons with holes, repaired with GEOS where invalid.
2. **Decode** the attributes the class lists: by default a code becomes
   its value description's text (`rtt` 14 → `route_type` "Primary
   Route"); `"kind": "text"` keeps the value, `"number"` keeps a number
   and makes the codes the value tables list (VMAP0's "Unknown" 29999)
   NULL. "UNK" and "No entry present" are NULL.
3. **Dissolve** the class by its attributes: the pieces with the same
   values become one feature, lines merged end to end, areas united.
   This joins what VMAP0 cut along its 15° and 30° tile edges (and
   library edges), and it is what keeps the packs small: VMAP0 stores a
   road between every two junctions as a feature of its own.
4. For each **level of detail** serving the class (from the class's
   `minLevel`), simplify each part (GEOS topology-preserving, within the
   level's `toleranceDeg`), leave out below the finest level the parts
   smaller than a square of twice the tolerance (or lines shorter than
   it), cut them down the quadtree to the level's storage level, and
   write one row per tile and attribute set into `<table>_<storage
   level>`, with a `feature_id`. The tables have no R-tree.

The report gives, per class, the features read, filtered, skipped
(unusable geometry: fails the build), degenerate (faces that enclose
nothing, a known VMAP0 error: left out and listed), joined and written,
and per level the rows, vertices and parts left out as too small.

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
| `imageryVariants` | A list of imagery entries as above, each also with `variantOf` (the base pack dataset) and `variant` (the value that picks it, such as `2004-01`); see [packs.md](../design/packs.md#variants) |
| `shoreline` | `dataset`, `source`, `name`, `licence`, `attribution`, `replaces` (extra packs), `minimumAreaKm2` (drop smaller polygons; 0 keeps all), `lods[]` with `resolution`, `storageLevel`, `minLevel`, `maxLevel` (−1: every deeper level) |
| `countries` | `dataset`, `source` (Natural Earth admin-0 countries, with `ADM0_A3`, `ADMIN`, `ISO_A2_EH`, `ISO_A3_EH`), `name`, `licence`, `attribution`, `bufferDeg` (0.05), `simplifyDeg` (0.02; must be smaller), `exclude` (`ADM0_A3` codes left out: `ATA`) |
| `geoids[]` | `dataset`, `source` (a PROJ GeoTIFF geoid grid), `name`, `licence`, `attribution`, `step` (keep every step-th sample), `replaces` (extra packs: the base pack geoid it is a finer grid of), `copy` (true: ship the grid as it is, for small regional grids with no-data cells; otherwise it is written as 16-bit centimetres) |
| `datumShifts[]` | `dataset` (the id projections ask for, e.g. `ostn15`; the file is `<dataset>.tif`), `source` (a PROJ GeoTIFF horizontal offset grid, shipped as it is), `name`, `licence`, `attribution`; listed in the manifest's `datumShifts` |
| `vmap0` | `dataset`, `sources[]` (the source ids of the VMAP0 zips), `libraries[]` (empty: all but `rference`), `name`, `licence`, `attribution`, `sourceName`, `sourceVersion`, `sourceUrl` (the manifest's dataset source), `lods[]` (`storageLevel`, `minLevel`, `maxLevel`, `toleranceDeg`), `classes[]` (`coverage`, `class`, `table`, `minLevel`, `maxLevel`, `attributes[]` with `column`, `name`, `kind` (`decoded`, `text` or `number`), `filters[]` with `column`, `values`, `exclude`), `layers[]` (`id`, `name`, `tables`, `style` with `labels`, `styleRules`): see [VMAP0 packs](#vmap0-packs-phase-21) |
| `layerGroup` | (top level) The pack's name in the manifest, under which Add Layer lists its layers; regional packs of one theme share it. Default: `index.name` |
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
`shorelinebuilder`, `countrybuilder`, `vmap0builder`, `packbuilder`, and
the VPF reader in `vpf/`; `main.cpp` is only the command line);
`packager.pri` shares it with `tests/packager`, `tests/vmap0` and
`tests/globe` (whose VMAP0 golden images build their packs from the test
tile). It links `tscore` for the
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
