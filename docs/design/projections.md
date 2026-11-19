# Projections

The map projections of the 2D views: what the interface is, the formulas
of the three projections, their domains, how they run in shaders, and how
to add one. Code: `core/include/ts/projection/`. How a projection is put
on screen is in [morph.md](morph.md).

## The interface

`ts::Projection` maps the WGS 84 ellipsoid to a plane in metres. A
projection is immutable and thread-safe; the ones the program offers are
created once (`ts::projections()`) and live for the whole run.

| Member | What it gives |
| --- | --- |
| `id()`, `displayName()` | The name in project files (`"mercator"`) and the translated name in the menu |
| `projString()` | The same projection as a PROJ string, for the tests and for WMS (Phase 5) |
| `centralMeridianDeg()` | The longitude that maps to x = 0 (a constructor argument; 0 for the registered ones) |
| `minimumLatDeg()`, `maximumLatDeg()` | The valid domain in latitude |
| `forward(lat, lon)` | The map position. Longitudes wrap to within 180° of the central meridian; a latitude outside the domain lands on the map's edge |
| `inverse(point)` | The point on the ellipsoid, or nothing outside `bounds()` |
| `bounds()` | The rectangle the domain maps to |
| `scaleAt(lat)` | How much the map stretches the ground along the parallel (k) and the meridian (h) |
| `isCylindrical()` | True if a longitude/latitude rectangle is a rectangle on the map |
| `glslFunction()`, `glslSource()`, `shaderReference(lat, lon)` | The projection as GLSL (below) |

`ts::projections()` lists the projections in menu order (the first is the
default), `ts::projectionById()` finds one by its identifier, and
`ts::projectionIndex()` gives its index, which shaders use.

The view is always centred on the camera, not on the central meridian: the
renderer only ever uses *differences* of map positions, and the map
repeats every `bounds().widthM()` in x. So the central meridian matters to
`forward()` and `inverse()` as coordinates (a WMS request in EPSG:3857,
say), not to what is drawn.

## The Mercator family

Mercator, Web Mercator and Miller are normal cylindrical projections of one family
(`ts::CylindricalProjection`), with a the WGS 84 semi-major axis, λ the
longitude from the central meridian and φ the latitude:

    x = a λ
    y = a (c atanh(sin(s φ)) − e atanh(e sin(s φ)))

| Projection | `id` | s | c | e | Latitude limit | As PROJ |
| --- | --- | --- | --- | --- | --- | --- |
| Mercator (ellipsoidal, as EPSG:3395) | `mercator` | 1 | 1 | WGS 84's, 0.0818… | ±85.0841° | `+proj=merc +ellps=WGS84` |
| Web Mercator (EPSG:3857) | `web-mercator` | 1 | 1 | 0 | ±85.0511° | `+proj=webmerc +ellps=WGS84` |
| Miller Cylindrical | `miller` | 0.8 | 1.25 | 0 | ±90° | `+proj=mill +ellps=WGS84` |

- **Mercator** is conformal: h = k everywhere, and a straight line is a
  line of constant bearing. `atanh(sin φ)` is the familiar
  ln tan(π/4 + φ/2); the second term is the ellipsoid's correction.
- **Web Mercator** applies the spherical formula to geodetic latitudes, as
  web maps do. It is not quite conformal (h and k differ by up to 0.7 %)
  and lies up to about 43 km north of the ellipsoidal map's y at high
  latitudes. It is there because it is what tile and WMS servers speak.
- **Miller** is Mercator computed at 0.8 φ and stretched back by 1.25,
  which keeps the poles on the map (as lines) at the price of being
  neither conformal nor equal-area. PROJ's `mill` is spherical with radius
  a, and so is this.

Mercator's y is infinite at the poles, so both Mercator maps are **cut
where the map is square** (y = ±πa): about 85.05° for Web Mercator, the
usual limit, and 85.08° for the ellipsoidal one. `forward()` places
anything beyond on the edge; the renderer therefore draws nothing above
the edge rather than something infinite.

**Inverse.** With ψ = y / a, the latitude solves
sin(s φ) = tanh((ψ + e atanh(e sin(s φ))) / c). The right-hand side moves
with sin(s φ) by a factor of about e² (0.0067), so starting from the
spherical answer the iteration converges to double precision in four or
five steps; with e = 0 it is closed-form. The code uses atan(sinh u) for
asin(tanh u), which doesn't lose precision towards the poles.

