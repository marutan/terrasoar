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

## The three projections

All three are normal cylindrical projections of one family
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

## In shaders

One mesh serves the globe and every projection: the vertex shader places
each vertex on the map from its longitude and latitude, with the
projection's own GLSL ([morph.md](morph.md) says why, and what the shader
does with the result). Floats can't hold a map coordinate to better than
about a metre, so a projection's GLSL function does not project a
position. It projects a **difference from a reference point**:

```glsl
vec2 tsProjectMercator(vec2 dLonLatRad, vec4 r0, vec4 r1)
```

returns, in metres, `forward(reference + d) − forward(reference)` for an
offset d in longitude and latitude (radians) from a reference point. The
reference point is a tile's centre; what the function needs to know about
it comes from `shaderReference(lat, lon)`, computed in double precision
and passed as two `vec4` uniforms whose meaning is the projection's own.
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
vec2 tsProject(int index, vec2 dLonLatRad, vec4 r0, vec4 r1)
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

## Adding a projection

1. **Write the class.** Derive from `ts::Projection`, or from
   `CylindricalProjection` if it fits the family (then only the
   parameters, names and PROJ string are needed). Implement `forward()`,
   `inverse()`, `bounds()`, the latitude limits and `scaleAt()`.
2. **Write the GLSL.** `glslSource()` returns one function with the
   signature above and a name of its own (`glslFunction()`). It returns
   the difference from the reference point, computed so that it stays
   precise for small offsets; `shaderReference()` supplies whatever of
   the reference it needs, in up to eight floats. It may call the helpers
   in `projection.cpp` (`tsAtanh`).
3. **List it** in `projections()` in `projection.cpp`. That adds it to the
   View menu, to project files (by `id()`) and to every shader.
4. **Test it.** Add it to the rows of `againstProj` and `roundTrip` in
   `tests/projection`; `glslMatchesCpp` and the scale test pick it up from
   the list. Add a golden image in `tests/globe`.

What the rest of the program assumes, and a new projection must respect or
change:

- The map **repeats in x** with period `bounds().widthM()`, and x doesn't
  depend on the latitude's sign conventions beyond that: the renderer
  draws copies of the world side by side. A projection that doesn't
  repeat (anything but cylindrical ones centred on the equator) needs the
  wrap-around in `MapFrame` and the selection to be made optional first.
- `isCylindrical()` true lets the tile renderer draw a tile as one column
  of quads. Leave it false otherwise and tiles keep their full grids.
- The antimeridian is where tiles end, so nothing is ever stretched across
  the map; a projection with other cuts (an interrupted one) would need
  them made in the meshes.

The roadmap's later projections (transverse Mercator, conics, polar
stereographic, pseudocylindricals) all need the first point addressed;
the interface and the shader mechanism already fit them.

## Tests

`tests/projection` (no display needed, except the GLSL case, which skips
without OpenGL 3.3):

- Forward and inverse against PROJ over a global grid, the domain's limits
  and both sides of the antimeridian: within 1 mm, and 10⁻⁸° for the
  inverse.
- Round trips within 1 mm on the ground.
- Published values: the EPSG guidance note's EPSG:3857 example, Miller at
  50° N.
- Domain and bounds: the limits, square Mercator maps, latitudes beyond
  the domain on the edge, nothing off the map.
- A central meridian other than Greenwich, against PROJ.
- Scale factors against map distance over geodesic distance; Mercator
  conformal, Web Mercator nearly, Miller not.
- The GLSL against the C++ on the GPU (above).
- Many threads sharing the registered projections; benchmarks of
  `forward()` and `inverse()`.
