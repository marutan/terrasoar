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
Requests are ordered by level, then distance.

`tests/tiling` checks, for cameras from a whole-globe view to a tilted
view of a city: every pixel showing the globe is covered by a selected
tile, no selected tile contains another, every drawn tile's error is
within the limit, and fixed cameras select fixed tile sets.

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
  default (the reference PC has four hardware threads). Each frame the
  render thread replaces the wanted list; queued tiles not wanted any more
  are dropped, and tiles being loaded that aren't wanted are told to
  cancel through their flag. Finished tiles wait until the render thread
  takes them.
- **Decoded-image cache** (`ts::LruCache`, thread-safe, 48 MB): a tile
  evicted from the GPU comes back without asking the provider again.
- **Upload queue**: at most 8 tiles go to the GPU per frame, so a burst
  of finished tiles never stalls a frame.
- **GPU tiles** (`ts::TileRenderer`, 96 MB): a texture (RGBA8 with
  mipmaps) and a vertex buffer per tile; when over budget, the tiles drawn
  least recently are dropped, never one drawn this frame. A tile the
  provider has no image for is kept as an empty tile and not drawn, so the
  plain globe shows there.
- **Frames**: the loader's callback runs on a worker; the view posts a
  frame request to the GUI thread. While tiles are loading or waiting for
  upload, frames keep coming (paced to the screen); then rendering stops.

## Drawing order and depth

Within the globe's pass: the ray-cast ellipsoid (its depth pushed 1 %
farther, so that a tile's flat triangles, which lie slightly below the
curved surface, pass the depth test), then the tiles (depth test and
write, back faces culled), then markers. All write logarithmic depth.
The ray-cast globe shows wherever no tile is drawn: before the first
tiles load, and where a provider has no data.

## Budgets and measurements

| Item | Default | Measured (llvmpipe, 320 × 240) |
| --- | --- | --- |
| Selection, tilted city view (263 tiles visited) | | 0.46 ms |
| Tile mesh, 16 × 16 + skirt | | 0.02 ms |
| Checkerboard tile, 256 px | | 1.0 ms (worker) |
| Frame with 33 tiles | | 4.5 ms |
| Decoded-image cache | 48 MB | |
| GPU tiles | 96 MB | |
| Uploads per frame | 8 | |
| Loader threads | 2 | |