**Scale.** The ground is the WGS 84 ellipsoid whatever the formulas
assume: a radian of longitude is N cos φ metres and a radian of latitude M
metres, with N and M the prime-vertical and meridional radii of curvature.
So k = a / (N cos φ) for all three, and h = (dy/dφ) / M.

## Other normal cylindrical projections

`ts::NormalCylindricalProjection` (`normalcylindrical.h`) is the general
case, x = a k λ and y = a f(φ) with a constant k and any northing
function f, which each subclass gives with its inverse, its derivative
and its own GLSL. The map repeats every 2πak, so maps narrower than
Mercator's wrap with their own width (`bounds().widthM()`).

| Projection | `id` | k | f(φ) | Latitude limit | As PROJ |
| --- | --- | --- | --- | --- | --- |
| Equidistant Cylindrical | `equidistant-cylindrical` | cos φ1 | φ | ±90° | `+proj=eqc +lat_ts=φ1 +R=6378137` |
| Gall Stereographic | `gall-stereographic` | 1/√2 | (1 + √2/2) tan(φ/2) | ±90° | `+proj=gall +R=6378137` |
| Gall–Peters | `gall-peters` | k0 (below) | q(φ) / (2 k0) | ±90° | `+proj=cea +lat_ts=45 +ellps=WGS84` |

- **Equidistant Cylindrical** has a parameter, the standard parallel φ1
  (0°, Plate Carrée, by default; 0–85°). Spherical with radius a, as
  PROJ's `eqc`.
- **Gall Stereographic** is spherical with radius a, as PROJ's `gall`.
  In GLSL the northing difference tan(A + h) − tan(A) is written
  sin(h) / (cos A cos(A + h)), with A = φt/2 and h = d/2.
- **Gall–Peters** is the cylindrical equal-area projection on the WGS 84
  ellipsoid with standard parallels ±45°: k0 = cos φ1 / √(1 − e² sin² φ1)
  and q(φ) = (1 − e²)(sin φ / (1 − e² sin² φ) + atanh(e sin φ) / e).
  The inverse solves q(φ) = 2 k0 y / a by Newton's method; q is flat at
  the poles (q ≈ qp − c (π/2 − φ)²), so a northing within a few units in
  the last place of the pole's is taken as the pole. PROJ's own inverse
  uses a truncated series that is out by about 10⁻⁸° (2·10⁻⁶° at its south
  pole); the tests take the point itself as the reference where PROJ
  doesn't come back to it. In GLSL the differences of both terms of q are
  written without subtracting nearly equal numbers (see
  `normalcylindrical.cpp`).

## Parameters

A projection may have settings (`Projection::parameters()`: a key, a
label, a default and a range). `projections()` holds the default
instance of each; `projectionWith(id, values)` gives an instance with
other values, made once and kept for the whole run, so the camera and the
renderers can keep plain pointers. Values are clamped to their ranges.
An instance with parameters shares its default instance's shader index
(`projectionIndex()` goes by `id()`): the values reach the shader in the
tile's `shaderReference()` floats, which are worked out every frame.

**View → Map Projection → Projection Settings…** (`ProjectionSettingsDialog`)
edits them for the projection shown. Project files keep them in
`view.projectionParameters`, an optional object of numbers by key,
written only when there are any (so the format version is unchanged).

## In shaders

One mesh serves the globe and every projection: the vertex shader places
each vertex on the map from its longitude and latitude, with the
projection's own GLSL ([morph.md](morph.md) says why, and what the shader
does with the result). Floats can't hold a map coordinate to better than
about a metre, so a projection's GLSL function does not project a
position. It projects a **difference from a reference point**:

```glsl
vec2 tsProjectMercator(vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
```

returns, in metres, `forward(reference + d) − forward(reference)` for an
offset d in longitude and latitude (radians) from a reference point. The
reference point is a tile's centre; what the function needs to know about
it comes from `shaderReferenceFor(lat, lon, halfSpan)` (halfSpan: how
far the tile reaches from it), computed in double precision and passed as
four `vec4` uniforms whose meaning is the projection's own (the
cylindrical ones use two).
Near the reference the result is as precise as the offset, so a tile a
few metres across is placed to micrometres.

