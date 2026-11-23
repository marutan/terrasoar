# More data packs (Phase 26)

Packs with data of their own, built from public domain or freely
licensed sources, besides [VMAP0](vmap0.md). Their configurations are
written by `tools/packager/make_extra_configs.py`; vector packs are read
through GDAL/OGR ([packager.md](../tools/packager.md#vector-datasets)).
Every source is kept, with its licence, in
`marutan/terrasoar-packs-sources`
([packs.md](../design/packs.md#where-the-source-data-is-kept)).

| Pack | Source | Licence | Size | Build |
| --- | --- | --- | --- | --- |
| `naturalearth-10m` | Natural Earth 1:10m vectors 5.1.2, ten themes | Public domain | 76 MB | 47 s, 110 MB |
| `naturalearth-relief` | Natural Earth I with shaded relief and water, 21600 × 10800 | Public domain | 72 MB | 41 s, 2.3 GB |
| `naturalearth-hypsometric` | Cross-blended hypsometric tints with shaded relief and ocean bottom | Public domain | 67 MB | 46 s, 2.3 GB |
| `naturalearth-shaded-relief` | Grey shaded relief | Public domain | 39 MB | 24 s, 2.1 GB |
| `blackmarble` | Black Marble 2012 and 2016, 3 km (13500 × 6750 each) | Public domain (NASA) | 18 MB | 15 s, 720 MB |
| `wdbii` | WDBII borders and rivers from the GSHHG 2.3.7 zip, five resolutions | LGPL-3.0-or-later | 89 MB | 6 s, 92 MB |
| `ourairports` | OurAirports airports, runways and navaids of 2026-11-22 | Public domain (Unlicense) | 16 MB | 4 s |
| `names` | GNS (NGA, outside the US) of 2026-09-03 and GNIS (USGS) of 2026-11-02 | Public domain (US government works) | 324 MB | 12 min, 1.0 GB |
| `elevation-etopo-30s` | ETOPO 2022 surface elevation at 30″ | CC0-1.0 (NOAA) | 461 MB | 7.5 min |
| `gebco-w180-w90`, `-w90-0`, `-0-e90`, `-e90-e180` | The GEBCO 2026 Grid at 15″, one quarter of the world each | Public domain (GEBCO) | 356, 374, 385 and 396 MB | 7–8 min each, 5.2 GB (most of it the mapped grid) |

Sizes are the packs as downloaded; build times and peak memory on one
core.

## Natural Earth map layers

Rivers and lake centrelines, lakes, populated places, roads and ferries,
railways, airports, ports, time zones, state and province boundary lines,
and peaks and regions. Each theme is split by Natural Earth's own
suggested zoom (`min_zoom`; a web map's zoom z is about Terra Soar's level
z − 1) into tables shown from levels 0, 3, 5 and 6, so the whole globe
shows only the major rivers and cities and detail comes in as you zoom
in. Levels of detail at storage levels 0, 2, 4 and 6, simplified within
0.05°, 0.02°, 0.005° and 0.001°: Natural Earth's 1:10m is accurate to
about a kilometre. Names are labels on the places, airports, ports and
peaks.

## Natural Earth raster maps, Black Marble

Imagery datasets of the pack's own, each with an imagery layer
(`pack:<dataset>:image`): Natural Earth's maps tiled to level 6 (32768 ×
16384, like the full-resolution Blue Marble), Black Marble to level 5
(16384 × 8192, a little more than its 13500 pixels across). Natural
Earth's shaded relief is grey, read into all three channels. Black Marble
2012 and 2016 are two layers of one pack.

## WDBII borders and rivers

The CIA's World Data Bank II, as GSHHG distributes it: national borders,
internal borders (US states, Canadian provinces and the like), maritime
boundaries, rivers by WDBII's levels (1 major to 4 minor, each its own
table shown from levels 0, 3, 5 and 7), intermittent rivers and canals.
GSHHG gives each at five resolutions (crude to full), taken one per level
of detail (storage levels 0, 2, 4, 6, 8) rather than simplified from the
full one. The borders are those of the 1980s (the Soviet Union and
Yugoslavia whole); for current borders use Natural Earth or VMAP0's
boundaries.

## OurAirports

Airports by size (large from level 3, medium from 5, small and seaplane
bases from 7), heliports from 8, navigation aids from 6, and runways from
8 as lines between their ends. Of 47,366 runways, 32,464 have no ends
recorded and are left out (counted as without geometry); closed runways
and closed airports are left out. The data change daily; the pack is a
snapshot of the repository at one commit.

## Place names

Points with their names as labels: populated places and major features,
the approved name of each only (GNS name type `N`; no variants or
non-Roman scripts). Outside the United States the GEOnet Names Server
(NGA's Populated Places, Hypsographic, Hydrographic and Undersea feature
class files); in it, USGS GNIS Domestic Names (populated places, summits,
ranges, lakes, bays, seas, islands, isthmuses, capes, glaciers, falls;
features marked historical left out).

GNS lists for each name the map scale bands it is shown at (`display`, 1
the largest scale to 9 the smallest); the more bands, the more important
the name, so its tables follow them: capitals from level 2, first-order
administrative seats from 4, places shown at band 9 from 5, at band 7
from 6, at band 5 from 7; places shown only at the largest scales
(bands 1 to 4), and abandoned, historical and destroyed places, are left
out. Mountains, islands, lakes and bays are tiered the same way. GNIS
has no such ranking: its places show from 7, summits and lakes from 8.

NGA's server (`geonames.nga.mil`) answers few networks, so the GNS files
were taken from the Internet Archive's capture of 2026-09-03 of NGA's own
files; their SHA-1 is the one the archive recorded when it captured them.

## Terrain: ETOPO 2022 at 30″, GEBCO 2026 at 15″

`elevation-etopo-30s` replaces the base pack's terrain (`"replaces":
"elevation"`): the Terrain layer reads it while it is installed, with no
change to projects. Tiles of 85 samples a side, not the base pack's 65,
to level 8: 84 intervals of a level-8 tile are 30.1″, the source's own
spacing (65 samples would give 40″, and level 9 with 65 would be more than
a release file may be). Heights on EGM2008, as the base pack's.

The GEBCO packs are one DEM of their own, `gebco-2026`, split by
longitude into four quarters of the world (180° W–90° W, 90° W–0°, 0°–90°
E, 90° E–180°), each a pack with the same dataset and layer, **GEBCO 2026
terrain and sea floor** (`pack:gebco-2026:terrain`). The program reads
the installed quarters together, each tile from the pack that has it
(`ProviderRegistry`, `CompositeElevationProvider`); put the layer above
Terrain and it shows GEBCO's 15″ heights where a quarter is installed and
the Terrain layer's elsewhere. Levels 1 to 9 only (a quarter is whole
level-1 tiles), 85 samples a side: 15.1″ at level 9, GEBCO's spacing.

GEBCO publishes the grid as a 4.2 GB zip of eight GeoTIFFs, twice what a
release file may be, from a host that doesn't answer every network: it is
kept in `terrasoar-packs-sources` in three parts, downloaded on a GitHub
runner by that repository's "Mirror a large source" workflow, and
`fetch_sources.py` joins them (`"parts": 3`). The packager mosaics the
eight tiles as a GDAL VRT (`"members"`) and reads only the columns of its
quarter. A quarter's 15″ grid is 21505 × 43009 samples, 3.7 GB as floats:
grids over 1 GB are kept in a memory-mapped temporary file rather than in
memory, so the build fits a machine with less free memory.
