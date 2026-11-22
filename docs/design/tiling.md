# Tiling

The globe's imagery (and later elevation and vector data) comes in tiles
of a geographic quadtree. This page describes the layout, the meshes,
how the tiles to draw are chosen, and how they get from a provider to the
screen. Code: `core/include/ts/tiling/`, `core/include/ts/globe/tilerenderer.h`.

## The quadtree

`ts::TileKey` is (level, x, y) in EPSG:4326 (latitude/longitude on WGS 84):

| Level | Columns × rows | Tile size | At the equator |
| --- | --- | --- | --- |
| 0 | 2 × 1 | 180° | 20 000 km |
| 1 | 4 × 2 | 90° | 10 000 km |
| n | 2^(n+1) × 2^n | 180° / 2^n | 20 000 km / 2^n |
| 18 | 524 288 × 262 144 | 0.000 69° | 76 m |
| 24 (deepest) | | 0.000 011° | 1.2 m |

- Level 0 is two tiles: x = 0 is the western hemisphere (−180° to 0°),
  x = 1 the eastern. This is the tile pyramid of the OSGeo TMS
  "global-geodetic" profile, with rows counted from the north as in WMTS.
- Columns count east from the antimeridian, rows south from the north
  pole. Child i of a tile is north-west, north-east, south-west,
  south-east for i = 0 … 3.
- Tiles never cross the antimeridian: a rectangle is always
  west < east. `TileKey::containing()` puts 180° E in the last column and
  −180° in the first.
- `TileKey::packed()` packs a key into 64 bits for hashing; keys sort by
  level, then row, then column.

## Meshes

`ts::buildTileMesh()` makes a regular grid of 16 × 16 segments over a
tile's rectangle, plus a skirt.

- **Relative to the centre.** Vertices are float offsets from the tile's
  ECEF centre; the centre goes to the GPU as a high/low pair, and the
  vertex shader adds (centre − eye) and the offset, so tiles keep their
  precision near the eye (see [camera-and-precision.md](camera-and-precision.md)).
- **Poles.** The row of a tile at a pole collapses to the pole point. The
  geographic quadtree therefore covers the polar caps with ordinary tiles
  (no special cap geometry, unlike Web Mercator, which stops at 85°).
  Texture coordinates stay linear in latitude and longitude, which is
  exactly how EPSG:4326 imagery is laid out.
- **Skirts.** Neighbouring tiles at different levels approximate the
  curved surface with chords of different lengths, which leaves thin gaps
  along their shared edge. Every tile has a strip hanging down from its
  border, deep enough to cover the gap: twice the sag of one grid segment
  (R (1 − cos(θ/2)) for a segment of angle θ) plus a metre.
- **Shared indices.** All tiles with the same segment count share one
  16-bit index buffer. Triangles are counter-clockwise seen from outside
  (the skirts face outwards), so back faces are culled; that removes the
  parts of tiles beyond the limb.

## Level of detail

`ts::TileSelector::select()` walks down from the two roots every frame.

**Culling.** A tile is skipped when it can't be seen:

- *Frustum*: its bounding sphere (from nine surface samples, plus the sag
  between them and the skirt depth) is outside a side plane, behind the
  eye or beyond the far plane.
- *Horizon*: in the frame scaled by (1/a, 1/a, 1/b), the ellipsoid is the
  unit sphere and a surface point q is visible from the eye v exactly when
  q·v ≥ 1. Between the nine samples q·v can rise by at most |v| times the
  angle between samples, so a tile whose samples all fall short of 1 by
  more than that is hidden. Levels 0 and 1 are too curved to bound this
  way and are never horizon-culled.

**Screen-space error.** A visible tile's error is the size of one of its
texels on screen:

    error = texel size × viewport height / (2 d tan(fov/2))

with the texel size the tile's north–south extent divided by the tile size
in pixels (the largest extent: texels shrink east–west towards the poles)
and d the distance from the eye to the tile's bounding sphere. A tile is
split while its error exceeds 1.5 pixels (lower quality tiers will raise
this) and its level is below the provider's deepest.

