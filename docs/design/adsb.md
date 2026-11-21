# Aircraft from ADS-B

How aircraft are shown from ADS-B: the formats read, the Mode S decoder
and CPR, altitudes and heights, the track store, interpolation and
staleness, trails and how the shader cuts them to the clock, live
receivers, and the program's aircraft layers. Code: `core/include/ts/adsb/`
(`ts/adsb`) and `core/include/ts/tracks/` (`ts/tracks`, shared with ships
later), tested by `tests/adsb`; drawing in `GlobeRenderer`, tested by
`tests/globe`; the program in `app/src/aircraftlayers.*` and
`mainwindowaircraft.cpp`, tested by `tests/ui`. For users:
[aircraft.md](../user/aircraft.md).

Phase 17 is delivered in parts: 17a (the decoder), 17b (readers, the track
store, heights), 17c (drawing), 17d (aircraft layers in the program) and
17e (live receivers). airplanes.live, planned with the phase, is left out
until access to its API is agreed (the roadmap's open question 7).

## Mode S and ADS-B messages

`ts::decodeModeS()` reads one message, 56 or 112 bits
(`ts::ModeSMessage`), following ICAO Annex 10 Vol. IV and DO-260B as
explained in *The 1090 MHz Riddle* (J. Sun, 2nd ed., TU Delft, 2021):

- **Parity** (`ts::modeSParity()`): the CRC of all but the last 24 bits
  (generator 0x1FFF409) exclusive-ored with them. It is 0 for a correct
  DF 17 or 18, at most 79 for DF 11 (the interrogator's code). For DF 0,
  4, 5, 16, 20 and 21 the parity *is* the address, so those messages can't
  be checked on their own (`addressFromParity`).
- **DF 17/18** (extended squitter; DF 18 only with control field 0, an
  ICAO address), by type code:
  - 1–4 identification: the callsign (6-bit characters) and the emitter
    category ("A3");
  - 5–8 surface position: movement (the non-linear speed table), ground
    track, CPR;
  - 9–18 airborne position with barometric altitude (12 bits, the Q bit:
    25 ft steps; else Gillham), CPR;
  - 19 velocity: subtypes 1–2 ground speed and track (east and north
    components; ×4 supersonic), 3–4 airspeed and heading; the vertical
    rate (64 ft/min, GNSS or barometric) and the GNSS altitude less the
    barometric;
  - 20–22 airborne position with GNSS height (metres above the ellipsoid);
  - 28 subtype 1: emergency status and the squawk.
- **DF 4/20, 0/16**: the 13-bit altitude code (M bit: metres; Q bit:
  25 ft; else Gillham's Gray code, 100 ft, as `decodeAltitudeCode()`);
  **DF 5/21**: the identity (squawk, `decodeIdentity()`). Comm-B's own data
  is not read.
- **DF 11**: the address, and on the ground or not (the capability).

Corrupt messages are not corrected (one-bit correction can invent
aircraft); they are dropped and counted.

### Compact Position Reporting

A position is 17 bits of latitude and longitude within a zone, in even and
odd frames whose zones differ in size (60 and 59 latitude zones from pole
to pole; NL longitude zones, 59 at the equator down to 1 beyond 87°)
(`ts/adsb/cpr.h`):

- `cprLongitudeZones()` gives NL from DO-260B's transition latitudes,
  computed from the formula once (not tabled), compared with `<` as the
  standard does.
- `cprGlobalAirborne()` decodes an even and an odd frame: the latitude
  zone index from both, each frame's latitude, refused if the two lie in
  different longitude zones (the aircraft crossed a boundary between
  them) or outside ±90°; then the longitude for the newer frame.
- `cprGlobalSurface()`: the same in a quarter of the space, so four
  solutions in longitude and two in latitude; the nearest to a reference
  (the receiver, or where the aircraft last was) is taken.
- `cprLocal()` decodes one frame near a reference position: right within
  half a zone (180 NM airborne, 45 NM on the surface).

`ts::AdsbDecoder` keeps, per aircraft, its last even and odd frames and
its last position:

1. If its last position is less than 5 minutes old, the frame is decoded
   locally from it, and kept if within reach of it (2000 kt for the time
   between, plus 2 NM).
2. Otherwise (or if not within reach: the last position is forgotten), an
   even and an odd frame no more than 10 s apart (25 s on the surface)
   are decoded globally.
3. With a receiver position, airborne positions farther than 450 NM from
   it are refused.

Messages whose address is their parity are used only for aircraft heard
with a checked parity (DF 11, 17, 18) in the last minute.

## Formats read

`ts/adsb/adsbreaders.h` reads recordings into `ts::AircraftReport`s, one
per line, frame or aircraft in a snapshot. `detectAdsbFormat()` tells them
apart from the first few kilobytes:

| Format | Times | Notes |
| --- | --- | --- |
| SBS-1/BaseStation (`MSG,…`) | the line's date and time, local or UTC | MSG types 1–8: callsign, altitude, speed, track, position, vertical rate, squawk, emergency flag, on ground |
| Raw: AVR `*…;` | none: all at the start time (marked) | through the decoder |
| Raw: AVR `@…;` | the 12 MHz counter from a start time | through the decoder |
| Raw: `time,hex` | the Unix time | through the decoder; what Terra Soar records |
| Beast | the 12 MHz counter from a start time (2⁴⁸ wraps handled) | 0x1a escapes; Mode A/C and status frames skipped |
| readsb/dump1090 `aircraft.json` | `now` less `seen_pos` (positions) and `seen` (the rest) | one snapshot, an array, or one a line; `aircraft` or `ac`; milliseconds when `now` is in them; non-ICAO addresses (`~…`) skipped |
| tar1090 trace | `timestamp` plus each point's offset | altitude "ground", flag 8 for geometric altitude, the aircraft object when present, registration and type; gzip-compressed or not |
| CSV | Unix seconds, milliseconds or ISO 8601 | columns chosen (`CsvColumns`), guessed from the header's names |

Lines that can't be read are reported with their line number
(`AdsbReadResult::problems`, the first hundred, and a count), and reading
goes on. Raw messages that fail their parity are noise, counted
(`messagesRefused`), not reported. A gzip-compressed file is uncompressed
through GDAL's `/vsigzip/` (traces are small).

The same parsers serve the live receivers (`SbsParser`, `parseRawLine()`,
`BeastParser` fed in pieces, `readReadsbSnapshot()`).

## Altitudes and heights

Terra Soar's heights are above the WGS 84 ellipsoid.

- **Geometric** altitude (TC 20–22, `alt_geom`, or the barometric
  altitude plus the GNSS-less-barometric difference heard in the last
  minute) is a height above the ellipsoid, used as it is.
- **Barometric** altitude is a pressure altitude: the geopotential height
  of the pressure in the International Standard Atmosphere.
  `pressureAltitudeToHeightM()` turns it into a geometric height above sea
  level, r H / (r − H) with r = 6 356 766 m (11 000 m geopotential is
  11 019 m), and the builder adds the geoid's undulation (the base pack's
  EGM2008, in quarter-degree cells, `AircraftLayers::heightSettings()`).
  Real temperatures and pressures move it by up to a few hundred metres,
  so such heights are marked approximate (`TrackSample::heightApproximate`).
- Aircraft on the ground with no altitude have no height (NaN) and are
  drawn on the ellipsoid.

Exports write heights above sea level where the format means that (KML's
absolute altitudes, GPX elevations).

## The track store

`ts::MovingTrack` (`ts/tracks`) holds one object's reports
(`ts::TrackSample`: time, position, height, track, speed, vertical rate)
in time order: added in any order, then sorted, and reports within a
millisecond of the one before dropped (`finish()`).

`AircraftTrackBuilder` makes `ts::AircraftTrack`s from reports in any
order: each position takes the altitudes, speed, track and vertical rate
heard last before it (within 30 s), the altitudes heard are kept beside
it (`AircraftAltitudes`), and what changes slowly (callsign, squawk,
registration, type, category, emergency, on ground) is kept only when it
changes (`AircraftInfo`). For live data it builds again only the aircraft
with new reports (`buildChanged()`) and forgets those not heard for six
hours.

- **Where an object is** at a time (`MovingTrack::at()`): between two
  reports, interpolated linearly in latitude, longitude (the shorter way
  round), height, track (the shorter way round), speed and vertical rate.
- **Gaps**: reports farther apart than the gap time (30 s by default) are
  not joined: after the last report before a gap the object stays there
  until it is stale, then isn't shown until the next.
- **Stale**: an object with no report for the stale time (60 s) is not
  shown.
- **Pieces** (`pieces()`): the runs of reports without a gap.

## Trails

A trail is off, a time span before the clock's time, or every report up
to it (`TrailSettings`, per layer, with per-aircraft overrides).
`MovingTrack::trail()` works out on the CPU exactly the points a trail
shows at a time (the tests' reference); the GPU does it differently:

- `trailLines()` turns each piece into one timed `SpaceLine` holding all
  its reports (less those within 10 m of the line through their
  neighbours, `simplifyTrack()`), each point with its time and, by
  altitude or vertical rate, its own colour.
- `SpaceLine::timed`: each segment's instance carries its two times
  (floats from a time origin, re-based when the clock moves more than
  10⁵ s from it) and the span shown. The vertex shader drops segments
  wholly outside [time − span, time]; the fragment shader cuts the rest
  where the time along the segment leaves that window, so the trail ends
  exactly at the aircraft (moved the same way, linearly between
  reports).
- The renderer's line time (`GlobeRenderer::setSpaceLineTime()`) is a
  uniform: moving the clock uploads nothing. Trails are built again only
  when the data or the style changes.
- Space lines come in groups (`setSpaceLines(lines, group)`): satellites'
  tracks (0, every tick), aircraft trails (1, rarely), drop lines (2,
  every tick), each uploaded on its own.
- On the flat map lines are placed again only when the view moves, not
  while it is still and the clock plays.

The colours by altitude run from red-orange on the ground through
yellow, green and blue to violet at 15 km (`altitudeStops()`, the legend
in the properties); by vertical rate from blue (descending) through grey
to orange (climbing), full at 10 m/s.

## Drawing aircraft

Aircraft are `GlobeMarker`s with `MarkerShape::Aircraft`: the instanced
marker quad turned on the screen to the heading (the vertex shader
projects a point a little along the heading's direction on the ground,
given per instance; on the map, the direction between the placed point
and one a kilometre on), and the fragment shader draws an aircraft from
above (fuselage, swept wings, tailplane) with four samples a pixel and a
dark rim. Size is in pixels, so aircraft stay visible at any range.
Labels are billboards without icons after the satellites'
(`GlobeView::setAircraft()`), drop lines globe-only space lines.
`GlobeView::markerAt()` counts the satellites' markers first, then the
aircraft's.

## Live receivers

`ts::ReceiverFeed` reads one receiver on the GUI thread:

- **TCP** (SBS 30003, AVR 30002, Beast 30005): reports are timed when
  they arrive, not by the receiver's clock (SBS times are another
  computer's local time; Beast counters are no clock at all). When the
  connection drops or can't be made it waits 1 s, then twice as long each
  time up to 30 s, and connects again (the decoder starts afresh).
- **aircraft.json over HTTP**: fetched every `pollS` (at least 1 s)
  through the `NetworkClient`.
- **Recording**: SBS lines as they came, raw and Beast messages as
  `time,hex` lines, snapshots as JSON lines: all read back as recordings.

`AircraftLayers` keeps a feed and a builder per receiver source, takes
the reports as they come and rebuilds the changed aircraft's tracks once
a second.

## Aircraft layers

An aircraft layer is a vector layer whose source has kind `aircraft`
(`ts::AircraftSource`: a recording with its format and reading options,
or a receiver), drawn by the program (the vector renderer skips it). Its
style (`ts::AircraftStyle`) holds the colour, size, labels, drop lines,
trail and its colouring, the stale and gap times, the filters (callsign,
address, altitude band, area), the aircraft hidden by hand and their own
trails.

`AircraftLayers` (in the program) reads recordings on a thread pool, by
source, so layers with one source share it; results come back to the GUI
thread under a lock and through a queued call by name (so ThreadSanitizer
sees no unsynchronised hand-over). On each tick of the clock it places the
shown aircraft and their labels and drop lines; the clock ticks while any
aircraft layer is shown (`ProgramClock::setTicking(client, …)` now counts
its clients: satellites and aircraft).

A recording, when first read, moves the clock to its start (paused) if
the clock is outside it, and the time slider spans every recording
shown. The altitude and area filters, which change with the clock, hide
markers only; trails follow the other filters.

## Performance

On the development machine (tests `tests/adsb` `dayRecording`,
`readingRate`; `tests/globe` `aircraftHoldFrameRate`, under Mesa
llvmpipe):

- A day's recording of a busy receiver, 120 aircraft over 8 hours every
  2 s (1.7 million reports), builds into tracks in 0.5 s and 97 MB; its
  trails (all the data, simplified to 33 000 points) in 0.5 s; placing
  every aircraft at a time takes 0.01 ms.
- SBS reads at about 220 000 lines a second (its times read by hand, the
  hour's start kept from line to line).
- 1000 aircraft, each with an hour of trail (39 000 segments), playing:
  frames in 24 ms on the globe and 28 ms on the map, against the 100 ms
  software budget, with no trail uploaded as the clock moves.
