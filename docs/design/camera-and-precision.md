# Camera and precision

How the camera is described, how it turns input into motion at any frame
rate, and how the globe stays precise from 50 000 km down to one metre
above the ground. Code: `core/include/ts/camera/`,
`core/include/ts/globe/globerenderer.h`, `app/src/globeview.h`.

## The camera

`ts::CameraPose` is an orbit around a point on the ellipsoid:

| Field | Meaning | Limits |
| --- | --- | --- |
| `targetLatDeg`, `targetLonDeg` | The point looked at, on the surface (height 0); always at the centre of the view | latitude ±89.999 999° (heading is undefined at a pole) |
| `rangeM` | Distance from the eye to the target | at most 5 × 10⁷ m; at least what keeps the eye 1 m above the ellipsoid |
| `headingDeg` | Direction the camera faces, clockwise from north | [0, 360) |
| `tiltDeg` | 0 looks straight down, towards 90 looks at the horizon | [0, 85] |

With the target's east-north-up frame (E, N, U) and h = sin ψ E + cos ψ N
for heading ψ and tilt τ:

    eye     = target + range (cos τ U − sin τ h)
    forward = sin τ h − cos τ U
    up      = cos τ h + sin τ U
    right   = forward × up

`ts::Camera` turns the pose and the viewport (logical pixels, 45° vertical
field of view) into all of that in double precision, plus:

- `rayThroughPixel(x, y)`: the picking ray (unit direction). Pixel
  coordinates are continuous; pixel (i, j)'s centre is (i + 0.5, j + 0.5).
- `pickGeodetic(x, y)`: the ray against the ellipsoid
  (`ts::intersectEllipsoid`), as latitude and longitude. The status bar
  readout and every mouse interaction use it.
- `project(point)`: the inverse; screen position and depth.
- `viewRotation()`, `projection()`: matrices for **eye-relative** ECEF
  offsets. No matrix ever contains the eye's translation (see below).
- Near and far planes: near is half the eye height (0.05 m minimum), far
  is past the horizon of the polar-radius sphere plus 50 km.

The camera also has a **morph factor** between the globe (0) and a flat
map (1) and the map's projection. On the map the same pose applies, seen
from straight above with north up; picking, `project()` and the clip
planes follow. That is all in [morph.md](morph.md), with the map's own
navigation; the rest of this document is the globe.

## Navigation

`ts::CameraController` holds the camera and turns input into motion. It
takes positions in logical pixels and times from any monotonic clock, so
the tests drive it with a fake clock.

| Input | Motion |
| --- | --- |
| Left drag | Pan. The grabbed point of the globe stays under the pointer: each move shifts the target by the lat/lon difference between the grabbed point and the point now under the pointer, repeated until they agree to 10⁻¹² ° (2–3 rounds). The heading stays fixed, so north keeps its direction |
| Release while moving | Inertia: the target keeps moving along a rhumb line at the release speed (from the last 0.1 s of the drag), decaying as e^(−3t) |
| Right or middle drag, Ctrl + left drag | Turn (180° per viewport width) and tilt (90° per viewport height) about the target |
| Wheel | Zoom by 0.8× per notch, keeping the point under the pointer in place |
| Double click | Fly to the point, three times closer |
| Arrows / W A S D | Move at half the range per second, relative to the heading |
| Page Up/Down, + / − | Zoom by 2× per second |
| Q / E, R / F | Turn 60°/s, tilt 30°/s |
| Home, View → Reset View | Fly back to the start view |

### Frame-rate independence

Motion that continues on its own (held keys, inertia, flights) advances
only in `advance(elapsedS)`, by elapsed time, never by frame count, and
each kind is integrated exactly so that the result doesn't depend on how
the time is split into frames:

- **Panning follows rhumb lines.** A rhumb line keeps its azimuth, so ten
  short moves with the heading's azimuth end exactly where one long move
  would (`ts::rhumbDirect`, GeographicLib's `Rhumb`). A geodesic would
  not: its azimuth changes along the way.
- **Zooming is exponential**, r(t) = r₀ e^(λt), and the pan speed is
  proportional to the range, so the distance covered while zooming is the
  integral r₀ (e^(λt) − 1) / λ, not a sum of per-frame steps.
- **Inertia** covers v/k (e^(−k t₀) − e^(−k t₁)) between t₀ and t₁.
- **Flights** are a function of elapsed time over a duration in seconds
  (1–4 s, longer for longer flights): the target follows the geodesic
  (`ts::geodesicDirect`), the range is interpolated in log space and rises
  on long flights so both ends stay in view, with smoothstep easing. At a
  low frame rate frames are skipped, not slowed.
- **Stalls are clamped:** one step is at most 0.25 s, so a frozen frame
  doesn't throw the camera across the globe. (At 10 fps a step is 0.1 s,
  well under the clamp.)

`tests/camera` replays the same input at 10, 20, 60 and 144 fps: held keys
end within 1 µm and 10⁻⁹ ° of each other, flights halfway through within
1 µm. Inertia ends within 1 % of the glide (0.6 % measured), because the
release speed is estimated from drag samples taken at each frame rate's
own times.

### Render on demand and frame pacing

