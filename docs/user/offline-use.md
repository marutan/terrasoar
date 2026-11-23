# Using Terra Soar offline

Terra Soar works without a network. The built-in layers (Blue Marble, the
shoreline, the graticule) are always on your computer. Layers from web map
servers (WMS, WMTS, XYZ tiles and WFS) need the server, but you can download an area of
one in advance as an **offline region** and view it later on a plane, at sea or
in the field.

## Downloading a region

1. Add the WMS, WMTS, XYZ or WFS layer you want to keep (see
   [Adding WMS layers](adding-wms-layers.md),
   [Adding WMTS layers](adding-wmts-layers.md),
   [Adding XYZ tile layers](adding-xyz-layers.md) and
   [Adding WFS layers](adding-wfs-layers.md)).
2. Choose **File → Offline Regions…** and press **New Region…**.
3. Give the region a **name** and pick the **layer**.
4. Choose the **area**:
   - **Current View** takes the part of the Earth the view shows now (it
     is filled in when the dialog opens).
   - **Draw Rectangle**: the dialog steps aside; drag across the area on
     the globe or map.
   - **Draw Polygon**: click each corner of the area, then double-click
     (or press Enter) to finish. Backspace takes the last corner back.
   - **Or country**: type part of a country's name ("king" finds the
     United Kingdom) and pick it from the list. The area is the whole
     country with its islands and about 5 km around its coasts and
     borders, so nothing at the edge is missed. The region takes the
     country's name, unless you have typed one. Antarctica isn't in the
     list: use a rectangle that reaches the South Pole instead.

   While drawing, the right mouse button and the wheel still move the
   view, and Esc gives up. Areas may cross the 180° meridian (Fiji, the
   Bering Strait). An area round a pole has to be a rectangle that reaches
   the pole.
5. Choose the **levels**. Level 0 is the whole world in two tiles; each
   level doubles the detail. The dialog says how fine the deepest level
   is (for example "about 150 m per pixel" at level 8, about 10 m at
   level 12). Start at level 0 so the region also shows when zoomed out.
6. Read the size. The number of tiles is exact; the size is estimated
   from a few tiles fetched as samples, so it is "about" (usually within
   a third). Regions are limited to a million tiles: each extra level
   quadruples the count, so choose a smaller area or fewer levels if the
   dialog says it is too many. For a whole country the limit comes
   early: the United Kingdom, France or Japan fit to level 14, Norway to
   13, the United States to 12, Canada and Russia only to 11.
7. Press **Download**.

The download runs in the background, two tiles at a time, while you carry
on. The status bar shows how far it has got, and so does the Offline
Regions window, where the selected region's outline is drawn on the globe.

## Terms of use

Downloading many tiles at once is a heavy load on a server, and some
servers' terms forbid it; the OpenStreetMap servers are the best-known
example.

- For services in Terra Soar's list whose terms forbid it, the dialog says
  so and the **Download** button stays off.
- For services whose terms are unknown to Terra Soar (any server you typed
  in yourself, and some in the list), the dialog asks you to check the
  terms first, with a link where it has one. Please do.
- Services that allow it, such as NASA GIBS, download without a warning.

## Managing regions

**File → Offline Regions…** lists your regions with their layer, levels,
number of tiles, size on disk and state:

| State | Meaning |
| --- | --- |
| Downloading: 45 % | Running; the window and status bar follow it |
| Paused at 45 % | Paused by you, or by itself because the server couldn't be reached (the reason follows) |
| Not finished | The download was stopped (by closing Terra Soar, for example) |
| Ready offline | Every tile is on your computer, with the date downloaded |
| 12 tiles missing | Downloaded, but the server failed for some tiles |

The middle button does what the region needs:

- **Pause** a running download; **Resume** carries on where it stopped,
  also after closing and reopening Terra Soar.
- **Update** a finished region: tiles that are missing or that the server
  says are out of date are fetched again; the rest stays as it is. Do this
  before a trip.

**Export as KML…** saves a region's area as KML, to share it or open it in
other programs ([KML and KMZ](kml.md)).

**Delete** removes a region. Its tiles stay in the cache as ordinary saved
tiles for now, and go when space is needed.

## Viewing offline

Nothing to switch on: when the server can't be reached, Terra Soar shows
what it has saved. Inside a region that is every tile at every level you
chose. The layer gets a warning icon in the Layers panel to say the server
can't be reached; that is expected offline. Outside your regions you see
what you happened to look at before, or coarser tiles, or nothing.

Saved tiles normally last a week (or as long as the server says) before
Terra Soar asks the server whether they changed; offline, old tiles are
shown as they are.

## Space on disk

Ordinary saved tiles are limited to 256 MB per layer, the least recently
used going first. Region tiles don't count towards that limit and are
never removed to make room: a region takes as much space as its size
says, until you delete it. Regions are kept in your cache folder (on Linux
`~/.cache/Terra Soar/terrasoar/wms`, on Windows
`%LOCALAPPDATA%\Terra Soar\terrasoar\cache\wms`); deleting that folder
deletes your regions too.

## Extra data packs

The built-in Blue Marble and shoreline are kept small so Terra Soar
downloads quickly. **File > Data Packs…** lists larger versions you can
install once and then use offline:

| Pack | What you get | Download |
| --- | --- | --- |
| Blue Marble, full resolution | The same picture of the Earth, about 1.2 km per pixel instead of 4.9 km | 67 MB |
| Blue Marble, month by month | Blue Marble for each month of the year: snow, ice and vegetation change with the seasons, following the program's clock (see [Time](layers.md#time)); about 9.8 km per pixel | 25 MB |
| Shoreline, full resolution | Detailed coasts and small islands: high resolution from zoom level 6, full resolution from level 8 (the built-in shoreline stays at about 1 km) | 241 MB |

