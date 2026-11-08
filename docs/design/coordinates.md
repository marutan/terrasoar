# Coordinates

Every position in Terra Soar refers to the WGS 84 ellipsoid. This page
lists the parameters, the frames the code uses, the conversion formulas
and how accurate each one is. The code lives in `core/include/ts/geo/` and
`core/include/ts/maths/`; the tests that hold it to these figures are in
`tests/geo/` and `tests/maths/`.

## WGS 84 parameters

`ts/geo/wgs84.h` (NGA.STND.0036, EPSG:7030). Two constants define the
ellipsoid; the rest are derived from them at compile time.

| Constant | Symbol | Value |
| --- | --- | --- |
| Semi-major axis (defining) | a | 6 378 137 m |
| Flattening (defining) | f | 1 / 298.257 223 563 |
| Semi-minor axis | b = a (1 − f) | 6 356 752.314 245 m |
| First eccentricity squared | e² = f (2 − f) | 0.006 694 379 990 14 |
| Second eccentricity squared | e′² = e² / (1 − e²) | 0.006 739 496 742 28 |

`tests/geo` checks the derived values against the published ones.

## Frames

| Frame | Type | Units | Notes |
| --- | --- | --- | --- |
| Geodetic (EPSG:4979) | `ts::Geodetic` | degrees, metres | Latitude first, unlike EPSG:4979's normalised (lon, lat) order. Height is above the ellipsoid, not above sea level. |
| ECEF (EPSG:4978) | `ts::Vec3` | metres | x through (0°, 0°), y through (0°, 90° E), z through the north pole. Right-handed. |
| Local east-north-up | `ts::LocalFrame` | unit vectors in ECEF | Up is the ellipsoid normal (geodetic vertical); east × north = up. |

Longitudes may hold any value; `normaliseLonDeg()` brings one into
[−180, 180). Functions that return a longitude give it in [−180, 180].

All positions are `double`. A `float` has a 24-bit mantissa, so at the
Earth's radius it can't resolve better than about half a metre; the
renderer will subtract the eye position in double before going to the GPU
(see `camera-and-precision.md`, Phase 1).

Matrices (`ts::Mat4`) are column-major, as OpenGL expects, and act on
column vectors: `a * b` applies `b` first.

## Geodetic → ECEF

Closed form, exact to rounding (`geodeticToEcef()`):

    N = a / √(1 − e² sin²φ)          prime-vertical radius of curvature
    x = (N + h) cos φ cos λ
    y = (N + h) cos φ sin λ
    z = (N (1 − e²) + h) sin φ

## ECEF → geodetic

`ecefToGeodetic()` uses Vermeille's closed-form solution (H. Vermeille,
"An analytical method to transform geocentric into geodetic coordinates",
J. Geodesy 85, 2011, section 2). It needs one cube root and a few square
roots, no iteration, so its cost and accuracy are the same everywhere:

    p = (x² + y²) / a²
    q = (1 − e²) z² / a²
    r = (p + q − e⁴) / 6
    s = e⁴ p q / (4 r³)
    t = ∛(1 + s + √(s (2 + s)))
    u = r (1 + t + 1/t)
    v = √(u² + e⁴ q)
    w = e² (u + v − q) / (2 v)
    k = √(u + v + w²) − w
    D = k √(x² + y²) / (k + e²)

    φ = 2 atan2(z, D + √(D² + z²))
    λ = atan2(y, x)
    h = (k + e² − 1) / k · √(D² + z²)

On the polar axis (√(x² + y²) < 1 nm) the general formula is 0/0, so the
code returns φ = ±90°, λ = 0 and h = |z| − b directly.

The formula holds outside the evolute of the ellipsoid, a small region
around the centre (within about 43 km of it). Nothing in Terra Soar goes
that deep; the tests cover 1000 km below the surface and out.

### Why not PROJ or an iteration

PROJ's own inverse iterates with a stopping tolerance and loses accuracy
far from the surface (see the table below), and going through a `PJ`
object for every picked pixel or tile corner ties hot code to a PROJ
context per thread. An iteration such as Bowring's is accurate near the
surface but needs a convergence test and a varying number of steps.
Vermeille's closed form has neither, and is accurate at every height the
application uses. PROJ stays the reference the tests compare against.