For the cylindrical family the x difference is `a × dLon`, exactly. The y
difference must not be computed as y(φ) − y(φ₀), which would subtract two
numbers of millions of metres. Two identities avoid it:

    sin(p + q) − sin(p)   = 2 cos(p + q/2) sin(q/2)
    atanh(u) − atanh(v)   = atanh((u − v) / (1 − u v))

so with S = sin(s φ₀) and C = cos(s φ₀) from the reference (in double) and
q = s × dLat:

    D  = 2 (C cos(q/2) − S sin(q/2)) sin(q/2)            = sin(s φ) − S
    dy = a (c atanh(D / (C² − S D)) − e atanh(e D / (1 − e² S² − e² S D)))

Nothing here cancels. The reference carries
`r0 = (S, C, least dLat, greatest dLat)` and
`r1 = (1 − e² S², e, s, c)`; the latitude limits clamp `dLat`, which is
how the map ends at its edge. GLSL's own `atanh` is often
log((1 + x) / (1 − x)) / 2 and loses small arguments completely (a metre
is 1.6 × 10⁻⁷ of the radius), so `tsAtanh()` uses the series below
|x| = 0.2.

`ts::projectionGlsl()` returns the shared helpers, every registered
projection's function, and the dispatcher shaders call:

```glsl
vec2 tsProject(int index, vec2 dLonLatRad, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
```

with `index` the projection's `projectionIndex()`, a uniform. All
projections are therefore in every shader, and changing projection changes
a uniform: no shader is rebuilt and no mesh reloaded.

**How good it is.** `tests/projection` draws with this GLSL and reads the
results back: offsets from a millionth of a degree to 85° either side of
nine reference latitudes, for each projection, against the C++
`forward()`. The allowance is 2 × 10⁻⁶ of the offset for nearby points,
rising to 3 × 10⁻⁵ for points tens of degrees away (which only the
coarsest tiles have, where a pixel is kilometres); the worst case measured
on Mesa llvmpipe uses 17 % of it.

## Maps that don't repeat: Transverse Mercator, UTM, Cassini

Projections whose x and y both depend on both coordinates derive from
`ts::LocalExpansionProjection` (`localexpansion.h`). Writing a careful
difference formula for each would be long and error-prone, so the GPU
places their vertices in one of two ways, chosen per tile by its
half-span:

- **Small tiles** (half-span under `localExpansionLimitRad()`, 0.01 rad,
  about 64 km): the second-order Taylor expansion about the reference,
  `F(ref + d) − F(ref) ≈ J d + ½ dᵀ H d`. J and H come from central
  differences of `project()` in double precision (step 10⁻⁴ rad; near a
  pole the stencil moves inside and the expansion is carried back). The
  expansion's error is a third derivative times d³: about a metre at the
  largest such tile, whose texels are 500 m, and nothing large is
  subtracted in float. `r0` holds J, `r1` and `r2.xy` H, `r2.w` = 1.
- **Large tiles**: the projection's formula in float
  (`glslForwardSource()`, its own name plus `Forward`) at the vertex and
  at the reference, subtracted: a metre or so of rounding against texels
  of kilometres. `r0` and `r1` hold its parameters, `r2.xy` the reference.

`r3` holds how far the domain lets d go, and the shader clamps d to it, so
geometry beyond the edge is squashed onto it (as latitudes are on
Mercator). GLSL's `atan` and `asinh` may be far from float precision
(Mesa's `atan` is out by about 10⁻⁵, tens of metres here), so the formula
uses `tsAtan2` and `tsAsinh` from `projection.cpp`: two exact halvings of
the argument, then a short series.

The subclass gives `project(lat, dLon)` (radians, no limits: it must be
smooth a little past the domain), a first guess for the inverse, the GLSL
formula and its parameters. The base class does the rest: `inverse()`
refines the guess by Newton's method on `project()` (so it inverts this
forward exactly), `bounds()` samples the domain's outline, `scaleAtPoint()`
measures the Jacobian against the ellipsoid, and `nearestInDomain()` (for
a drag past the edge) solves without the domain and clamps.