Pick a pack and press **Install**. The download runs in the background
and survives closing the window: **Pause** stops it and **Resume**
carries on from where it stopped, also after quitting Terra Soar or
losing the connection. Every file is checked before the pack is used, so
a damaged download is never shown.

An installed pack takes the place of the built-in data it improves: the
Blue Marble and Shoreline layers of every project show the pack's data,
with no change to the project. **Remove** deletes the pack and brings the
built-in data back. When a newer version is listed, **Update** replaces
the installed one.

Packs are kept in `~/.local/share/Terra Soar/terrasoar/packs` on Linux
and `%LOCALAPPDATA%\Terra Soar\terrasoar\packs` on Windows.

### VMAP0 map layers

The **VMAP0** packs bring layers of their own: the world vector map of
the US National Geospatial-Intelligence Agency, *Vector Map Level 0*, at
a scale of 1:1,000,000 (edition 5, from 2000). One pack per theme:

| Pack | What you get | Download |
| --- | --- | --- |
| VMAP0 boundaries | International boundaries; disputed boundaries in orange, claim lines, de facto boundaries and the date line | 4 MB |
| VMAP0 hydrography | Rivers (with names), lakes, land that floods, canals, dams, waterfalls and springs, coastlines | 83–134 MB each region |
| VMAP0 transportation | Roads by class, railways, trails, ferry routes, bridges and tunnels, airports with their names | 109 MB |
| VMAP0 population | Built-up areas, towns and villages with their names, landmarks | 27 MB |
| VMAP0 elevation | Contour lines with their heights, spot heights, depth contours | 69–112 MB each region |
| VMAP0 vegetation | Woodland, cropland, grassland and scrub, marsh, tundra | 85–143 MB each region |
| VMAP0 ice, ground, industry and utilities | Ice fields and sea ice, sand and lava, mines and oil fields, pipelines and power lines | 66 MB |
| VMAP0 administrative areas (ESRI) | States and provinces: their boundaries, and their names on click | 77 MB |

Hydrography, elevation and vegetation are large, so each comes as four
packs, one per region of VMAP0: North America; Europe and North Asia;
South America, Africa and Antarctica; South Asia and Australia. Install
the regions you need; their layers show every region you have installed.

An installed VMAP0 pack doesn't change what you see until you add its
layers: **Add Layer → Data Packs** lists each pack's layers, and **All
Layers** adds them together as a collection. Each kind of feature
appears only from a suitable zoom level, so the whole globe isn't
cluttered: international boundaries always, main roads and lakes from
continent scale, other roads and towns as you zoom in to a region, and
small rivers and contours closer still. With software graphics (no GPU),
turn off the themes you don't need: all of them at once over a busy region
makes the view slow. Town and airport names, and spot heights, are written beside
their points. With **What Is Here** on, clicking a feature lists what it
is and its details in plain words (a road's class, a river's name, a
boundary's status).

VMAP0 was compiled from charts of the 1970s to 1990s: roads, towns and
boundaries are as they were then, accurate to about 2 km, and they are
no statement on any boundary or name. The administrative areas pack is
licensed by ESRI for internal or non-commercial use only; the others are
public domain.

### More map layers

These packs also bring layers of their own, added from **Add Layer → Data
Packs** like VMAP0's:

| Pack | What you get | Download |
| --- | --- | --- |
| Natural Earth map layers | Rivers and lakes, towns and cities, roads and ferries, railways, airports, ports, time zones, state and province borders, and peaks, with names; each from the zoom level that suits it | 76 MB |
| Natural Earth I relief map | A map-like picture of the Earth: land cover in soft natural colours, shaded relief and a pale blue sea (about 1.9 km per pixel) | 72 MB |
| Natural Earth hypsometric map | Lowlands to highlands in greens to browns, with shaded relief and the relief of the ocean floor | 67 MB |
| Natural Earth shaded relief | Grey shaded relief of the land: a quiet background, or under a half-transparent layer | 39 MB |
| Black Marble (Earth at night) | The Earth's city lights at night, for 2012 and for 2016, each as its own layer | 18 MB |
| WDBII borders and rivers | National, internal and maritime borders, rivers by size, intermittent rivers and canals, down to a detailed resolution (the borders are those of the 1980s) | 89 MB |
| Airports (OurAirports) | About 72,000 airports, heliports and seaplane bases with their names, runways, and navigation aids (VOR, NDB, DME) | 16 MB |
| Place names | Names of cities, towns, villages and neighbourhoods and of major features (mountains and ranges, lakes, bays, seas, islands, capes, glaciers, falls, undersea features), shown as you zoom in: capitals first, villages close in | 309 MB |
| Terrain, finer (ETOPO 2022) | The Terrain layer at about 0.9 km instead of 4.9 km: mountains, valleys and the sea floor in more detail. Takes the place of the built-in terrain | 461 MB |
| GEBCO terrain and sea floor | The sea floor and the land at about 460 m, as a layer of its own above Terrain; four packs, one per quarter of the world | 356–396 MB each |

The picture packs are layers like Blue Marble: put one at the bottom of
the tree to use it as the background instead. OurAirports is community
data, not for navigation; only runways whose ends are recorded are drawn
(about a third of them).

## Limits

- Only WMS, WMTS, XYZ and WFS layers can be kept this way. The built-in layers need no
  download; for more detail in them, install a data pack (below).
