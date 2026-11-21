# Ships from AIS

How ships are shown from AIS: the sentences and their reassembly, the
decoder, the formats read and their times, the vessel tracks and their
staleness, symbols, hulls and course vectors, live receivers and open
feeds, and the program's ship layers. Code: `core/include/ts/ais/`
(`ts/ais`), sharing `ts/tracks` with aircraft (see
[adsb.md](adsb.md#the-track-store)), tested by `tests/ais`; drawing in
`GlobeRenderer`, tested by `tests/globe`; the program in
`app/src/vessellayers.*`, `aisdialogs.*`, `vesselwindows.*` and
`mainwindowships.cpp`, tested by `tests/ui`. For users:
[ships.md](../user/ships.md).

Phase 18 is delivered in parts: 18a (sentences and the decoder), 18b
(readers and tracks), 18c (drawing), 18d (ship layers in the program) and
18e (receivers and open feeds).

## Sentences

`ts::parseAisSentence()` reads one NMEA 0183 line (`ts::AisSentence`):

- an optional **tag block** (`\c:1700000000,s:station*hh\`, NMEA 4.0)
  before it, whose checksum is checked; `c` (Unix seconds, or
  milliseconds when large) gives the time, `s` the source;
- `!AIVDM` or `!AIVDO` (any talker), the fragment count and number, the
  sequential message id, the channel, the armoured payload and its fill
  bits, and the checksum (`ts::nmeaChecksum()`, the exclusive-or of the
  characters between `!` and `*`).

Failures are `ts::AisSentenceError`s (no sentence, bad checksum, bad
fields, bad payload characters), which the readers report with the line.

`ts::AisAssembler` puts messages of several sentences back together,
keyed by the sequential id, channel and fragment count: fragments may
come in any order and interleaved with other messages; a message is
complete when all its fragments have come, and one not completed within
64 later sentences is dropped.

## The decoder

`ts::decodeAis()` reads a payload's six-bit characters into bits
(`ts::aisPayloadBits()`, the fill bits dropped) and decodes, following
ITU-R M.1371-5:

| Type | Message | What is kept |
| --- | --- | --- |
| 1–3 | Class A position | status, rate of turn, speed, accuracy, position, course, heading, second |
| 4 | Base station | its time and position |
| 5 | Static and voyage (two sentences) | IMO, callsign, name, type, dimensions, draught, destination, ETA |
| 9 | Search-and-rescue aircraft | altitude, speed, position, course |
| 18 | Class B position | speed, position, course, heading |
| 19 | Class B extended | the same, with name, type and dimensions |
| 21 | Aid to navigation | its kind, name (with the extension), position, dimensions |
| 24 | Class B static (parts A and B) | name; type, callsign, dimensions |
| 27 | Long-range position | status, position (to a tenth of a minute), speed, course |

"Not available" values are missing (`std::optional` or NaN), not
numbers: longitude 181°, latitude 91°, speed 102.3 knots, course 360°,
heading 511, rate of turn −128, and a time second of 60 or more. The
rate of turn comes from its indicator (ROT_AIS = 4.733 √|ROT|, in degrees
a minute, with its sign); ±127 says turning faster than 5° in 30 s
without a value. Text is six-bit ASCII with `@` and trailing spaces
removed. Other types are counted and left out.

## Formats read

`ts::readAis()` reads a whole recording (gunzipped first if it is
compressed) and calls back with one `ts::VesselReport` a decoded report;
`ts::detectAisFormat()` tells the formats apart from the start of a file.

- **NMEA** (`ts::NmeaAisParser`): the time of a sentence comes from its
  tag block, or a time before the sentence or after it on the line (Unix
  seconds, or ISO 8601 with a space or `T`). With none, sentences are a
  second apart from `AisReadOptions::startTimeS`
  (`AisReadResult::untimed` says so). The position report's own second is
  not used: it says nothing about the minute.
- **CSV**: columns given by `ts::AisCsvColumns`, which
  `ts::guessAisCsvColumns()` guesses from a header (NOAA's
  `MMSI,BaseDateTime,LAT,LON,SOG,COG,Heading,VesselName,…,VesselType,
  Status,Length,Width,…` and common other names). Times are read by hand
  when they are plain `YYYY-MM-DD HH:MM:SS` (a `QDateTime` for each row
  costs twice the reading). NOAA's heading 511 is missing.

Bad lines are counted, the first few kept with their line numbers
(`AisReadProblem`).

## Vessel tracks

`ts::VesselTrackBuilder` takes reports in any order and makes one
`ts::VesselTrack` a station (MMSI): a `MovingTrack` of positions (at sea
level; search-and-rescue aircraft at their altitude), the station's
static data over time (`VesselInfo`, a new entry only when something
changes, so `infoAt()` gives what was known at the clock's time) and its
motion (heading and rate of turn, which `TrackSample` doesn't hold).

A station is no longer drawn after `ts::vesselStaleS()` without a
report: ten of its nominal reporting intervals (M.1371's table: Class A
every 2 s above 23 knots, 6 s above 14, 10 s otherwise, 3 minutes at
anchor, moored or below 3 knots; Class B every 30 s, or 3 minutes below
2 knots), and at least the layer's minimum (6 minutes by default). Aids
to navigation and base stations stay an hour.

## Drawing ships

Ships are `GlobeMarker`s (`ts::vesselMarker()`) with shapes the fragment
shader draws: `Ship` (a pointed hull shape) when under way, `ShipStopped`
(round) when moored, at anchor or aground, or when its direction isn't
known, `Diamond` for aids
to navigation, `Square` for base stations, `Aircraft` for SAR aircraft.
The direction (`ts::vesselDirectionDeg()`) is the true heading, or the
course over the ground when there is no heading. Colours (`ts::shipTypeColour()`) follow the
usual AIS scheme by the ship type's group (`ts::shipTypeGroup()`).

**Hulls**: each marker carries its hull (length, beam, and where the
hull's middle is from the antenna: ahead and to starboard, from the
reported distances to bow, stern, port and starboard; only for ships
with a direction). When half the
hull is longer on the screen than the marker's radius, the vertex shader
grows the quad to fit the hull in metres and the fragment shader draws
the hull outline to scale (a pointed bow and a square stern), so close
in the symbol becomes the ship.

The quads fit their shapes (narrow for ships): with thousands of
stations, software GL is bound by the pixels shaded. The fragment shader
returns at once outside a shape and its rim, and draws without sampling
inside it; only edges take four samples.

**Course vectors** (`ts::courseVector()`) are space lines along the
great circle from the ship's position for its speed and course over the
chosen minutes. Labels are billboards after the aircraft's;
`GlobeView::markerAt()` counts the satellites' markers, then the
aircraft's, then the ships'. Trails are `ts/tracks` trails in space line
group 3, course vectors in group 4.

## Live receivers and feeds

`ts::AisFeed` reads one live source on the GUI thread:

- **TCP**: NMEA lines from a host and port, reassembled and decoded as
  they come and timed when they arrive. When the connection drops or
  can't be made it waits 1 s, then twice as long each time up to 30 s.
- **UDP**: datagrams of NMEA lines on a port.
- **Digitraffic** (`https://meri.digitraffic.fi`, CC BY 4.0):
  `/api/ais/v1/locations?from=<ms>` every 30 s for the positions since
  the last poll (GeoJSON; speed 102.3, course 360, heading 511 and rate
  −128 not available) and `/api/ais/v1/vessels` first and then every tenth fetch (5 minutes) for the
  names and voyage data (`ts::readDigitrafficLocations()`,
  `ts::readDigitrafficVessels()`), with the `Digitraffic-User` header the
  service asks for. Failures back off to at most 5 minutes.
- **Kystverket**'s open stream (`153.44.253.27:5631`, NLOD) is NMEA over
  TCP. It didn't answer from the development machine, so it is tested
  only against a mock server.
- **Recording** (receivers): the sentences as they came, each with a
  tag block of the time it arrived, read back as a recording.

## Ship layers

A ship layer is a vector layer whose source has kind `vessels`
(`ts::VesselSource`: a recording with its format and reading options, a
receiver, or a feed from `ts::aisFeedCatalogue()`), drawn by the program
(the vector renderer skips it; the provider registry credits feeds while
they are shown). Its style (`ts::VesselStyle`) holds the colour or
colours by type, size, labels, course vector minutes, the trail and its
colouring, the stale minimum and gap times, the filters (name or MMSI,
type groups, statuses, speed band, area), the stations hidden by hand and
their own trails.

`VesselLayers` (in the program) works like `AircraftLayers`: recordings
read on a thread pool by source and handed to the GUI thread under a
lock through a queued call by name; live sources kept as a feed and a
builder each, their changed tracks rebuilt once a second; on each tick
of the clock the shown stations placed with their labels and course
vectors; the clock ticks while any ship layer is shown. A recording,
when first read, moves the clock to its start if the clock is outside
it, and the time slider spans every recording shown.

## Performance

On the development machine (tests `tests/ais` `dayOfNoaa`,
`csvReadingRate`; `tests/globe` `shipsHoldFrameRate`, under Mesa
llvmpipe at 320×240):

- A day of NOAA-like data, 2000 vessels reporting every minute (2.9
  million reports), builds into tracks in 1.0 s and 200 MB; their trails
  (all the data, 151 000 points after simplifying) in 0.4 s; placing every
  vessel at a time takes 0.5 ms.
- NOAA's CSV reads at about 100 000 rows a second.
- 10 000 stations, 1500 with an hour of trail, playing: frames in 48 ms on
  the globe and 54 ms on the map against the 100 ms software budget, no
  trail uploaded as the clock moves. 20 000 stations take about 100 ms
  in software GL, almost all of it shading the markers' pixels; on a GPU
  they cost nothing much.