| Projection | `id` | Parameters | Domain | As PROJ |
| --- | --- | --- | --- | --- |
| Transverse Mercator | `transverse-mercator` | central meridian, scale factor (1) | ±40° of the central meridian | `+proj=tmerc +ellps=WGS84` |
| UTM | `utm` | zone 1–60 (30) | ±40° of the zone's meridian | `+proj=tmerc +k_0=0.9996` |
| Cassini–Soldner | `cassini` | central meridian | ±30° of it | `+proj=cass +ellps=WGS84` |

- **Transverse Mercator** is Krüger's series to n⁶ (Karney 2011), as
  PROJ's default `tmerc`, accurate to nanometres within about 3900 km of
  the central meridian and a millimetre at 40°. UTM is Transverse Mercator
  with k0 = 0.9996 on its zone's meridian; the map shows the false
  easting and northing's origin at the zone's meridian and the equator.
- **Cassini–Soldner** is Snyder's series (eqs. 3-11 to 3-14), as PROJ's
  `cass`; Snyder's inverse series is only the first guess. PROJ's inverse
  stops at that series, so it doesn't quite invert its own forward far
  from the meridian; the tests use the true point there.

**The rest of the program.** Such a map doesn't repeat (`repeats()`
false): `MapFrame::worldWidthM` is 0, `offsetOf()` doesn't wrap and
`copyShiftsM()` gives one copy. The camera holds the centre in the
domain (`clampLonDeg()`), takes the scale at the centre with
`scaleAtPoint()`, and zooms out no further than the whole map. A drag
past the edge stops on it and slides along it. The tile selector
measures each tile by points sampled along the edges of its part in the
domain (a fiftieth larger for the curve), judges the mesh's straight
edges against the projected curve, and needs no detail outside the
domain. Vector pieces are clipped to the domain and markers outside it
are hidden, because lines and dots squashed onto the edge would show.

## The UK National Grid and the OSGB36 datum

`ts::NationalGridProjection` (`nationalgrid.h`, id `uk-national-grid`) is
the Ordnance Survey National Grid, EPSG:27700: Transverse Mercator on the
Airy 1830 ellipsoid (a = 6 377 563.396 m, b = 6 356 256.909 m), true
origin 49° N 2° W, k0 = 0.9996012717, false origin E 400 km, N −100 km.
It is `TransverseMercatorProjection` made on Airy (Krüger's series are
built for any ellipsoid) with a datum change before it, so map positions
are true eastings and northings. It has no settings; it shows ±40° of
2° W and ±85° of latitude (as Mercator; see below).

**The datum.** The map's data are WGS 84; the grid is on OSGB36.
`ts::Osgb36Datum` converts as PROJ does from EPSG:4326 to EPSG:4277:

- **With OSTN15**, the Ordnance Survey's definitive transformation, in
  its NTv2 form (PROJ's `uk_os_OSTN15_NTv2_OSGBtoETRS.tif`): WGS 84 is
  taken as ETRS89 (they differ by under a metre, as PROJ also takes
  them), and the grid's offsets, which are given at OSGB36 points, are
  removed by iteration (`OSGB36 + offset(OSGB36) = ETRS89`), as PROJ's
  inverse `hgridshift`. The OS's 40 test points (TP01–TP40) come out
  within 8 mm, as PROJ's do; PROJ's own result within a micrometre.
- **Without it, or outside it**: the Helmert transform "OSGB36 to WGS 84
  (6)" (EPSG:1314, position vector, rotations as small angles), at zero
  ellipsoidal height, the height dropped: PROJ's choice without the grid,
  within 0.1 mm of PROJ and 5 m of OSTN15 over Great Britain.
- Over the grid's outermost 0.1° (all at sea: the grid runs from 9° W to
  2° E and 49° N to 61° N) the two are blended by a smoothstep, so
  positions don't jump at its edge (Helmert and OSTN15 differ by metres
  there).

`ts::HorizontalShiftGrid` (`datumshift.h`) reads such a grid (PROJ's
GeoTIFF horizontal offsets: arc-seconds, longitude positive east, NTv2's
positive west undone) whole into memory and interpolates it bilinearly.
The grid comes in the `geoid-osgm15` ("UK grids") pack, listed in its
manifest under `datumShifts` (see [packs](packs.md)).
`ProviderRegistry::setInstalledPacks()` hands the installed packs' grids
to `ts::setDatumShiftGrids()`; `projectionWith()` then makes a National
Grid instance with the grid (keyed by it, as instances are by their
parameters), and the main window chooses the projection again whenever
the packs change. `Projection::datumShiftGridId()` says which grid a
projection wants.