**No holes, coarse to fine.** A tile is replaced by its children only when
all its visible children are on the GPU. Until then the tile itself is
drawn and the missing children are requested. So the view is always
covered by what is already loaded, and detail fills in level by level.
Requests are ordered by screen-space error, largest first, then by
distance: a parent's error is twice its children's, so coarse tiles still
come first, and in a tilted view the near tiles of a level come before
the far ones.

`tests/tiling` checks, for cameras from a whole-globe view to a tilted
view of a city: every pixel showing the globe is covered by a selected
tile, no selected tile contains another, every drawn tile's error is
within the limit, and fixed cameras select fixed tile sets.

**On the flat map** (the camera's morph factor is 1, see
[morph.md](morph.md)) the same walk uses the map instead of the globe:

- *Culling*: a tile is skipped when its rectangle on the map is outside
  the view in every copy of the world (the map repeats in longitude), or
  it lies wholly beyond the projection's latitude limits. There is no
  horizon.
- *Screen-space error*: the size of a texel along a meridian of the map,
  the tile's projected height over its height in texels. It depends on
  the latitude and the zoom, not on the distance from the centre:
  Mercator's stretching towards the poles asks for finer tiles there.
- *Geometry*: a tile is also split while its mesh, straight between grid
  rows, is more than half a pixel from the projection's curve
  (`mapGeometryErrorPx()`), whatever the deepest level of the data. A
  projection not smooth at its poles (Mollweide), where the geometry
  error falls only slowly with the level, limits this to
  `geometryLevelsBeyondTexels()` levels beyond what the texels need.

While the view changes between the two (0 < morph < 1) a tile is judged
where it is drawn at that moment, part of the way between globe and map:
it is drawn if a sphere bounding it there meets the frustum, without the
horizon test, its error is that of a texel there, and it is never coarser
than level 2 (see [morph.md](morph.md)). The same tests run for maps in all
three projections, across the wrap and at the edges of the map.

### A turned or tilted map

Looking straight down with north up, the map's selection is as above,
unchanged (`untiltedMapUnchanged`). Turned or tilted
(`Camera::isMapTurnedOrTilted()`), the view is no longer a rectangle of
the map, and a tilted one is not at one distance:

- *What is in view*: `Camera::mapFrame()` gives the view's footprint on
  the map's plane, a convex polygon in map metres: a 24-sided polygon
  drawn round the circle within the far limit, cut by the four side planes
  of the view. A tile is kept if its rectangle on the map (in any copy of
  the world, or its part in the domain) meets the polygon, by separating
  axes.
- *The far limit* (`Camera::mapFarLimitM()`): a plane has no horizon of
  its own, so a tilted map is drawn out to 40 times the eye's height above
  it, at most 10 000 km. At 10 000 km a level-2 tile (the coarsest drawn
  on the map, 45°) has a texel of 19.5 km, under 2 pixels in an 800-pixel
  view: the coarsest level still nearly meets the error limit there, and
  everything nearer is finer. Tiles beyond it are never selected or
  requested, from any provider, and the far clip plane is there. A haze
  hides the edge (from 55 % of the limit) and a sky is drawn beyond it.
- *The tilt limit*: 70° (`Camera::maximumMapTiltDeg`), short of looking
  along the plane; the globe keeps 85°.
- *Screen-space error* on a tilted map: the size of a map texel seen
  from the tile's nearest point, as on the globe: the texel on the plane
  times the pixels per radian over the distance from the eye to the
  nearest point of the tile's rectangle (`MapFrame::distanceToEyeM()`).
  Detail falls off towards the horizon. Turned but straight down, every
  texel is still the same size on screen, and the straight-down error is
  kept. The geometry error is scaled the same way.
