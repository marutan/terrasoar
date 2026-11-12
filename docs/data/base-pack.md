# The base pack

The base pack is the data Terra Soar ships with, so that the globe looks
right on first launch with no network: Blue Marble imagery and the world's
shorelines as land polygons, and the outlines of the countries for choosing
an offline region by country. It is built at build time by the packager
([docs/tools/packager.md](../tools/packager.md)) from pinned downloads, not
stored in Git.

## Contents

| File | Dataset | What | Size |
| --- | --- | --- | --- |
| `bluemarble.sqlite` | `bluemarble` | Blue Marble Next Generation (July 2004, topography and bathymetry) as JPEG tiles of 256 × 256 px, quadtree levels 0–4 (682 tiles; level 4 is 8192 × 4096 px for the world, about 4.9 km per pixel) | 6.1 MB |
| `shoreline.gpkg` | `shoreline` | GSHHG 2.3.7 land polygons at four resolutions, cut to quadtree tiles | 45.6 MB |
| `countries.gpkg` | `countries` | Natural Earth 5.1.2 admin-0 countries (257; Antarctica left out), each widened by 0.05° (about 5 km) and simplified within 0.02° | 1.4 MB |
| `licences/gshhg/` | | GSHHG's `LICENSE.TXT`, the LGPL v3 (`COPYING.LESSERv3`) and the GPL v3 it builds on (`gpl-3.0.txt`) | 44 kB |
| `manifest.json` | | Files with SHA-256 and size, datasets with source, licence and attribution | 3 kB |

### Shoreline levels of detail

GSHHG comes in five resolutions, each simplified by its authors with
checked topology. The pack uses four, each cut to the tiles of one quadtree
level and shown over a range of display levels:

| Table | GSHHG resolution | Tolerance | Stored at level | Shown at levels | Tiles | Vertices |
| --- | --- | --- | --- | --- | --- | --- |
| `land_c` | crude | 25 km | 0 | 0–1 | 2 | 12 620 |
| `land_l` | low | 5 km | 2 | 2–3 | 32 | 90 671 |
| `land_i` | intermediate | 1 km | 4 | 4–5 | 384 | 457 123 |
| `land_h` | high | 0.2 km | 6 | 6 and deeper | 3 955 | 1 964 004 |