## Normals and the local frame

- `surfaceNormalEcef(φ, λ)`: (cos φ cos λ, cos φ sin λ, sin φ).
- `ellipsoidNormalEcef(P)`: the normalised gradient (x/a², y/a², z/b²) of
  the ellipsoid through P. On the surface it equals the line above without
  any trigonometry, which suits per-vertex work.
- `localFrame(φ, λ)`: east (−sin λ, cos λ, 0), north
  (−sin φ cos λ, −sin φ sin λ, cos φ), up as above.

## Ray–ellipsoid intersection (picking)

`intersectEllipsoid()` scales x and y by 1/a and z by 1/b, which turns the
ellipsoid into the unit sphere without changing the ray parameter t, and
solves |O + tD|² = 1. The roots are taken in the stable form
q = −(B + sign(B)√disc), t = q/A and C/q, so a ray from far away that
grazes the surface doesn't lose precision by subtracting nearly equal
numbers. A ray starting outside returns the entry point, one starting
inside returns the exit point, a tangent ray counts as a hit, and a zero
direction never hits.

The screen-to-ray step is `ts::Camera::rayThroughPixel()`, and
`ts::Camera::pickGeodetic()` combines the two (see
[camera-and-precision.md](camera-and-precision.md)); `tests/camera` checks
pixel → ray → surface → pixel round trips.

## Geodesics

`geodesicInverse()` and `geodesicDirect()` wrap GeographicLib's WGS 84
`Geodesic` (C. F. F. Karney, "Algorithms for geodesics", J. Geodesy 87,
2013). Karney's method is accurate to about 15 nm for any pair of points,
nearly antipodal ones included, where Vincenty's formulae fail to
converge. Distances are on the ellipsoid surface; heights are ignored.
Azimuths are degrees clockwise from north in (−180, 180].

`rhumbDirect()` and `rhumbInverse()` do the same for rhumb lines (constant
azimuth) with GeographicLib's `Rhumb`. The camera pans along rhumb lines
because a rhumb line split into pieces is the same line, which makes
panning independent of the frame rate.

GeographicLib's `Geodesic::WGS84()` is constructed once (thread-safe since
C++11) and only its const members are called, so both functions are
thread-safe.

## Accuracy

Measured by `tests/geo` (GCC 13, x86-64); the tests fail above the
tolerance column.

| What | Measured | Tolerance |
| --- | --- | --- |
| Geodetic → ECEF → geodetic → ECEF, every whole degree incl. poles and ±180°, heights −1000 km … 36 000 km (392 046 points) | 1.8 × 10⁻⁸ m | 1 mm |
| Same, height alone | 1.5 × 10⁻⁸ m | 1 mm |
| Same, latitude and longitude on an offset grid (not reported) | — | 10⁻⁹ ° |
| Geodetic → ECEF against PROJ (EPSG:4979 → 4978) | 0 m | 1 mm |
| ECEF → geodetic against PROJ, 10 km below to 100 km above the surface | 1.3 × 10⁻⁴ m | 1 mm |
| ECEF → geodetic against PROJ, 1000 km below / 36 000 km above | 0.019 m / 0.39 m | reported only (PROJ's iteration, see above) |
| Ray pick straight down from 1000 km, and from ten Earth radii to points up to 70° off-centre (not reported) | — | 1 mm |
| Geodesic distance, 500 cases from Karney's test set | 7.5 × 10⁻⁹ m | 1 µm |
| Geodesic direct end point, same cases | 5.7 × 10⁻¹⁴ ° | 10⁻¹² ° (≈ 0.1 µm) |

The geodesic cases are every 20th line of `GeodTest-short.dat` (Karney,
"Test set for geodesics", <https://doi.org/10.5281/zenodo.32156>, CC0), in
`tests/data/geodtest-sample.dat`, so each category in the file (random,
nearly antipodal, short, polar, meridional, equatorial) is sampled.

## Cost

From the `benchmark*` tests (`-O2`, one core, same machine as above; a
guide to orders of magnitude, not a baseline):

| Operation | Time |
| --- | --- |
| `geodeticToEcef()` | 42 ns |
| `ecefToGeodetic()` | 190 ns |
| `intersectEllipsoid()` | 18 ns |
| `geodesicInverse()` | 1.7 µs |
