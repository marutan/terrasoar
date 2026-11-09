# Vector rendering

How vector layers — the base pack's shoreline today, user vector data
later — are drawn on the globe: filled polygons, outlines and lines over
the imagery. Code: `core/include/ts/vector/` (meshing, loading, styles) and
`ts::VectorRenderer` in `core/include/ts/globe/`.

## From features to the screen

```
FeatureProvider (worker)          VectorTileLoader worker          render thread
loadFeatures(stored key) ───────► VectorMesher::build ───────────► upload
  polygons and lines in             cut polygons into cells,         select tiles,
  lon/lat, cut to their             triangulate (GEOS CDT),          draw fills,
  stored tile                       outline segments, ECEF           then outlines
```

1. **Selection.** Each feature source has its own `TileSelector`, the same
   frustum, horizon and screen-space-error selection as imagery
   ([tiling.md](tiling.md)), with tiles counted as **1024 pixels** wide.
   That is two levels coarser than imagery, and it matches the base pack's
   shoreline: each GSHHG resolution is simplified to about one pixel of a
   tile shown 1024 pixels wide (25, 5, 1 and 0.2 km at storage levels 0,
   2, 4 and 6). Finer selection only drew sub-pixel detail as a thick,
   fuzzy line.
2. **Stored tiles.** A source stores features for fewer levels than it
   is shown at: `FeatureProvider::storedKey(key)` names the tile whose
   features serve `key` (for the shoreline, its ancestor at levels 0, 2,
   4 or 6). Drawn tiles that share a stored tile share one load and one
   mesh.
3. **Meshing on workers** (`VectorMesher`, one per worker thread, each with
   its own GEOS context). See the next section.
4. **Upload.** Up to 8 meshes per frame, all sources together. A
   least-recently-used eviction keeps meshes under 64 MB of GPU memory.

## Meshing

**Fill.** Each polygon is made valid if needed, then cut down a quadtree
of cells (each level clips its parent's pieces, as in the packager) until
the cells are at most `cellSizeDeg()` wide: 1/16 of the stored tile, at
most 5.625°. Each cell's piece is triangulated with GEOS's constrained
Delaunay triangulation, so holes (lakes) stay holes. Because no triangle
spans more than a cell, its straight edges stay close to the curved
surface: at most about 7.6 km below it for the coarsest cells (5.6° at
levels 0–1, where a pixel is 20 km or more), 30 m for the high
resolution's 0.18° cells. Vertices are computed in double precision as
ECEF metres, then stored as float offsets from the tile's centre (with the
longitude and latitude, for clipping), as the imagery tiles do
([camera-and-precision.md](camera-and-precision.md)).

**Outlines.** Every ring of every polygon, and every line string, is cut
into segments of at most a quarter cell, so they follow the surface too.
Segments lying along the stored tile's own edges are left out. Those edges
are cuts, not shores: the packager's tile cuts, the antimeridian where
GSHHG splits polygons, and the poles where Antarctica's polygon is closed.
They are all tile edges, so one rule removes them all. This is why the
pack needs no separate shoreline lines (they would have doubled its size).

**Small rings.** Polygons (and holes) smaller than 1/512 of the stored
tile, about two pixels where it is shown largest, are left out unless they
touch the tile's edge (where they may be part of a larger polygon). At
coarse levels thousands of lakes and islets would otherwise be dots.

## Drawing

Vector layers draw after all imagery, in tree order (bottom first), with
the graticule among them (below). They don't use the depth test:

- **Horizon.** Each vertex knows whether the ellipsoid faces the eye there
  (the dot product of the surface normal with the direction to the eye).
  Fragments on the far side are discarded. With no terrain the ellipsoid
  is convex, so this is exact; terrain (Phase 7) will need depth.
- **Clipping.** Each draw is clipped to a rectangle in longitude and
  latitude, in the fragment shader. A stored tile is normally drawn once,
  over its own rectangle. Where tiles of another level of detail are drawn
  inside it (at a transition between resolutions), it is drawn once per
  drawn tile it serves, clipped to that tile, so no area is covered by two
  resolutions and translucent fills don't double up. Rectangles get
  1e-5° of slack so neighbours leave no cracks.