- *Terrain*: a tilted map shows the terrain in relief (see
  [morph.md](morph.md#rotation-and-tilt-on-the-map)), so its height
  meshes use the same distance-based levels, and skirts close the gaps
  between levels. A map in relief draws the tiles' full meshes, not the
  cylindrical maps' strips, which have no vertices inside a row.

`tests/tiling` (`tiltedMapSelection`) checks every projection, turned and
tilted to the limit over a region and a city: every pixel covered once,
every drawn tile within the error limit at its distance, levels never
finer up the view, nothing requested beyond the far limit, requests in
order, the same tiles for the same camera, and at most 600 tiles drawn
(about 10 to 160 measured). `tiltedMapRequestsNearFirst` checks that the
first requests in a tilted city view are the nearest.

## Geometry and imagery

Tile meshes depend only on the key, so `ts::TileRenderer` builds them on
the render thread as the selection asks for them (up to 32 per frame,
about 0.02 ms each), and the selection refines as soon as they exist.
Imagery is separate: each imagery source (one per provider, shared by
every layer that uses it) has its own loader and GPU textures. For each
drawn tile, each layer uses its own image if loaded, else its nearest
loaded ancestor's, stretched through a texture transform. Each source
requests, coarse first, a stand-in four levels above every drawn tile and
then the drawn tiles themselves. Layers are blended in the tile fragment
shader, up to 8 per draw; see [layer-model.md](layer-model.md).

Within each of those two passes, and for terrain heights and vector
tiles too, tiles are fetched from the middle of the view outwards
(`TileSelector::orderFromViewCentre()`): by the angle, seen from the eye,
between the camera's target and the point of the tile nearest it, where
the renderer draws that point (globe, map or between; on the map the copy
of the world nearest the middle). The tile under the middle of the
screen scores 0 and comes first. A tile's own middle would not do: a
coarse tile that shows only at the limb has its middle on the far side
of the Earth, close to the line of sight, and came first. Meshes, built locally, still go in the selection's order
(`TileSelection::load`, the largest error first).

What isn't seen isn't fetched (`fetchesOnlyWhatShows` in `tests/globe`;
`TileStatistics` counts each):

- **Stand-ins already beaten.** A drawn tile's stand-in isn't asked for
  when the tile shows an image at least as fine already: its own, or one
  between it and the stand-in's level. After zooming in a level, every
  new tile shows its parent from the last view, which is finer than the
  stand-in four levels up (`standInsSkipped`).
- **Hidden layers.** A layer isn't fetched for a drawn tile when a layer
  above it has its own image there (not a stretched ancestor's, whose
  detail may yet have transparent parts), every pixel of it opaque
  (`isFullyOpaque()`, worked out on the loader thread), at full opacity
  in a group at full opacity, and covering the whole tile (a Web Mercator
  image stops at 85.05°). A layer at no opacity isn't fetched either
  (`hiddenSkipped`). Until the top layer's tile arrives, the ones below
  are fetched as before, so something shows meanwhile.
- **Fast moves.** While the camera moves faster than 1.5 screen heights
  a second (`MotionHold`: how far the eye moves, against the height of
  ground shown at the target), each source asks only for the stand-ins;
  the detail is asked for 150 ms after it slows down. A drag or a zoom
  through many levels then doesn't fetch tiles that would be on screen
  for a moment. The hold starts after two fast frames in a row, so a
  jump to a place doesn't wait. While it lasts the renderer counts as
  busy, so frames keep coming until the detail has been asked for
  (`detailHeld`). Terrain heights and vector tiles aren't held: they
  have no stand-ins.

## From provider to screen

```
render thread                         worker threads (TileLoader)
─────────────                         ───────────────────────────
prepare(camera)
 ├ takeLoaded(8, all sources) ────────◄ finished images
 │   upload textures
 ├ select(camera, mesh resident)
 ├ build missing meshes (≤ 32)
 ├ bind images (own or ancestor)
 ├ setRequests(wanted) per source ────► queue, cancel unwanted in flight
 └ evict least recently used           load: cache → provider → cache
draw()                                  callback → GUI thread: new frame
 └ per tile: 8 layers per draw;
   composite groups offscreen
```

- **Loader** (`ts::TileLoader`, thread-safe): two worker threads by
  default (the reference PC has four hardware threads), more for a
  provider that asks (network sources wait on servers, not the CPU). Each frame the
  render thread replaces the wanted list, so the queue is always the
  current view's, in its order; queued tiles not wanted any more
  are dropped without being fetched, and tiles being loaded that aren't
  wanted are told to cancel through their flag, which aborts the HTTP
  request (or drops it from the network client's own queue, or its wait
  before a retry). `loaderFetchesInOrderAndDropsUnwanted` in
  `tests/tiling` checks both. Finished tiles wait until the render thread
  takes them. A tile whose load *failed* (a network error, as opposed to
  a tile with no data) isn't delivered: the renderer keeps showing a
  coarser image there, and the tile is left out of the requests for 30 s
  before it is asked for again if still wanted. It doesn't count as
  loading meanwhile, so an unreachable server can't keep frames coming.
- **Decoded-image cache** (`ts::LruCache`, thread-safe, 48 MB): a tile
  evicted from the GPU comes back without asking the provider again.
- **Upload queue**: at most 8 tiles go to the GPU per frame, so a burst
  of finished tiles never stalls a frame.
- **GPU tiles** (`ts::TileRenderer`, 96 MB): a texture (RGBA8 with
  mipmaps) and a vertex buffer per tile; when over budget, the tiles used
  least recently are dropped, never one used this frame. "Used" is
  everything the frame relies on, not only what it draws: every mesh the
  selection asks about (the walk from the roots only passes through tiles
  with meshes, and replaces a tile by its children only while all the
  visible ones have them), and every image the frame wants that is
  already there (each drawn tile's own and its coarse stand-in). When
  only drawn tiles counted, those were the least recently drawn and went
  first once the budget was full: the next frame fell back to a few
  coarse tiles, the one after evicted the fine ones, and the detail
  dropped and came back over and over (level 10 to 3 and back, zooming
  with a WMS layer). If one frame needs more than the budget it is let
  through, and evicted back down once the frame doesn't (measured: at
  most 123 MB, tilting a full HD view with two layers).
  A mesh the selection asks for that isn't there is built at once, within
  `meshBuildsPerFrame`, so that a tile coming into view isn't drawn whole
  for a frame instead of its children. `detailHoldsUnderGpuPressure` in
  `tests/globe` zooms, pans and tilts with a 24 MB budget. A tile the
  provider has no image for is kept as an empty tile and not drawn, so the
  plain globe shows there.
- **Frames**: the loader's callback runs on a worker; the view posts a
  frame request to the GUI thread. While tiles are loading or waiting for
  upload, frames keep coming (paced to the screen); then rendering stops.

## Drawing order and depth

Within the globe's pass: the ray-cast ellipsoid, then the tiles (depth
test and write, back faces culled), then markers. A tile's flat triangles
lie slightly below the curved surface, by up to `tileSurfaceSagM()` (the
gap under a grid cell's diagonal; 15 m at level 6, 960 m at level 3), so
the ellipsoid's depth is pushed farther for them to pass the depth test:
1 % of the distance, plus that sag for the coarsest tile drawn this frame
(`TileRenderer::surfaceSagM()`) divided by the cosine of the angle
between the view ray and the vertical (at most 100 times the sag, towards
the limb). Along a ray that meets the surface at a shallow angle the gap
is that much longer: looking towards the horizon from a few kilometres
up, a 1 % margin alone let the ellipsoid's plain colour show through the
imagery (`base-uk-grazing` in `tests/globe`). All write logarithmic depth.
The ray-cast globe shows wherever no tile is drawn: before the first
tiles load, and where a provider has no data. While terrain is shown the
ray-cast pass is left out and the tiles carry the surface colour, since
the terrain is not on the ellipsoid ([terrain.md](terrain.md)).

## Budgets and measurements

| Item | Default | Measured (llvmpipe, 320 × 240) |
| --- | --- | --- |
| Selection, tilted city view (263 tiles visited) | | 0.46 ms |
| Selection, globe at 80° over a city with terrain margins, 800 × 600 (567 drawn, 808 visited) | at most 800 tiles | 1.6 ms (release) |
| Selection, map at its 70° limit over a city, 800 × 600 (91 drawn, 154 visited) | at most 800 tiles | 1.0 ms (release) |
| Frame at the tilt limit, checkerboard, 320 × 240: globe / map | 100 ms | 6.8 ms / 6.0 ms |
| The same with terrain: globe / map | 100 ms | 10.9 ms / 12.8 ms |
| GPU memory in those frames | 96 MB | at most 38 MB |
| Tile mesh, 16 × 16 + skirt | | 0.02 ms |
| Checkerboard tile, 256 px | | 1.0 ms (worker) |
| Frame with 33 tiles | | 4.5 ms |
| Decoded-image cache | 48 MB | |
| GPU tiles | 96 MB | |
| Uploads per frame | 8 | |
| Loader threads | 2 | |