**In the shader.** Small tiles need nothing new: the Taylor expansion is
of `project()`, datum change and all. Large tiles use the Transverse
Mercator formula on Airy with a prologue (`glslForwardPrologue()`) that
shifts the vertex's latitude and longitude by the abridged Molodensky
formulas (the Helmert's translation and the change of ellipsoid) in
float, plus `p1.xy`: the exact shift at the tile's reference minus
Molodensky's there (`glslParametersAt()`). So the shift is exact at the
reference, and what the shader leaves out (the Helmert's rotations and
scale, OSTN15's detail) changes by little across a tile. Molodensky
divides by cos φ, and near the poles it drifts from the exact shift
faster than a tile's texels allow; hence the ±85° latitude limit.

**Grid references.** `formatGridReference()` gives "TQ 30080 80729": the
two letters of the 500 km and 100 km squares (A–Z without I, from the
north-west; the false origin is in SV), then the easting and northing
within the square, truncated as the OS does, with 0 to 10 digits.
`parseGridReference()` reads the same, with or without spaces, and gives
the square's south-west corner. `Projection::gridReference()` is what
the status bar shows on the map (empty for projections without a grid of
their own).

## Adding a projection

1. **Write the class.** Derive from `ts::Projection`; from
   `CylindricalProjection` if it fits the Mercator family (then only the
   constants, names and PROJ string are needed); or from
   `NormalCylindricalProjection` for any other normal cylindrical one
   (then f, its inverse and derivative); or from
   `LocalExpansionProjection` for anything else (then `project()`, a guess
   for the inverse, and a float GLSL formula; step 2 is done). Otherwise
   implement `forward()`, `inverse()`, `bounds()`, the latitude limits and
   `scaleAt()`.
2. **Write the GLSL.** `glslSource()` returns one function with the
   signature above and a name of its own (`glslFunction()`). It returns
   the difference from the reference point, computed so that it stays
   precise for small offsets; `shaderReference()` supplies whatever of
   the reference it needs, in up to eight floats. It may call the helpers
   in `projection.cpp` (`tsAtanh`).
3. **List it** in `projections()` in `projection.cpp`. That adds it to the
   View menu, to project files (by `id()`) and to every shader. If it has
   parameters, also make instances of it in `makeProjection()` there.
4. **Test it.** Add it to the rows of `againstProj` and `roundTrip` in
   `tests/projection`; `glslMatchesCpp` and the scale test pick it up from
   the list. Add a golden image in `tests/globe`.

What the rest of the program assumes, and a new projection must respect or
change:

- If `repeats()` (the cylindrical ones) the map repeats in x with period
  `bounds().widthM()` and the renderer draws copies of the world side by
  side; otherwise it ends at the domain (`maximumLonOffsetDeg()` and the
  latitude limits), as above.
- `isCylindrical()` true lets the tile renderer draw a tile as one column
  of quads. Leave it false otherwise and tiles keep their full grids.
- The antimeridian is where tiles end, so nothing is ever stretched across
  the map; a projection with other cuts (an interrupted one) would need
  them made in the meshes.

The roadmap's later projections (conics, azimuthals, pseudocylindricals)
fit `LocalExpansionProjection`; an interrupted one would need its cuts.

## Tests

`tests/projection` (no display needed, except the GLSL case, which skips
without OpenGL 3.3):

- Forward and inverse against PROJ over a global grid, the domain's limits
  and both sides of the antimeridian: within 1 mm, and 10⁻⁸° for the
  inverse.
- Round trips within 1 mm on the ground.
- Published values: the EPSG guidance note's EPSG:3857 example, Miller at
  50° N.
- Domains that don't repeat: limits, clamping, bounds enclosing the
  domain, no inverse outside it.
- Instances with parameters: defaults give the listed instance, others
  are kept, clamped, share the shader index and agree with PROJ.
- Domain and bounds: the limits, square Mercator maps, latitudes beyond
  the domain on the edge, nothing off the map.
- A central meridian other than Greenwich, against PROJ.
- Scale factors against map distance over geodesic distance; Mercator
  conformal, Web Mercator nearly, Miller not.
- The GLSL against the C++ on the GPU (above).
- Many threads sharing the registered projections; benchmarks of
  `forward()` and `inverse()`.