`GlobeView` draws only when something changed. Input calls `requestFrame()`;
while the controller reports `isAnimating()`, each frame requests the
next. A single-shot timer, set by `ts::FramePacer` from the screen's
refresh rate, starts each frame no sooner than one refresh interval after
the previous one, so frames never come faster than the screen shows them,
even without vsync (remote desktop) or with a 1000 Hz mouse. When nothing
moves, no timer runs: the view uses no CPU (`tst_smoke` checks idle CPU;
`tests/ui` checks that no frames are drawn while idle).

### Terrain

With terrain shown, `Camera::setSurfaceHeight()` makes picking find the
terrain instead of the ellipsoid, and keeps the eye 20 m above it
([terrain.md](terrain.md#drawing-the-terrain)).

## Precision

A `float` has a 24-bit mantissa. ECEF coordinates are up to 6.4 × 10⁶ m,
so in float they are rounded to steps of 0.5 m. Rendered naively, the
world would jump in half-metre steps as the camera moves: invisible from
space, hundreds of pixels at one metre above the ground. Everything on
the CPU is double; three techniques keep the GPU's floats precise.

### 1. Relative to the eye

Matrices never contain the eye position. `Camera::viewRotation()` maps an
*offset from the eye* to camera space, so the large numbers cancel before
anything is rounded to float. Every position sent to the GPU is either an
offset from the eye (computed in double on the CPU), or a high/low float
pair:

    high = float(x)
    low  = float(x − high)
    GPU: offset = (pointHigh − eyeHigh) + (pointLow − eyeLow)

(`ts::splitDouble`.) For a point near the eye the high parts are equal or
differ by a few float steps, so their difference is exact, and the low
parts carry the rest to about 10⁻⁸ m. Points far from the eye lose
precision, but far away a centimetre is much less than a pixel. Markers
use this today; Phase 2's tile meshes will store vertices as offsets from
a tile centre and pass the centre as high/low.

### 2. The globe is ray-cast, not a mesh

`ts::GlobeRenderer` draws the ellipsoid without triangles: a triangle
covering the viewport gives every pixel its view ray, and the fragment
shader intersects that ray with the exact WGS 84 ellipsoid. In the frame
scaled by (1/a, 1/a, 1/b) the ellipsoid is the unit sphere, and with
e = eye / (a, a, b) and D = d (1, 1, a/b):

    qa = D·D,  qb = e·D,  qc = |e|² − 1
    q  = −(qb + sign(qb) √(qb² − qa qc))
    t  = a · (qc / q)        (metres; the near root, in stable form)

At 1 m above the surface qc is about 3 × 10⁻⁷. Computed in float from e
(whose components round to 6 × 10⁻⁸) it would be noise, so **qc is
computed in double on the CPU** and passed as a uniform; the rest has
only relative error, and t = qc / q comes out accurate to about 10⁻⁷ of
the distance, a fraction of a micrometre at 1 m. The intersection is the
same maths as `ts::intersectEllipsoid`, so the drawn globe and picking
agree at every zoom (tested: every pixel CPU picking hits shows the
globe; at 1 m up, the horizon row matches CPU picking to a pixel).

The graticule (10° lines, 1° lines when zoomed in, brighter equator and
prime meridian) comes from the latitude and longitude of the surface
normal at the hit point, antialiased with screen-space derivatives. It is
computed from float positions (about 0.5 m steps), so it fades out between
2 km and 500 m above the ground. The globe's edge is antialiased
analytically from the discriminant, so no multisampling is needed.

A mesh would need ever finer triangles near the eye: a 1° triangle's flat
face is 240 m below the curved surface in its middle. Phase 2's tiles
solve that with a quadtree; the ray-cast globe stays as the background
under unloaded tiles.

### 3. Logarithmic depth

With a near plane of centimetres and a far plane of 10⁴ km, a standard
24-bit depth buffer resolves nothing past a few kilometres. Reversed-Z
with a float depth buffer would be the usual fix, but it needs
`glClipControl` (OpenGL 4.5), above the 3.3 baseline. So the fragment
shaders write logarithmic depth:

    depth = log2(1 + w) / log2(1 + far)

where w is the distance along the view direction. With 24 bits this gives
a constant relative resolution of about 10⁻⁶: 1 µm at 1 m, 10 m at
10 000 km. The globe writes it for every pixel; markers write the same
function pulled towards the camera by a few times their own size, so the
ground around them doesn't cut their discs.

### Measured

| Check | Result | Limit |
| --- | --- | --- |
| Marker 2 m away, eye 1 m up, camera moved in 1 mm steps (41 frames): drawn vs double-precision projection | 0.025 px | 0.25 px (roadmap: 1 px) |
| Horizon row at 1 m up, GPU vs CPU picking | 0 rows | 1 row |
| Silhouette vs CPU picking, every decided pixel | 0 mismatches | 0 |
| Pixel → ray → ellipsoid → pixel | < 10⁻⁶ px | 10⁻⁶ px |

## High-DPI

The camera and all input work in logical pixels. The view renders into
`width × devicePixelRatio` device pixels; rays and projection are the same
because only the pixel density changes. Marker sizes and graticule widths
are given in logical pixels and scaled by the ratio in the shaders
(`tests/globe` renders at ratio 2 and checks marker size and position).
