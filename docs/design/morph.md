# The 2D map and the 3D/2D morph

How one view shows the globe, a flat map, and everything between: where
the map lies in space, how every vertex gets two positions, what the
camera does during the change, how the map is navigated, and what is
picked while it moves. The projections themselves are in
[projections.md](projections.md). Code: `ts::Camera` and
`ts::CameraController` (`core/include/ts/camera/`), the renderers in
`core/include/ts/globe/`, `ts::TileSelector`.

## One number

The camera has a **morph factor** t (`Camera::morph()`): 0 is the globe, 1
the flat map in the camera's projection (`Camera::mapProjection()`).
Everything that draws reads it. There is no second view and no second
renderer; the 2D map is the same scene with t = 1.

At t = 0 nothing in this document applies: the globe is ray-cast and the
tiles are drawn exactly as before, and the stored golden images of the
globe did not change.

## Where the map is

The map is drawn on the **plane that touches the ellipsoid at the
camera's target** (the centre of the view), with map east along the
target's east, map north along its north, and scaled so that **east-west
distances at the target are true**:

    plane position = target + E (x − x₀) s + N (y − y₀) s
    s = 1 / k(φ₀)        (k: the projection's scale along the parallel)

for a point at map position (x, y), the target at (x₀, y₀) and latitude
φ₀. `Camera::mapFrame()` returns these (`ts::MapFrame`).

The camera looks at that plane exactly as it looks at the globe: same
eye, same range, same perspective. Looking straight down at a plane, a
perspective view is a plain scaled map, so nothing special is needed for
"2D". Three things follow from the choice:

- **Close to the centre the two views agree.** The plane touches the
  globe there at the right scale (and for the conformal Mercator in both
  directions), so zoomed in on a city the morph moves almost nothing;
  zoomed out it unrolls the whole globe.
- **The centre stays put**, by construction, for every t.
- **The scale bar means the same thing** in both views: the range that
  shows 50 km across the view on the globe shows 50 km at the centre of
  the map.

The map's own zoom is range / s. Panning north on a Mercator map must not
make the map grow, so the controller keeps range / s constant while the
centre moves and lets the range follow. Changing back to the globe from
anywhere is then continuous.

## Two positions per vertex

Every vertex has a position on the globe and one on the map, and the
vertex shader blends them:

    position = mix(globe position, map position, t)      (relative to the eye)

The globe position is what the meshes always held: a float offset from
the tile's centre, with the centre and the eye as high/low pairs
([camera-and-precision.md](camera-and-precision.md)).

The **map position is computed in the vertex shader**, not stored. A tile
vertex knows its longitude and latitude from its texture coordinate; a
vector vertex stores its offset in longitude and latitude from its tile's
centre (it stored absolute degrees before, good to a metre; the offset is
good to centimetres). From that offset the projection's GLSL gives the
vertex's map offset from the tile centre ([projections.md](projections.md)),
and the CPU supplies, per draw and in double precision, where the tile
centre lies on the plane relative to the centre of the view:

```glsl
vec3 tsMorph(vec3 relative, vec2 dLonLatRad, vec4 mapOrigin, vec4 r0, vec4 r1)
{
  vec2 plane = mapOrigin.xy + morph.y * tsProject(int(morph.z), dLonLatRad, r0, r1);
  vec3 onMap = planeOrigin.xyz + planeEast.xyz * plane.x + planeNorth.xyz * plane.y;
  return mix(relative, onMap, morph.x);
}
```

Both terms of `plane` are small near the camera, so the map is as steady
as the globe at any zoom. The roadmap described this as storing both
positions in the mesh. Computing the second instead means:

- changing projection changes two uniforms: no mesh is rebuilt, no tile
  reloaded, nothing flashes;
- meshes don't grow (the GPU budget is shared with system memory on the
  reference PC);
- the projection exists once, in `ts/projection`, for the CPU and the GPU,
  and a test compares the two.

The cost is a few trigonometric functions per vertex while t > 0.

### What is drawn when t > 0

| | Globe (t = 0) | Map and morph (t > 0) |
| --- | --- | --- |
| Surface colour | Ray-cast ellipsoid | On the tiles, with the first imagery draw (`TileRenderer::draw(…, withBase)`) |
| Imagery | Tiles | The same tiles and shader |
| Graticule | Ray-cast per pixel | On the tiles, from each fragment's longitude and latitude (`TileRenderer::drawGraticule()`) |
| Vectors | No depth test; far side discarded by the ellipsoid's normal | Tested against the tiles' depth |
| Markers | Instanced at their ECEF positions | The CPU places them between globe and map each frame |
| Shading | Diffuse light | Fades out with t: the map is drawn in its true colours |

A ray can't be cast against a surface half-way between an ellipsoid and a
plane, so while t > 0 the surface and the graticule are drawn on the tile
meshes, which do morph. Tiles are therefore selected even with no imagery
layer.

