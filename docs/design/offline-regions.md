# Offline regions

How an area chosen by the user becomes a set of quadtree tiles, how big it
will be, how the tiles are downloaded and kept from eviction, and where it
is all stored. Code: `core/include/ts/regions/`, with the pins in
`ts::TileDiskCache` ([network-and-cache.md](network-and-cache.md)) and the
download of one tile in `ts::WmsImageryProvider::fetchForRegion()`
([wms.md](wms.md)). For users: [offline-use.md](../user/offline-use.md).

```
New Offline Region dialog                 region manager (GUI thread)
  area: view / rectangle / polygon          OfflineRegionStore  (regions.json)
  levels → countRegionTiles()               RegionDownload per running region
  estimateRegionSize() (thread)               ├─ 2 download threads
  bulkDownloadPolicy() → warn / refuse        │    RegionTileIterator → key
                                              │    provider.fetchForRegion(key, id)
                                              │      fresh? pin : GET → store + pin
                                              └─ progress polled 4× a second
```

Only WMS layers can be kept: the base pack is on disk already, and they
are the only network sources so far. A region belongs to one source (a
`{"kind": "wms", …}` object, as in a layer definition); two layers showing
the same source share its regions, since they share the provider and the
disk cache.

## Geometry

`ts::RegionGeometry` is a latitude/longitude rectangle or a polygon. Edges
are straight lines in longitude and latitude, like tile edges, not
geodesics: a region is a set of tiles, and the tiles' own edges are
straight in those coordinates.

**The antimeridian.** Longitudes are kept *unwrapped*: the first vertex in
[−180°, 180°) and each next one within 180° of the one before it, so a
region never jumps from 179° to −179°. A rectangle from 170° E to 170° W is
170° to 190°; the polygon round Fiji runs 176° to 182°. The extent's west
edge is in [−180°, 180°) and its east edge up to 360° further. A tile
(always within ±180°) is tested as it is and one turn (360°) to the east,
which covers every case.

**The poles.** A polygon that goes round a pole can't be unwrapped (its
longitudes would add up to ±360°) and is refused; a rectangle can reach
±90° instead, which is what such a region means in tiles anyway.

**Tests on a tile.** `intersects()` asks whether region and tile share
some area: the tile's centre inside the polygon (even–odd rule), a
vertex strictly inside the tile, or an edge passing through the tile's
interior (Liang–Barsky clipping, the clipped part's midpoint strictly
inside). Touching along an edge doesn't count, so a region whose edge lies
on a tile boundary doesn't pull in the next row of tiles. `covers()` asks
whether the tile lies wholly inside: its centre inside and no vertex or
edge in its interior.

Stored as `{"kind": "rectangle", "west", "south", "east", "north"}` or
`{"kind": "polygon", "vertices": [[lon, lat], …]}`.

## From a region to tiles

The tiles of a region at levels *min* to *max* are those whose area
inside the source's data bounds shares some area with the region. The
data bounds are the source's bounds from its capabilities, and for a Web
Mercator source the latitudes Mercator reaches (±85.05°), because
`getMapUrl()` requests nothing outside them
(`WmsImageryProvider::dataBounds()`). Each tile is clipped to the bounds
before it is tested, so a tile overlapping the bounds and the region in
different places isn't counted.

- `countRegionTiles()` walks the quadtree from the two roots and counts
  per level. A tile wholly inside the region (and the bounds) has all its
  descendants inside too, so they are counted with powers of four
  instead of visited. The walk then only follows the edge: the British
  Isles to level 16 (some 70 million tiles) take milliseconds.
- `RegionTileIterator` hands the same tiles out one at a time, level by
  level from the coarsest, each level in quadtree order. A download that
  stops part way therefore has the whole region at the coarser levels.
  It keeps only a stack per level, never the list of tiles.

`tests/regions` checks both against a test of every tile, level by level,
for polygons over the British Isles, round Fiji and across the Bering
Strait (both across the antimeridian), a concave polygon, a Pacific
rectangle and a source with bounds.

## How big: the estimate