The full resolution (about 10 million vertices) does not fit the budget;
it is the downloadable pack `shoreline-full`
([packs.md](../design/packs.md#the-first-packs)).

Land is GSHHG's hierarchy resolved into one polygon set: land (level 1)
and Antarctica by its ice front (level 5), minus lakes (2), plus islands in
lakes (3), minus ponds on those islands (4). The grounding line (level 6)
is not used.

### Countries

`countries.gpkg` has one table, `countries`: `id` (Natural Earth's
`ADM0_A3`, e.g. `GBR`), `name` (`ADMIN`, e.g. "United Kingdom"), `iso_a2`
and `iso_a3` (empty where Natural Earth has none) and a MULTIPOLYGON. It
exists to choose an offline region by country
([offline-regions.md](../design/offline-regions.md#countries)), not to
draw borders, so each outline is made to cover the whole country:

- the country's polygons widened by 0.05° and then simplified within
  0.02°, which keeps the original inside (the packager checks that every
  outline covers its country, and fails otherwise). Islands near each
  other merge; a country's lakes stay holes only if wider than 0.1°;
- a country on both sides of the antimeridian (Russia, Fiji, the United
  States, Kiribati, New Zealand) has its western parts moved by 360°, so
  its outline is one piece with longitudes past 180°;
- Antarctica is left out: a polygon can't go round a pole, and a
  rectangle reaching the pole covers it anyway.

The 1:10m scale has the small islands the coarser ones drop. Natural
Earth draws borders as they are on the ground ("de facto"); the outlines
are for choosing an area, not a statement about any border.

There are no separate shoreline lines: they would double the size. The
shore is the polygons' boundary without the segments lying on their tile's
edges; every artificial edge (tile cuts, the antimeridian, the south pole)
is such a segment. The vector renderer draws outlines that way
([vector-rendering.md](../design/vector-rendering.md)).

## Sources and licences

| Dataset | Source | Licence | Attribution shown |
| --- | --- | --- | --- |
| Blue Marble | NASA Earth Observatory, Blue Marble Next Generation, `world.topo.bathy.200407.3x21600x10800.jpg` from `assets.science.nasa.gov` | Public domain (NASA); NASA asks for credit | "Blue Marble: NASA Earth Observatory" |
| Shoreline | GSHHG 2.3.7 by Paul Wessel and Walter H. F. Smith (shapefiles, `gshhg-shp-2.3.7.zip` from the GMT project's GitHub release) | LGPL v3 or later | "Shoreline: GSHHG 2.3.7, P. Wessel and W. H. F. Smith (LGPL)" |
| Countries | Natural Earth 1:10m admin-0 countries 5.1.2 (`geojson/ne_10m_admin_0_countries.geojson` at tag `v5.1.2` of `nvkelso/natural-earth-vector`) | Public domain; credit requested | "Countries: Made with Natural Earth" (in the manifest; not drawn) |

The roadmap planned NGA's World Vector Shoreline (WVS). Its original
distribution is no longer available (NOAA's datasheet and the NGDC
Coastline Extractor are gone), so the pack uses GSHHG, which is derived
from WVS and the CIA World Data Bank II. GSHHG is LGPL data: the pack
carries its licence texts, the manifest names the exact source file and
checksum, and the packager's source (this repository) is the "source" of
the modified form.

URLs and SHA-256 sums are pinned in `tools/packager/basepack.json`.

## Size budget

The pack may be at most **75 MB**, with **20 MB reserved** for the coarse
global DEM of Phase 7, so today's contents must fit in **55 MB**
(`sizeLimitBytes` and `reservedBytes` in `basepack.json`). The packager
fails when the pack is over budget, and CI's base pack job runs it on
every change.

| Part | Bytes |
| --- | --- |
| Shoreline | 45.6 MB |
| Blue Marble | 6.1 MB |
| Countries | 1.4 MB |
| Licences, manifest | < 0.1 MB |
| **Total** | **53.2 MB** of 55 MB |

Phase 7 needs more than the reserve: the DEM (about 20 MB) and the
EGM2008 and EGM96 geoid grids (about 13 MB). Dropping the high-resolution
shoreline (`land_h`), now that `shoreline-full` has it and more, is the
obvious way to make room.

## File formats

**Tile store** (`*.sqlite`, read by `ts::SqliteTileImageryProvider`): a
SQLite file with `metadata(name, value)` — `scheme` (`ts-epsg4326`),
`format` (`jpeg`), `tile_size`, `min_level`, `max_level`, `name` — and
`tiles(level, x, y, data)` keyed by `ts::TileKey` (rows from the north).

**Tiled GeoPackage** (`*.gpkg`, read by `ts::GeoPackageFeatureProvider`): a
standard GeoPackage (EPSG:4326, R-tree spatial index on every table) with an
attributes table `ts_feature_tables(table_name, geometry, storage_level,
min_level, max_level)`. Each listed table holds one multipolygon per tile,
with integer columns `tile_level`, `tile_x`, `tile_y` (indexed). For a key
at display level *n*, the provider reads the tables serving *n* and returns
the features of the key's ancestor at their storage level. GIS programs
(QGIS, `ogrinfo`) open the file as it is.

**Manifest** (`manifest.json`, read by `ts::PackManifest`): format 1;
`id` (`terrasoar-base`), `version`, `files` (path, sha256, bytes),
`datasets` (id, kind — `imagery`, `features` (tiled) or `areas` (the
countries) — file, name, licence, attribution, source with name, version,
url and sha256, and for extra packs `replaces`) and `notices`. Paths are relative, `/`-separated
and may not leave the pack directory.

## In the application

The application looks for the pack, in order: `$TS_BASEPACK_DIR` (only
there, if set); `<app dir>/basepack` (Windows installs);
`<app dir>/../share/terrasoar/basepack` (Linux installs);
`<app dir>/../basepack` (a build tree after `make basepack`). The first
directory with a manifest whose id is `terrasoar-base` wins
(`ts::findBasePack()`).

On start-up the main window reads the manifest, makes its datasets
available to layer sources `{"kind": "basepack", "dataset": "<id>"}` and
starts a new project with the base layers: the graticule and the
shoreline over Blue Marble (a Land layer, the same polygons filled, is
in Add Layer). The view credits every dataset on screen in its
bottom-right corner, with the attribution from the manifest. A
background thread then
checks every file's size and SHA-256 (a fraction of a second). A missing or changed
file is reported in the status bar ("The base pack is damaged
(shoreline.gpkg)…") and logged; the providers turn unreadable tiles into
"no data", so a damaged pack never crashes the program. Without a pack
(a development build before `make basepack`) the project starts with the
graticule over the checkerboard and the status bar says so.
