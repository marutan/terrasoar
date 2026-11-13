# The base pack

The base pack is the data Terra Soar ships with, so that the globe looks
right on first launch with no network: Blue Marble imagery, the world's
shorelines as land polygons, a global DEM with the geoid grids that turn
its heights into heights above the ellipsoid, and the outlines of the
countries for choosing an offline region by country. It is built at build time by the packager
([docs/tools/packager.md](../tools/packager.md)) from pinned downloads, not
stored in Git.

## Contents

| File | Dataset | What | Size |
| --- | --- | --- | --- |
| `bluemarble.sqlite` | `bluemarble` | Blue Marble Next Generation (July 2004, topography and bathymetry) as JPEG tiles of 256 × 256 px, quadtree levels 0–4 (682 tiles; level 4 is 8192 × 4096 px for the world, about 4.9 km per pixel) | 5.8 MB |
| `shoreline.gpkg` | `shoreline` | GSHHG 2.3.7 land polygons at three resolutions, cut to quadtree tiles | 20.1 MB |
| `countries.gpkg` | `countries` | Natural Earth 5.1.2 admin-0 countries (257; Antarctica left out), each widened by 0.05° (about 5 km) and simplified within 0.02° | 1.4 MB |
| `elevation.sqlite` | `elevation` | ETOPO 2022 (land, ice surface and sea floor) as 65 × 65 height grids, quadtree levels 0–6 (10 922 tiles; 4.9 km apart at level 6), heights above EGM2008 ([terrain.md](../design/terrain.md)) | 29.3 MB |
| `egm2008.tif` | `egm2008` | EGM2008 geoid undulation, 5′ grid, integer centimetres ([vertical-datums.md](../design/vertical-datums.md)) | 5.4 MB |
| `egm96.tif` | `egm96` | EGM96 geoid undulation, 15′ grid, integer centimetres | 0.9 MB |
| `licences/gshhg/` | | GSHHG's `LICENSE.TXT`, the LGPL v3 (`COPYING.LESSERv3`) and the GPL v3 it builds on (`gpl-3.0.txt`) | 44 kB |
| `manifest.json` | | Files with SHA-256 and size, datasets with source, licence and attribution | 3 kB |

### Shoreline levels of detail

GSHHG comes in five resolutions, each simplified by its authors with
checked topology. The pack uses three, each cut to the tiles of one
quadtree level and shown over a range of display levels; the
intermediate resolution is stored twice, the second time cut to the
smaller tiles of level 6 that the deep levels need (the renderer's cells
are 1/16 of a stored tile, and the larger cells of level 4 would sag
hundreds of metres below the surface when zoomed in):

| Table | GSHHG resolution | Tolerance | Stored at level | Shown at levels | Tiles | Vertices |
| --- | --- | --- | --- | --- | --- | --- |
| `land_c` | crude | 25 km | 0 | 0–1 | 2 | 12 620 |
| `land_l` | low | 5 km | 2 | 2–3 | 32 | 90 671 |
| `land_i` | intermediate | 1 km | 4 | 4–5 | 384 | 457 123 |
| `land_i_6` | intermediate | 1 km | 6 | 6 and deeper | 3 926 | 487 117 |

The high (0.2 km) and full resolutions are the downloadable pack
`shoreline-full` ([packs.md](../design/packs.md#the-first-packs)), which
takes the place of this dataset when installed. The base pack had the high
resolution until version 2 (2026-11-12); it went to make room for the
terrain data of Phase 7 (below).

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
| Elevation | ETOPO 2022 v1, 60 arc-second surface elevation GeoTIFF, by NOAA National Centers for Environmental Information (`ETOPO_2022_v1_60s_N90W180_surface.tif` from `ngdc.noaa.gov`) | Public domain (US government); NOAA asks for credit | "Elevation: ETOPO 2022, NOAA NCEI" |
| Geoids | EGM2008 and EGM96 by the US National Geospatial-Intelligence Agency, as PROJ's grids (`us_nga_egm08_25.tif`, `us_nga_egm96_15.tif` from cdn.proj.org) | Public domain | "Geoid: EGM2008, NGA", "Geoid: EGM96, NGA" (in the manifest; not drawn) |
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

The pack may be at most **75 MB** (`sizeLimitBytes` in `basepack.json`;
`reservedBytes`, room kept for later contents, is now 0). The packager
fails when the pack is over budget, and CI's base pack job runs it on
every change.

| Part | Bytes |
| --- | --- |
| Elevation | 29.3 MB |
| Shoreline | 20.1 MB |
| Geoids | 6.3 MB |
| Blue Marble | 5.8 MB |
| Countries | 1.4 MB |
| Licences, manifest | < 0.1 MB |
| **Total** | **63.0 MB** of 75 MB |

History: until version 2 the shoreline had the high resolution (45.6 MB,
the pack 53.2 MB) and 20 MB were reserved for the DEM. Phase 7's DEM and
geoids needed more, so the high resolution moved to `shoreline-full`
(decided 2026-11-12) and version 2 reserved 35 MB. Version 3 has the DEM
(29.3 MB, more than the 20 MB first planned: a 4.9 km grid keeps real
detail, see [terrain.md](../design/terrain.md)) and the geoids, and
releases the reserve. Without `shoreline-full`, coasts are drawn from the
1 km intermediate resolution at every level from 4 down.

## File formats

**Tile store** (`*.sqlite`, read by `ts::SqliteTileImageryProvider`): a
SQLite file (64 KB pages) with `metadata(name, value)` — `scheme`
(`ts-epsg4326`), `format` (`jpeg`), `tile_size`, `min_level`, `max_level`,
`name` — and `tiles(level, x, y, data)` keyed by `ts::TileKey` (rows from
the north). The DEM is the same kind of store with `format` `heights`,
`tile_size` the samples per side (65), `vertical_datum` (`egm2008`, the
geoid dataset its heights are above) and `data` an encoded height grid
(read by `ts::SqliteElevationProvider`, [terrain.md](../design/terrain.md)).

**Geoid grids** (`*.tif`, read by `ts::GeoidModel` through PROJ): PROJ's
GeoTIFF grid format, 16-bit integers with a scale of 0.01 (centimetres),
DEFLATE compressed.

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