**Depth.** Part of the way through, the unrolling globe hides part of
itself. Tiles keep their depth test and logarithmic depth. Things drawn
*on* the surface (vectors, the graticule) can't use the globe's "far
side" test, which means nothing for a half-unrolled sheet; they are
tested against the tiles' depth instead, with their own depth brought a
tenth nearer plus 10 m (`tsOverlayDepth()`), because their triangles are
not the tiles' triangles and would otherwise fight them. On the finished
map everything is at one depth and the bias simply lets them through.

**The world wraps around.** The map repeats every 2πa in x. Each tile is
placed in the copy of the world nearest the centre of the view, and a
tile that straddles the edge opposite the centre is drawn twice, once on
each side (`MapFrame::copyShiftsM()`); vectors likewise, per stored tile.
Because tiles end at the antimeridian and are placed whole, **nothing is
ever stretched across the map**: there is no antimeridian special case in
the shaders. Zooming out stops when the view is one world wide
(`Camera::maximumMapRangeM()`), so the map never shows the same place
twice.

**Cracks.** On the globe a skirt hides the hairline between neighbouring
tiles, whose shared edge is computed from two different centres. On the
map the skirt has no height, so tiles are drawn 0.01 % larger about their
centres instead (a fortieth of a texel).

**The edge of Mercator.** Vertices beyond the projection's latitude limit
are held on it, so the tiles' triangles above about 85° collapse to
nothing and the row that contains the limit is squeezed into what is left
of it. The map's last fraction of a degree is therefore slightly
compressed; nothing is drawn beyond it.

### How fine the geometry must be

A mesh is straight between its grid lines. That matters twice:

- **During the morph** the globe must stay round, so the coarsest tiles
  drawn while t > 0 are level 2 (`TileRendererSettings::minimumMapLevel`):
  2.8° per grid segment.
- **On the finished map** a tile is a rectangle with the right corners,
  but the projection curves *inside* it in latitude: imagery and the
  graticule would sit a few pixels off towards the top and bottom of a
  Mercator map. The selection therefore splits a tile while the middle of
  its most poleward grid row is more than half a pixel from where the
  projection puts it (`TileSelector::mapGeometryErrorPx()`), whatever the
  imagery's deepest level; a layer without data that deep is drawn from
  its ancestors' images as usual. Near the equator, and on Miller almost
  everywhere, this asks for nothing.

On the finished map of a cylindrical projection nothing varies *along* a
grid row, so each tile is drawn as one column of 16 quads
(`tileMeshStripIndices()`): 32 triangles instead of 640. That is what
keeps the map as cheap as the globe on software renderers, whose cost is
mostly triangles (see Measurements).

### Selection on the map

`TileSelector` reads the camera's morph factor ([tiling.md](tiling.md)):

- **t = 1.** A tile is drawn if its rectangle on the map, in any copy of
  the world, meets the view, and it lies inside the projection's
  latitudes. Its screen-space error is the size of a texel along a
  meridian of the map, which depends on the latitude and the zoom, not on
  the distance from the centre.
- **0 < t < 1.** A tile is drawn if *either* view would show it, without
  the globe's horizon test (the far side unrolls into view), at the
  larger of the two errors.

Vector sources use the same selector.

## The camera during the change

`CameraController::setViewMode(mode, duration)` animates t with an
ease-in-out (smoothstep) over the duration: 1 s, or 0.5 s on a software
renderer (`GlobeView::morphDurationS()`), where every frame of the change
costs more. A duration of 0 changes at once (opening a project).

- The **centre** does not move. The exception is a centre the map can't
  have (beyond Mercator's edge, or within a degree of a pole on Miller):
  then it slides to the nearest latitude the map allows.
- Going to the map, **heading and tilt** ease to zero with t (the heading
  the short way round): the map is north-up, seen from straight above.
  Going back they stay at zero.
- The **range** stays, unless the view would be wider than the world on
  the map; then it eases in to that limit.
- Like every motion here it is driven by elapsed time: at a low frame
  rate frames are skipped, not slowed, and a stall is one clamped step.
- **Asked to turn round** part of the way, it goes back from where it is,
  taking proportionally less time.
- **Input waits.** Drags, the wheel and held keys do nothing during the
  second the change takes; `setPose()` and `flyTo()` finish it at once
  first.

At exactly t = 1 the camera limits its pose to what a map allows
(`Camera::setPose()`): heading and tilt 0, the latitude inside the
projection's domain, the range within `maximumMapRangeM()`.

## Navigating the map

The same controller and the same input as the globe
([camera-and-precision.md](camera-and-precision.md)), computed in map
coordinates:

| Input | On the map |
| --- | --- |
| Left drag | Slides the map: the grabbed place stays under the pointer. The pointer is a fixed offset from the centre in map coordinates, so the new centre is exact in one step |
| Release while moving | Inertia with a velocity in map metres per second, decaying as e^(−3t) |
| Wheel, Page Up/Down, + / − | Zoom as on the globe, keeping the place under the pointer |
| Arrows / W A S D | Slide at half the map's range per second: straight lines on the map |
| Right or middle drag, Q / E, R / F | Nothing: a map doesn't turn or tilt |
| Double click, Home | Fly as on the globe, ending north-up |

Sliding east or west goes **round the world**: the longitude wraps and
the map carries on. North and south stop at the projection's latitude
limit. Motion in map coordinates is additive, so the frame-rate
independence of the globe's navigation holds here exactly: the same held
keys at 10, 20, 60 and 144 frames per second end at the same place.

## Picking

`Camera::pickGeodetic()` and `project()` use **the globe while t < 0.5
and the map from t = 0.5** (`Camera::isMap()`).

- On the map a pixel's ray is intersected with the map's plane, its
  offset from the centre converted to map coordinates, wrapped into the
  world in x, and inverted by the projection. Above or below the map
  there is nothing to pick.
- During the change the surface is neither, so a pick is only
  approximate there, except at the centre of the view, which both
  mappings give exactly. Nothing depends on it: the pointer readout in
  the status bar is the only reader during the second it lasts, since
  input waits.

**Measuring is geodesic in every view.** A distance is the geodesic
between two picked points on the ellipsoid, whatever is on screen;
`tests/camera` picks Paris and Berlin from the screen on the globe and on
all three maps and gets the same distance to a millimetre. The **scale
bar** (`ts::scaleBarFor()`) is measured the same way: the geodesic
between two points a few pixels either side of the centre of the view,
rounded down to 1, 2 or 5 × 10ⁿ. Where the scale at the top or bottom of
the view differs from the centre's by more than a tenth (a Mercator map
of a continent, a whole globe) the bar says where it holds: "2,000 km at
40° N".

