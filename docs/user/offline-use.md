# Using Terra Soar offline

Terra Soar works without a network. The built-in layers (Blue Marble, the
shoreline, the graticule) are always on your computer. Layers from web map
servers (WMS) need the server, but you can download an area of one in
advance as an **offline region** and view it later on a plane, at sea or
in the field.

## Downloading a region

1. Add the WMS layer you want to keep (see [Adding WMS layers](adding-wms-layers.md)).
2. Choose **File → Offline Regions…** and press **New Region…**.
3. Give the region a **name** and pick the **layer**.
4. Choose the **area**:
   - **Current View** takes the part of the Earth the view shows now (it
     is filled in when the dialog opens).
   - **Draw Rectangle**: the dialog steps aside; drag across the area on
     the globe or map.
   - **Draw Polygon**: click each corner of the area, then double-click
     (or press Enter) to finish. Backspace takes the last corner back.

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
   dialog says it is too many.
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

## Limits

- Only WMS layers can be kept this way. The built-in layers need no
  download; larger built-in data (full-resolution Blue Marble and
  shoreline) will come as installable packs.
- Picking a country by name isn't available yet; draw its outline as a
  polygon instead.