The count is exact and instant. The size isn't known until the tiles are
fetched: a map tile of open sea may be 1 KB and one of a city 40 KB.
`estimateRegionSize()` fetches a sample: `sampleRegionTiles()` picks
random points in the region's extent, keeps those inside it, and takes the
tile containing each at a level chosen with the weight of that level's
count (so the deepest level, three quarters of the tiles, gets three
quarters of the samples). 24 samples (32 in the tests) are fetched on four
threads and the mean size times the count is the estimate. The samples
are stored in the cache as ordinary tiles, so the download doesn't fetch
them again. Without a server the estimate falls back to a typical size
for the format (`typicalTileBytes()`: 20 KB for a 256-pixel JPEG, 25 KB
for PNG).

**The agreed margin is ±30 %.** `tests/regions` downloads two regions
from the mock WMS and checks the estimate against what was stored; it
comes within 5 %. Real servers vary more between tiles, which is why the
dialog says "about".

The dialog counts as the user types, waits 0.4 s after the last change,
then estimates on a thread of its own (cancelled when anything changes
again). It refuses regions of more than a million tiles.

## Pinning

A region's tiles live in the source's disk cache like any other tile. A
second table holds the pins:

```sql
CREATE TABLE pins (region INTEGER NOT NULL, tile INTEGER NOT NULL,
                   PRIMARY KEY (region, tile)) WITHOUT ROWID;
CREATE INDEX pins_tile ON pins(tile);
PRAGMA user_version = 2;  -- version 1 (no pins) is upgraded in place
```

- A pinned tile is never evicted, and pinned bytes don't count against
  the cache's size limit (256 MB per source): a region is as large as the
  user chose, and browsing elsewhere neither pushes it out nor is pushed
  out by it. Eviction picks the least recently used tiles *not* pinned.
- Regions can share tiles (overlapping areas, or the coarse levels every
  region has); a tile stays pinned until its last region lets go.
- The cache keeps the pinned total in memory, like the total size, so
  deciding whether to evict costs nothing; it is summed again on opening
  and after a region is unpinned.
- Deleting a region unpins its tiles (`unpinRegion()`), which become
  ordinary cached tiles, the oldest of all; if the cache is then over its
  limit they are the first to go. Removing a tile (one that can't be
  decoded) removes its pins.
- Pinning doesn't change freshness. A pinned tile past its HTTP freshness
  is shown when offline like any saved tile; online it is revalidated
  when viewed, and Update revalidates the whole region.

## Downloading

`ts::RegionDownload` runs one region's download on two threads of its
own, through the provider's network client, which keeps its six request
slots: the view's own tiles still get through, and the server sees no
more than two requests at a time from a download.

For each tile, `fetchForRegion()`:

1. Requests nothing outside the source's data (NoData).
2. Pins a stored tile that is still fresh, and fetches nothing.
3. Otherwise sends GetMap, with If-None-Match / If-Modified-Since for a
   stale stored tile: 304 renews it; 200 is checked to be an image and
   stored with its pin in one call.