## In the application

- **View → 2D Map** (Ctrl+M) changes between the globe and the map.
- **View → Map Projection** picks Mercator, Web Mercator or Miller, at
  once and at any time; it takes effect the next time the map is shown,
  or immediately if it is.
- **View → Scale Bar** shows or hides the bar in the bottom-left corner.
- Project files keep the view and the projection (format version 2,
  [layer-model.md](layer-model.md)); a project can be given on the command
  line.
- The debug overlay (F3) shows the projection and how far the change is.

WMS layers (Phase 5) are imagery layers and inherit all of this.

## Tests

- `tests/projection`: the projections ([projections.md](projections.md)).
- `tests/camera`: the map's pose limits; `MapFrame` and the copies of the
  world; pixel ↔ point round trips on every map, off the map and across
  the antimeridian; north up and true scale at the centre; the same
  geodesic distance in every view; the change (centre fixed in every
  frame, half-way at half time, frame-rate independence at 10/20/60/144
  fps, stalls, turning round, input ignored, an unreachable centre); map
  navigation (drag and zoom keep the place under the pointer, the map
  keeps its size, keys and inertia are frame-rate independent, wrapping
  round the world, stopping at the edge, flights); the scale bar.
- `tests/tiling`: selection on the map (every point in view covered once,
  texels within the limit, nothing out of view), culling including across
  the wrap and beyond Mercator's edge, the error following the
  projection, the minimum level, the geometry error, both views during
  the change, the strip indices.
- `tests/globe` (GPU): golden images at t = 0, 0.5 and 1 with imagery,
  vector fills and outlines, the graticule layer and markers; a tilted
  view a quarter of the way; Miller and Web Mercator world maps;
  polygons and markers across the antimeridian without streaks; the top
  of the Mercator map and Miller's pole; a marker at the centre staying
  within 0.6 px of the middle of the image at eleven steps of the change,
  and one off-centre moving smoothly between where the camera says it is
  at either end; the map filling the view edge to edge across the wrap;
  the frame rate of a scripted change; and, with `TS_BASEPACK_DIR`, Blue
  Marble and the shoreline on both world maps, over the UK, across the
  Pacific antimeridian, Antarctica on Miller, and half-way through the
  change.
- `tests/ui`: View → 2D Map there and back with the animation, the
  pointer readout and a drag on the map, no turning, the scale bar, no
  frames while idle; the projection menu.
- `tests/layers`, `tests/smoke`: project files of version 1 and 2; the
  application opening a project saved in the map view.

## Measurements

Mesa llvmpipe (LLVM 20), 320 × 240, checkerboard imagery, release build
(`tests/globe` benchmarks):

| Frame | Time |
| --- | --- |
| Globe with imagery | 5.4 ms |
| Mercator map of the same area | 6.1 ms |
| Half-way through the change | 17 ms |
| A scripted change with imagery, vectors and the graticule, mean of 31 frames | 19 ms (54 fps) |

Before tiles on the map were drawn as strips and the level-3 floor was
replaced by the geometry-error test, the map frame took 24 ms and the
scripted change 46 ms: on a software renderer the cost of these frames is
the number of triangles, not pixels or draw calls. `morphHoldsFrameRate`
fails when the frames of the scripted change take more than 100 ms (10
frames per second, the budget for software rendering), judged by its
fastest quarter so that a busy CI machine doesn't fail it; the reference
PC's 20 fps target needs its hardware and is not checked in CI.
