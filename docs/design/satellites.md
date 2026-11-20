# Satellites

How satellites are shown from two-line element sets: reading TLEs and
OMMs, the SGP4/SDP4 propagator, the change from SGP4's frame to the
Earth-fixed one, time, tracks and the antimeridian, footprints and passes,
downloading from CelesTrak, and the limits of the accuracy. Code:
`core/include/ts/orbits/` (`ts/orbits`, tested by `tests/orbits`). For
users: [satellites.md](../user/satellites.md).

Phase 15 is delivered in parts: 15a (elements and the propagator), 15b
(tracks, passes, the worker, CelesTrak), 15c (the running clock), 15d
(drawing tracks and markers) and 15e (satellite layers in the program).

## Element sets

`ts::OrbitalElements` holds one satellite's mean elements as a TLE or an
OMM gives them: name, catalogue number, classification, international
designator, epoch, the mean motion's derivatives, B*, inclination, right
ascension of the ascending node, eccentricity, argument of perigee, mean
anomaly, mean motion (revolutions a day), ephemeris type, element set
number and revolution number. Units are as written there (degrees,
revolutions a day); the propagator converts them.

The epoch is a `ts::JulianDate`: a whole Julian day (ending in .5) and the
fraction after it, so microseconds survive. A TLE epoch is a two-digit
year (57–99 are 19xx, 00–56 20xx) and a day of the year with its fraction
(1.0 is 1 January, 0h UTC).

### TLE

`ts::parseTle()` reads one set by its columns. Every fixed column (spaces,
decimal points, the line numbers) is checked, so a line shifted by one
character is refused rather than misread. The checksum in column 69 (the
sum of the digits, and 1 for each '-', modulo 10) is checked unless
`TleOptions::checkChecksums` is off; the reference test set has three
deliberately wrong checksums and is read with it off. Both lines must have
the same catalogue number. The exponent fields (" 12345-3" is
0.12345×10⁻³) are read as Vallado's code reads them: the mantissa, then
times the power of ten.

Catalogue numbers above 99 999 are written in Alpha-5: a letter for the
ten-thousands from 10 (A) to 33 (Z), skipping I and O, then four digits
(`parseCatalogueNumber()`).

`ts::parseTleText()` reads a whole file: three-line sets (a name line,
plain or as "0 NAME", before line 1), two-line sets, blank lines and `#`
comments. What can't be read is listed with its line number, and the rest
is kept.

### OMM

`ts::parseOmm()` reads one Orbit Mean-Elements Message in its JSON form,
as CelesTrak (`FORMAT=json`) and Space-Track give it. Its members may be
numbers or strings (Space-Track writes strings). The epoch is
`YYYY-MM-DDTHH:MM:SS.ffffff`, UTC, with or without a Z.

OMM is the form fetched by default, because TLEs can't hold catalogue
numbers above 99 999 in plain digits and CelesTrak leaves such objects out
of its TLE files: its stations group of 2026-11-20 has 13 TLEs and 17 OMMs.

`ts::formatTle()` writes elements back as TLE lines (with Alpha-5 numbers
and checksums): what is read from a TLE is written back character for
character, except that zero in an exponent field is written "00000+0" (as
CelesTrak does) where some files have "00000-0".

## SGP4 and SDP4

`ts::Sgp4` is Terra Soar's own implementation of the reference: Spacetrack
Report #3 (Hoots and Roehrich, 1980) as revised by Vallado, Crawford,
Hujsak and Kelso, "Revisiting Spacetrack Report #3" (AIAA 2006-6753), with
Vallado's later fixes (sgp4unit.cpp of 2020). It is not a linked library,
so the program stays GPL-2.0-or-later (decided 2026-11-17).

- **Near-Earth (SGP4)**, periods under 225 minutes: secular effects of
  gravity (J2, J4) and atmospheric drag (B*), the long-period J3 terms, and
  the short-period periodics. Perigees under 220 km use the simplified
  drag terms; under 156 km (and 98 km) the atmosphere's parameter s is
  lowered as the reference does.
- **Deep space (SDP4)**, periods of 225 minutes or more: the Sun's and
  Moon's secular and long-period terms (dscom, dpper), and the resonances
  of 12-hour orbits with high eccentricity (GPS-like and Molniya) and of
  24-hour (geosynchronous) orbits, integrated in steps of 720 minutes
  (dsinit, dspace). The Lyddane modification is used below 0.2 rad
  (11.46°) of perturbed inclination, as GSFC does; inclinations that go
  negative are turned back (node + π, argument of perigee − π).