4. Fails on server errors, non-images and `no-store` responses (the
   server forbids keeping the tile, so it can't be offline), and without
   a disk cache.

Because stored fresh tiles are only pinned, starting a download of the
same region again is how it **resumes** after the program was closed and
how it is **updated**: only what is missing or stale is fetched.

**Pause** lets the requests on the wire finish and holds the rest.
**Cancel** drops the requests on the wire at once; what was stored stays
pinned until the caller unpins it. When six tiles in a row fail because
the server can't be reached, the download **pauses itself** with the
reason; each such tile is tried once more on resume. Other failures are
counted and the download goes on; the region is then complete with tiles
missing, and Update tries them again.

The application's `OfflineRegionManager` (in `app/`) owns the running
downloads, polls them four times a second for the Offline Regions window
and the status bar, and records a finished download in the region list.
Downloads stop when the program closes; the region stays, marked not
finished, and Resume carries on.

## Terms of use: allowBulkDownload

Many map servers allow viewing but not bulk downloading; the
OpenStreetMap tile servers are the best-known example. Each catalogue
entry ([wms.md](wms.md#the-catalogue)) may say so:

```json
{"title": "OpenStreetMap (terrestris)", "url": "https://ows.terrestris.de/osm/service",
 "allowBulkDownload": false, …}
```

`bulkDownloadPolicy()` finds the source's entry by host and path (query,
scheme, host case and a trailing slash don't matter), else by host alone:

| Entry | Policy | The program |
| --- | --- | --- |
| `allowBulkDownload: false` (same path, or same host) | Forbidden | Refuses: `RegionDownload::start()` fails before any request, and the dialog's Download button is off |
| `allowBulkDownload: true`, same path | Allowed | Downloads |
| `true` on another path of the host, no flag, or not in the catalogue | Unknown | Warns in the dialog, with the terms link if known; the user decides |

An entry found by host alone can forbid but not allow: another service on
the same host may have other terms. The flag is optional, so older
programs read the catalogue as before. The shipped catalogue sets it for
NASA GIBS and USGS (allowed: public data) and terrestris (forbidden:
OpenStreetMap data served for viewing); the others don't say yet.

## Storage

| What | Where |
| --- | --- |
| The tiles and their pins | The source's disk cache, `<cache>/wms/<sha1 of the source id>.sqlite` |
| The list of regions | `<cache>/wms/regions.json` |

`<cache>` is `QStandardPaths::CacheLocation` (for example
`~/.cache/Terra Soar/terrasoar` on Linux, `%LOCALAPPDATA%\Terra Soar\terrasoar\cache`
on Windows; neither is cleared by the system).

```json
{"format": "terrasoar-offline-regions", "version": 1, "nextId": 3,
 "regions": [{"id": 1, "name": "United Kingdom",
              "source": {"kind": "wms", …}, "sourceName": "…",
              "geometry": {…}, "minLevel": 0, "maxLevel": 12,
              "created": "2026-11-12T10:00:00Z", "downloaded": "…",
              "complete": true, "tiles": 12345, "failedTiles": 0,
              "bytes": 678901234}]}
```

Region ids are the pins' region numbers. They are never reused (`nextId`
only grows), so pins left behind by a crash between deleting a region and
unpinning it can't attach to a later region. The file is written with
`QSaveFile`; one that can't be read, or of a newer version, gives an empty
list and a warning in the log (the tiles stay pinned).

## Tests

`tests/regions` (no network: a mock WMS on 127.0.0.1):

- Rectangles and polygons: normalisation, unwrapping across the
  antimeridian from either side, refusals (too few points, off the Earth,
  round a pole), area, JSON round trips.
- Known tile sets: a quarter of the world level by level, the four tiles
  of a Pacific rectangle across the antimeridian, a polar cap; counts and
  the iterator against a test of every tile for six regions; the British
  Isles to level 16 in under five seconds; samples inside the region,
  distinct, weighted to the deepest level.
- The region list: round trip, ids never reused, damaged and newer files.
- Pins: pinned tiles survive cache pressure while unpinned ones go oldest
  first; shared pins; unpinning returns tiles to the LRU; pins survive a
  restart; a version 1 cache is upgraded with its tiles.
- The bulk-download flag: parsing, matching, the shipped catalogue;
  a forbidden source refuses before any request.
- Downloads: every tile of a region pinned in a cache far smaller than
  the region; a second run fetches nothing; data bounds; pause holds,
  resume finishes with each tile requested once; cancel; an unreachable
  server pauses the download, resume fetches what failed; server errors,
  `no-store`, revalidation.
- The estimate within ±30 % of the download, and the guess without a
  server.

`tests/globe` (`offlineRegionGolden`): a region over the British Isles
downloaded from the mock WMS, then drawn with every request going to a
closed port: no request is attempted, every tile comes from the disk, and
the picture equals the one drawn online and the golden image.

`tests/ui` (`offlineRegionEndToEnd`, `drawRegionOnTheGlobe`): the New
Offline Region dialog's count, estimate, terms warning and refusals; the
download to "Ready offline" in the Offline Regions window; deleting
unpins; rectangles and polygons drawn on the globe with the mouse, Esc
cancelling.
