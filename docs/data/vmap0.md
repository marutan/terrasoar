# VMAP0 data packs

NGA's *Vector Map Level 0* (VMAP0) as Terra Soar data packs (Phase 21):
what is in them, where it comes from and on what terms, how it is
converted, how big the packs are, and what is known to be wrong with the
data. How the packs are built: [docs/tools/packager.md](../tools/packager.md#vmap0-packs-phase-21).
How the program shows them: [docs/design/packs.md](../design/packs.md#pack-layers).
For users: [offline-use.md](../user/offline-use.md#vmap0-map-layers).

## The source

VMAP0 is a world vector map at 1:1,000,000 by the US National Imagery and
Mapping Agency (NIMA, now the National Geospatial-Intelligence Agency,
NGA), compiled mostly from the Operational Navigation Charts and updated
for edition 5 (30 September 2000) from DAFIF, WVS+ and the GeoNames
database. It is in the Vector Product Format (VPF, MIL-STD-2407), in four
libraries, one per CD-ROM, each with ten thematic coverages (boundaries,
data quality, elevation, hydrography, industry, physiography, population,
transportation, utilities, vegetation) cut into 15° tiles over land and
30° tiles over the oceans.

NGA no longer publishes VMAP0. The packs are built from the University of
Minnesota's copy of the four CD-ROMs, <https://geo.lib.umn.edu/gisdata/vmap0/>,
downloaded on 2026-11-21:

| File | Library | Bytes | SHA-256 |
| --- | --- | --- | --- |
| `noamer.zip` | NOAMER, North America | 256 476 180 | `45241a1fb60d97eff7f69f7af836275440cc4dae0d8caeca3684b7d3f5dedd94` |
| `eurnasia.zip` | EURNASIA, Europe and North Asia | 250 227 318 | `bd2cad4328851023ede844c4e125ca1e0de10c2813c3d0c07ada25023b55e8f0` |
| `soamafr.zip` | SOAMAFR, South America, Africa and Antarctica | 175 433 948 | `41367cbb4bbd875a2cd9361cd15a45cc031d4c03c238596c0b1ae4510f69d0fe` |
| `sasaus.zip` | SASAUS, South Asia and Australia | 238 665 319 | `b1af09093af52aa9d92edd91465f6311f28d94c7ef4d3b9b4e438a2ed337fbef` |

The configurations pin them (saved as `vmap0-<library>.zip`), and
`fetch_sources.py` downloads and checks them as it does every source;
they are not in the repository. Each zip also holds the reference library
`RFERENCE` (a coarse world overview), which is never used.

## Terms

VMAP0 is a US government work. Its database header gives its
releasability as "UNRESTRICTED"; its FGDC metadata (`FGDC_DAT.TXT`) gives
access constraints "None" and the use constraint "Cite National Imagery
and Mapping Agency". The packs credit it as "Vector Map Level 0 (VMAP0),
NGA" (the attribution in the corner of the view, and the About window's
Data tab).

**ESRI's parts.** Each CD-ROM's `README1.TXT` names some features as the
intellectual property of Environmental Systems Research Institute (ESRI),
under the ESRI Data License Agreement it contains: internal or
non-commercial use only, with the metadata and licence files
accompanying any copy. They are the Boundaries coverage's first-order
administrative boundaries (edges with `f_code` FA000 and `use` 26), its
named administrative areas (faces with `f_code` FA001 and a name), and
the reference library's `libref`, `placenam` and `polbnd` coverages. As
decided on 2026-11-21, the public packs leave all of them out, and the
first two make a pack of their own, `vmap0-admin`, under ESRI's terms
(the reference library is in no pack: it repeats the libraries,
generalised). The split is a filter on `use` in the configurations, and
`tests/vmap0` checks that only `vmap0-admin` keeps those features.

Every pack carries, in its `licences/` folder, `README1.TXT` and
`FGDC_DAT.TXT` of each library it uses, unchanged, and
`tools/packager/notices/vmap0.txt`, which sums up the above. See also
`THIRD_PARTY_NOTICES`.

## The packs

Seventeen packs, one per theme; the three biggest themes are split into
four regional packs, one per library, which the program reads together
(their layers show every region installed). Each pack is one GeoPackage
dataset with its layers and default styles (Add Layer → Data Packs).

| Pack | Classes (VPF coverage/class → table) | Layers |
| --- | --- | --- |
| `vmap0-boundaries` | `bnd/polbndl` without `use` 26 → `boundaries` (international boundaries, claim lines, de facto boundaries, the International Date Line) | International Boundaries: disputed ones (`bst` "In Dispute") in orange, claim lines fainter |
| `vmap0-hydrography-<library>` | `bnd/coastl` → `coastlines`; `hydro/watrcrsl` → `named_rivers`, `rivers`, `intermittent_rivers`; `hydro/inwatera` → `lakes`, `inundation`; `aquecanl` → `canals`; `miscl` → `dams`; `miscp` → `water_points`; `dangerp` → `hazards` | Lakes, Rivers (intermittent fainter, named wider), Coastlines, Canals and Aqueducts, Waterfalls, Springs and Hazards |
| `vmap0-transportation` | `trans/roadl` → `main_roads` (`rtt` 14), `roads`; `railrdl` → `railways`; `traill` → `trails`; `transtrl` → `ferries`, `crossings`; `transtrc` → `crossing_points`; `aerofacp` → `airports`; `rryardp` → `railway_yards`; `mistranl` → `cableways` | Roads by class, Railways, Trails, Ferry Routes, Bridges, Tunnels and Fords, Airports (named), Railway Yards and Cableways |
| `vmap0-population` | `pop/builtupa` → `built_up_areas`; `builtupp` → `built_up_points`; `mispopp` → `settlements` (settlements and native settlements), `landmarks` (the rest) | Built-Up Areas, Places (named), Landmarks |
| `vmap0-elevation-<library>` | `elev/contourl` → `contours`; `elevp` → `spot_heights`; `bnd/depthl` → `depth_contours` | Contours, Spot Heights (labelled with their height), Depth Contours |
| `vmap0-vegetation-<library>` | `veg/treesa`, `cropa`, `grassa`, `swampa`, `tundraa` | Vegetation, a fill colour per kind |
| `vmap0-other` | `phys/landicea`, `seaicea`, `grounda`; `ind/extracta`, `extractp`, `misindp`, `storagep`; `util/pipel`, `utill`, `utilp` | Ice and Snow, Ground Surface (sand, lava), Industry, Pipelines and Power Lines |
| `vmap0-admin` (ESRI) | `bnd/polbndl` with `use` 26 → `first_order_boundaries`; `bnd/polbnda` with a name → `admin_areas` | First-Order Administrative Boundaries, Administrative Areas (a faint fill, names on click) |

**Left out**: the oceans and seas (`bnd/oceansea`: big polygons that only
carry sea names), the administrative areas' label points
(`bnd/polbndp`), the text features of every coverage (`*txt`: names
placed as cartographic text, which the places and airports already
carry), the data quality coverage (`dq`: sources and accuracy of the
charts) and the transportation and utilities `dqline` classes, the
library and tile reference coverages, and the reference library. The
reader reads all of them; `tests/vmap0` reads the data quality coverage.

**Attributes** are written decoded, with readable names: a road has
`feature` "Road", `route_type` "Primary Route", `median`, `existence`,
`accuracy`; a river its `name` and `hydrologic_category`; a contour its
`elevation_m` (a number, metres) and `category`. What Is Here shows them.
Names are as VMAP0 has them: in capitals.

## Levels of detail

Every pack has four levels of detail (the quadtree level the features
are cut to, the display levels it serves, the simplification tolerance):

| Storage level | Shown at | Tolerance | About |
| --- | --- | --- | --- |
| 0 | 0–1 | 0.08° | half a pixel of a level-1 tile shown 1024 pixels wide |
| 2 | 2–3 | 0.02° | |
| 4 | 4–5 | 0.005° | |
| 6 | 6 and deeper | 0.001° (elevation 0.002°) | 110 m (220 m): far within VMAP0's accuracy |

Below the finest level, parts smaller than a square of twice the
tolerance (lines shorter than it) are left out. Each class is shown from
its own level, so the whole Earth isn't cluttered (a level is roughly a
view 20 000 km across at 0, 1500 km at 4, 350 km at 6, 90 km at 8):

| From level | Classes |
| --- | --- |
| 0 | International boundaries |
| 2 | Coastlines, sea ice |
| 3 | Main roads, lakes, ice fields, first-order administrative boundaries and areas (ESRI) |
| 4 | Railways, ferry routes, named rivers, depth contours |
| 5 | Airports, built-up areas and points, vegetation, ground surface |
| 6 | Other roads, canals, settlements, pipelines, power lines |
| 7 | Rivers without a name, intermittent rivers, land subject to inundation, contours, trails, bridges and tunnels, dams, waterfalls and springs, hazards, spot heights, mines and oil fields (areas) |
| 8 | Railway yards, cableways, crossing points, landmarks, industrial points, utility points |

## Sizes

The target set before building: at most 150 MB per pack (the size limit
in every configuration, which fails the build), so each downloads
comfortably; a theme over it is split by library. VMAP0 itself is about
1.8 GB as VPF (920 MB zipped).

| Pack | Size | Build time | Memory |
| --- | --- | --- | --- |
| `vmap0-boundaries` | 4.0 MB | 12 s | 99 MB |
| `vmap0-transportation` | 109.4 MB | 43 s | 849 MB |
| `vmap0-population` | 27.3 MB | 17 s | 216 MB |
| `vmap0-other` | 66.0 MB | 1 min 47 s | 397 MB |
| `vmap0-admin` | 76.6 MB | 46 s | 211 MB |
| `vmap0-hydrography-noamer` | 134.1 MB | 2 min 27 s | 994 MB |
| `vmap0-hydrography-eurnasia` | 98.1 MB | 58 s | 556 MB |
| `vmap0-hydrography-soamafr` | 83.4 MB | 36 s | 443 MB |
| `vmap0-hydrography-sasaus` | 98.1 MB | 50 s | 510 MB |
| `vmap0-elevation-noamer` | 100.5 MB | 38 s | 701 MB |
| `vmap0-elevation-eurnasia` | 106.1 MB | 39 s | 777 MB |
| `vmap0-elevation-soamafr` | 69.0 MB | 27 s | 471 MB |
| `vmap0-elevation-sasaus` | 111.5 MB | 46 s | 837 MB |
| `vmap0-vegetation-noamer` | 120.0 MB | 7 min 41 s | 817 MB |
| `vmap0-vegetation-eurnasia` | 142.7 MB | 7 min 26 s | 730 MB |
| `vmap0-vegetation-soamafr` | 84.9 MB | 3 min 44 s | 472 MB |
| `vmap0-vegetation-sasaus` | 86.3 MB | 3 min 38 s | 433 MB |
| **All seventeen** | **1.52 GB** (1.44 GB without `vmap0-admin`) | about 35 min | |


Built one at a time on the development machine (the packager uses one core); memory is the build's peak.
Without dissolving features by their attributes (the first trial) the
transportation of one library alone was 90 MB: VMAP0 keeps every road
between two junctions as a feature of its own, and each row costs its
attributes and an R-tree entry.

## Drawing them

Every public pack at once (sixteen packs, 30 layers) over Europe, the
densest part of VMAP0, on Mesa llvmpipe (software GL) at 320 × 240
pixels (`tests/globe` `vmap0FrameBudget` with `TS_VMAP0_PACKS_DIR`):

| View | Loaded in | Line segments | Triangles | On the GPU | Frame (fastest quarter) |
| --- | --- | --- | --- | --- | --- |
| Whole Earth | 0.2 s | 4 833 | 0 | 0.3 MB | 8 ms |
| Continent (3000 km) | 3.3 s | 141 612 | 33 272 | 10 MB | 87 ms |
| Region (300 km) | 7.1 s | 327 227 | 139 971 | 38.5 MB | 197 ms |
| Region on the map | 0.1 s | 21 996 | 19 333 | 38.5 MB | 42 ms |

The vector meshes stay within their GPU budget (64 MB) at every view, and
frames within the software budget (100 ms) at the whole Earth and a
continent. A region of Europe with every theme on is over it in software
GL: the main roads, railways, built-up areas and power lines alone are
some 300 000 line segments there (the contours, rivers without names and
land subject to inundation are shown from level 7, not 6, to keep it down;
before that the same view was 525 000 segments and 295 ms). With fewer
themes on, or on a GPU, it is well within. On the clipped test tile every
view is within budget (CI).

## Known data issues

- **Age.** Edition 5 is of 2000, compiled mostly from charts of the 1970s
  to 1990s: roads, railways, towns and boundaries are as they were then.
  Boundaries and names are no statement of any government's position.
- **Accuracy.** VMAP0's data quality areas state about 1–2 km absolute
  horizontal accuracy (1128 m for the test tile). Against GSHHG's
  shoreline in the base pack, the test tile's coastline lies a median
  0.37 km away, 90 % within 1.1 km (`tests/vmap0`); VMAP0 and Blue
  Marble's coasts can differ visibly at close range.
- **Degenerate faces.** A few faces enclose nothing (zero area); they are
  left out and listed in the build report (`degenerate`). Edition 5 has two: a lake in NOAMER's hydrography (`inwatera` 19522) and a wood in its vegetation (`treesa` 20748).
- **Library edges.** VMAP0 cuts its features at library edges as at tile
  edges. In a world pack they are joined again; regional packs each keep
  their side, so a lake across a library edge shows its outline there
  when both regions are installed.
- **Names** are in capitals and transliterated as the charts had them.
- **Contours** are every 1000 ft (305 m) in most places, with heights in
  metres; the elevation packs simplify them to 220 m rather than 110 m
  to stay within the size target.