- **Constants**: WGS 72 by default, which TLEs are fitted with;
  `GravityModel::Wgs72Old` (Spacetrack Report #3's rounded xke) and
  `Wgs84` are there for comparison only.
- **Mode**: "improved" by default, with sidereal time from the IAU 1982
  formula; `Sgp4Mode::Afspc` is the US Air Force's operational form.

The code keeps the reference's names and the order of every calculation,
so the two can be read side by side and agree to the last bits. Where the
reference is one long function, it is split into functions over small
structs (the Sun's and Moon's terms, the resonance terms, the secular
elements) to keep each short.

The reference stores the last integration step of the resonance in the
satellite record, to start from next time. `Sgp4::propagate()` is const
and integrates from the epoch each time instead. The steps are the same
720-minute steps from the epoch either way, so the numbers are the same,
and one `Sgp4` can be shared between threads. Propagating a
geosynchronous satellite years from its epoch takes a few thousand steps,
which is still well under a millisecond.

### Errors

Propagation reports the reference's error codes instead of a position
(`ts::Sgp4Error`): 1 the mean eccentricity left [−0.001, 1), 2 the mean
motion fell to zero or below, 3 the perturbed eccentricity left [0, 1]
(deep space), 4 the semi-latus rectum fell below zero, and 6 the orbit
fell inside the Earth (decayed; the position, inside the Earth, is given
too). Elements SGP4 can't start from at all (an eccentricity outside
[0, 1), a mean motion of zero or less) give their error for every time,
and never a position.

### Verification

`tests/orbits` runs every case of the reference test set
(`tests/orbits/data/SGP4-VER.TLE`, Vallado et al. 2006) at every listed
time, as Vallado's `testcpp` does, and compares the position and velocity
with the reference output (`tcppver.out`), as python-sgp4's own test does:
within 2×10⁻⁷ km and km/s (the reference prints 10⁻⁸ km). The largest
differences are 1.2×10⁻⁷ km and 5×10⁻¹⁰ km/s, a tenth of a millimetre.
The cases cover near-Earth and deep-space orbits, both resonances, high
eccentricity, low perigees, inclinations going negative and the error
codes, which come in the reference's order (1, 1, 6, 6, 4, 3, 6). The
three gravity models are checked against Vallado's C++ output for
Vanguard 1.

## From TEME to the Earth

SGP4 gives positions in TEME: the true equator and mean equinox of the
time. `ts::temeToEcef()` turns them about the pole by Greenwich mean
sidereal time (IAU 1982, `ts::greenwichSiderealRad()`, the formula SGP4
itself uses) into the pseudo-Earth-fixed frame, and the velocity less the
Earth's turning (ω × r, ω = 7.292115146706979×10⁻⁵ rad/s).

Left out, on purpose:

- **Polar motion**, which turns the pseudo-Earth-fixed frame into ITRF
  (WGS 84): under 15 m at the surface and a few tens of metres at
  geostationary height, against SGP4's own errors of a kilometre or more.
- **UT1 − UTC**, under 0.9 s: the clock's UTC is used as UT1. That turns
  the Earth by up to 0.4 km at the equator under a satellite's point.

Checked against Vallado et al.'s TEME example (2004-04-06 07:51:28.386009
UTC, UT1 − UTC = −0.4399619 s): the pseudo-Earth-fixed position to 1 cm and
the velocity to 10⁻⁵ m/s (the paper turns the velocity with the day's
measured length). Sidereal time is checked against Vallado's example 3-5
(1992-08-20 12:14 UT1, 152.578787810°).

## Where a satellite is, and its track

`ts::satelliteState()` gives a satellite's Earth-fixed position and
velocity, the point below it (`ecefToGeodetic()`, so the height is above
the WGS 84 ellipsoid) and its speed in space.

A track is where the satellite was over a past span before a time and
where it will be over a future span after it (`ts::SatelliteTrack`).
Spans are given in minutes or in orbits (`ts::TimeSpan`), an orbit being
the anomalistic period from the Brouwer mean motion.

`ts::TrackSampler` samples one satellite's track on a fixed grid of times:
multiples of a step from the epoch, about 1/120 of an orbit (3°, 46 s for
the ISS), from 15 s to 30 minutes (`trackStepMinutes()`). The grid points
inside the spans are kept between calls, so as the clock moves on only the
points that come into the spans are computed, and those that fall out are
dropped. The ends are computed exactly each time: the start of the past
span, the time itself and the end of the future span. So the spans cover
exactly the chosen times, and the track's point at the time is where the
marker is.

### The antimeridian