- **Fills** are plain triangles in the layer's fill colour, premultiplied
  by its opacity.
- **Outlines** are drawn one quad per segment (instanced, no per-vertex
  buffer). The quad is built in screen space around the projected segment,
  so lines keep their width in pixels at any distance. The fragment shader
  measures the distance to the segment (a capsule, so neighbouring
  segments join without gaps) and antialiases the edge over one pixel. An
  end behind the camera's near plane is moved onto it first.

**Styles** (`ts::VectorStyle`, the `style` object of a layer definition):
`fill` and `outline` as `#rrggbbaa`, `outlineWidthPx` in logical pixels
(scaled by the device pixel ratio). Built in:

| Layer | Source | Style |
| --- | --- | --- |
| Shoreline | base pack `shoreline` | Outline `#f2e6bf`, 1.2 px; no fill |
| Land | base pack `shoreline` | Fill `#c8b98f`; no outline. With Blue Marble hidden, land over the ocean-coloured globe |
| Graticule | generated | Outline `#dfe6f0` at 70 %, 1 px |

## The graticule

The graticule is a layer like the others (`{"kind": "graticule"}`), drawn in
its place among the vector layers. It is computed per pixel, not from
geometry. A full-screen pass casts each pixel's ray against the ellipsoid
(the same intersection as the globe's surface), and draws 10° lines,
fainter 1° lines and slightly wider equator and prime meridian lines.
Lines closer than a few pixels fade out, and the edge is antialiased with
`fwidth`. It uses the layer's outline colour, width and opacity.
`GlobeStyle::graticule` still draws it under the imagery on the bare globe
(tests). Below about 2 km eye height it fades out: its float positions
are only good to about half a metre there.

## Attribution

The application shows, in the view's bottom-right corner, the attribution
of every layer on screen: each pack dataset's attribution from its
manifest, once, top layer first (`ts::attributionsForStack()`). It goes
when the layer is hidden.

## Tests

- `tests/vector`: meshing (area covered, triangles within a cell, vertices
  on the ellipsoid, lakes left open, outlines without tile-edge segments
  at the antimeridian and the pole, line strings split, small rings
  dropped unless at the tile edge, cancellation), concurrent meshers, the
  loader (loading, empty tiles, cancellation, a stress test under TSan).
- `tests/globe` golden images: a fixture GeoPackage at the antimeridian,
  at the pole and with a lake; the graticule layer over imagery; and, with
  `TS_BASEPACK_DIR`, the real base pack over the UK, the Pacific
  antimeridian and both poles at several zoom levels (CI's base pack job on
  Linux; the Windows jobs with the pack it built). The vector and
  real-pack images leave the graticule out: at close range, llvmpipe
  versions (Mesa 25.2 here, 26.2 on Windows CI) place its 1° parallels a
  pixel apart, their `asin` differing slightly. The graticule has its own
  images at whole-globe scale, where they agree.
- `tests/smoke`: the application starts offline (proxies pointing at a
  closed port) with the base layers drawn and credited.

## Budgets and measurements

Measured with the real pack on Mesa llvmpipe (320 × 240, the golden-image
views; `tests/globe` logs them):

| View | Stored tiles drawn | Outline segments |
| --- | --- | --- |
| Whole globe, either pole from far away | 2 (crude) | 11 200 |
| UK at 2 000 km | 12 (low) | 56 000 |
| London at 150 km, tilted | 24 | 90 000 |
| Fiji at the antimeridian, 1 500 km | 24 | 37 000 |
| Northern Greenland at 1 500 km | 29 | 70 000 |

Meshing a busy high-resolution tile (a 20 000-vertex coastline with a
lake, about 20 600 triangles) takes about 100 ms on one worker
(`tests/vector`'s benchmark); the two workers do it while the coarser
level stays on screen. Fills are meshed even for outline-only layers, so
a Land layer on the same source shares the mesh.