Ground tracks on the map are split where they cross the antimeridian
(`ts::splitAtAntimeridian()`): where a step jumps by more than 180° of
longitude, the latitude where it crosses ±180° is interpolated, one piece
ends there and the next starts there on the other side. The map's own
edges (where a projection wraps, or a polar projection ends) are handled
when drawing (15d).

### Footprints

A footprint is the area of the Earth that sees a satellite above a minimum
elevation (`ts::footprint()`): a circle around the point below it, of the
Earth central angle

    λ = arccos(R cos ε / (R + h)) − ε

for height h, elevation ε and the Earth's mean radius R (6371.0088 km), a
sphere (the ellipsoid changes it by well under a percent). Its edge is a
ring of points at that angle from the point below, in every direction.

## Passes

`ts::lookAngles()` gives azimuth, elevation (above the plane tangent to
the ellipsoid) and range from a place to a point. `ts::nextPass()` finds
the next time a satellite rises above a minimum elevation over a place,
when it is highest and when it sets: it steps through time (1/100 of an
orbit, from 15 s to 5 minutes), then narrows each crossing by bisection
and the highest point by golden-section search, to about a second. A pass
under way at the time searched from is found too (it searches back for the
rise), and a satellite that never sets within the search, as a
geostationary one seen from below it, is marked as always up.

## Computing off the GUI thread

`ts::OrbitComputer` computes `ts::OrbitRequest`s on its own thread: where
each satellite is at a time, and the tracks of those that show one. Only
the latest request matters; one made while another waits replaces it. It
keeps each satellite's `TrackSampler` between requests (and drops those no
longer asked for), so tracks follow a moving clock cheaply.

## CelesTrak

CelesTrak publishes current elements by group (`gp.php?GROUP=stations`)
and by satellite (`gp.php?CATNR=25544`), as TLE or OMM JSON.
`ts::celestrakGroups()` lists its groups with names for the user, under
CelesTrak's headings; `ts::celestrakGroupUrl()` and
`ts::celestrakSatelliteUrl()` make the addresses (JSON by default).

`ts::downloadElementSets()` fetches element sets through a cache kept for
use offline: one file for the body and one with the time of the download
and its validators (ETag, Last-Modified), named by the address's SHA-1.

- A download younger than two hours is used without asking: CelesTrak
  updates its elements every few hours and asks clients not to fetch a
  group more often than that (refreshing by hand forces it).
- An older one is revalidated (If-None-Match, If-Modified-Since); a 304
  renews it.
- When the server can't be reached, answers with an error, or with
  something that isn't element sets (CelesTrak answers "No GP data found"
  with status 200), the last download is used however old, with the
  reason. A download replaces the cached one only if it has element sets.

`ts::cachedElementSets()` reads the last download without the network, for
a project opened offline. The tests run all of this against a mock server.

Space-Track, which needs an account, comes with Phase 23's key storage.

## The clock

`ts::ProgramClock` (Phase 12b's clock, extended) runs at a rate (seconds of
the clock per second, negative backwards, from −86 400 to 86 400 in the
menu) or is paused. It starts in real time: rate 1, following the system
clock exactly (`isRealTime()`). Otherwise `time()` is the time it was last
anchored (played, paused, set, rate changed) plus the elapsed time (a
monotonic `QElapsedTimer`) times the rate, so it doesn't drift.

Setting a time pauses it there: a time chosen on the WMS time slider, a
step, or a time read from a project stays put; Play runs on from it.
Jumping to a date and time keeps it running if it was.

While running, `timeChanged()` comes every tick (100 ms), but only while
*ticking* is on. `SatelliteLayers` turns ticking on while any satellite is
shown, so a window with no satellites draws no frames however the clock
runs, and stays idle (the UI test of idleness and the smoke test of idle
CPU hold). The clock widget in the status bar reads `time()` a few times a
second while it runs, which repaints only its label.

Projects keep `view.time` as before and, new, `view.clock`
(`{"realTime", "running", "rate"}`): one saved in real time opens in real
time; another opens at its time, at its rate, running if it was. Older
projects open paused at their time, as they did.

## Drawing

Markers are `ts::GlobeMarker`s, which `ts::GlobeRenderer` already drew
instanced (one quad per marker, its position split into high and low
floats relative to the eye). Their vertex shader now leaves clip-space z at
0: the depth is written by the fragment shader anyway, and satellites past
the Earth's edge but farther than the horizon (the camera's far plane)
were clipped. Those behind the Earth get the far depth (1) and lose to it.

Tracks and footprints are `ts::SpaceLine`s: points in ECEF with the point
below each (latitude, longitude), an opacity per point (the past track
fades to 15 % at its oldest end), and a distance along for dashes (the
future track: 60 dashes an orbit, by time, so they are the same length
on every satellite at its own scale). Each segment is one instanced quad,
expanded in screen space to its width and drawn as a capsule (round ends
join segments without gaps), like the vector renderer's outlines, but in
space: tested against the globe's depth (so the Earth hides what is behind
it and not what is past its edge), not written, at its points' depth
brought nearer by a few pixels' worth, as markers are, so a ground track
isn't cut by the ground. z is 0 for the same reason as markers'.

On the flat map (and through the change to it) each point moves to its
map position, as markers do, and is placed again every frame. Segments
are broken where the map wraps (the ends more than half a world apart):
on a repeating map the segment is drawn to the seam on each side
(interpolated at the seam's longitude); on others (polar, interrupted
maps) segments jumping by a quarter of the map, or with an end off the
projection's own domain, are left out. So a track becomes its ground
track. Ground tracks the globe draws below tracks are `globeOnly`, not
drawn twice on the map.

Names are billboards without icons (`GlobeView::setSatelliteLabels()`),
drawn before KML's so they aren't crowded out, thinned like them. High
billboards (over 100 km) are hidden only where the ellipsoid lies between
the eye and them (a ray test), not by the surface normal under them,
which hid satellites past the Earth's edge.

`ts/orbits/satellitedraws.h` turns states and tracks into these
(`pastTrackLine()`, `futureTrackLine()`, `groundTrackLines()`,
`footprintLine()`, `satelliteMarker()`); the program and the golden-image
tests share it.

## Satellite layers

A satellite layer is a vector layer whose source has kind `satellites`
(`ts::SatelliteSource`): a CelesTrak group (with its address, so tests
and mirrors can change it), any address, a file, or text pasted in (kept
in the project). Its style (`ts::SatelliteStyle`) holds the colour, marker
size, names, which satellites draw tracks and footprints, the spans, the
footprints' elevation, the age past which elements are old, which
satellites are shown (all or none, with exceptions) and satellites' own
spans. Changing the style doesn't change the source, so the element sets
aren't loaded again. The vector renderer leaves these layers to the
program; the provider registry credits them (`attribution`).

`SatelliteLayers` (in the program) keeps the visible layers' element sets,
by their source's JSON, so layers with one source share it. Files and
text are read, and downloads made, on a thread pool; a set that is
loading again keeps showing the old one. On each tick it asks the
`OrbitComputer` for the shown satellites' positions (and the tracks of
those that draw them), and turns the answer into markers, labels and
lines. Results come back in order; one that arrives after a newer request
is still drawn, so a slow computer shows every result it finishes.

- **Tracks**: Automatic (the default) draws every shown satellite's
  tracks up to 20 shown, and only the selected one's beyond.
- **Selection**: clicking a marker (the nearest within a few pixels of its
  edge, `GlobeView::markerAt()`) selects it instead of the usual click
  (What Is Here, KML balloons). The selected marker is larger and always
  named, and its details window follows the clock.
- **Passes**: over the ground under the pointer once it has rested
  0.3 s, above the layer's footprint elevation, found again when the pass
  is over, the place or satellite changes, or the clock goes back.
- **Old elements**: older than the layer's limit (14 days) from the
  clock's time: grey markers, "(old)" after the name, a mark on the
  layer in the Layers panel and once a status bar message, and Refresh
  Elements (the layer's menu, the details window), which downloads again
  ignoring the cache's age, or reads the file again.
- **Status**: a source that fails is marked in the Layers panel; one shown
  from the last download, because the server couldn't be used, is marked
  offline with the reason.

## Performance

On the development machine under Mesa llvmpipe (the software renderer the
budgets are tested on): 6048 satellites' positions and ten tracks in 5 ms
(a tick moves each track by a point or none); frames with them all and
the ten tracks in 7–11 ms on the globe and 12–16 ms on the map (placed
again every frame), against the 100 ms software budget
(`tests/globe` `satellitesHoldFrameRate`).

## Accuracy

- SGP4 positions from fresh TLEs are good to about a kilometre near the
  epoch and lose a few kilometres a day (more for low orbits, where drag
  varies with the Sun's activity). Elements older than 14 days are marked
  in the program as old.
- The frame conversion's simplifications (above) are well inside that.
- Track points are exact SGP4 positions; the lines between them are
  straight in space, which cuts inside the orbit by r(1 − cos 1.5°),
  0.034 % of its radius, at 3° steps (2.3 km for the ISS at the middle of
  a step).
