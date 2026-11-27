# Aircraft

Terra Soar shows aircraft from ADS-B, the position reports aircraft
broadcast on 1090 MHz: from a recording you have, or live from your own
receiver (dump1090 or readsb on a Raspberry Pi with an SDR stick, for
example). Each aircraft is drawn at its position and altitude above the
globe, turned to its heading, with a trail behind it if you want one.

## Importing a recording

**Tracks → Import Aircraft ADS-B Data…** opens a recording. Terra Soar
reads:

- **SBS-1 / BaseStation** lines (`MSG,3,…`), the format dump1090 and readsb
  send on port 30003.
- **Raw Mode S messages** in hex: AVR lines (`*8D4840D6…;`) as on port
  30002, AVR with a receiver's 12 MHz counter (`@…;`), or one message a
  line with a Unix time before it (`1700000000.5,8D4840D6…`), which is how
  Terra Soar records raw messages. Terra Soar decodes them itself: the
  checksum, callsigns, airborne and surface positions, altitudes,
  velocities, squawks and emergencies.
- **Beast binary**, the format of port 30005.
- **aircraft.json** snapshots from dump1090 or readsb, one or many (one a
  line).
- **tar1090/readsb traces** (`trace_full_…json`, compressed or not): one
  aircraft's day.
- **CSV** with columns you choose: time, ICAO address, latitude and
  longitude, and if there are any, altitude, track, speed, callsign and
  vertical rate. Terra Soar guesses the columns from their names
  (OpenSky's state vectors are recognised, with their metres and metres a
  second); check them and change what it got wrong.

The dialog tells you which format it thinks the file is. Some formats
need an answer:

- **Beast**, and AVR with a counter, count time from the receiver's own
  clock, not the date. Give the time of the first message (by default,
  when the file was last written).
- **SBS** times are written in the local time of the computer that ran
  the receiver. Choose UTC if yours were in UTC.
- **Receiver**: where the receiver was. Optional; it lets Terra Soar
  decode positions of aircraft on the ground, and it ignores positions
  that would be impossibly far from it.

You can also drag a recording onto the window. SBS, timed raw messages,
aircraft.json and traces are added at once; the others open the dialog.

Each recording becomes a layer in the Layers panel. When it has been
read, the clock moves to its start, paused, and the time slider spans
it: press **Play** (Ctrl+Space) and the aircraft fly. Lines that can't
be read are left out, and the layer says how many and the first of them.

## Playing it back

Recordings play on the program's clock (see
[Satellites](satellites.md#the-clock)): Play and Pause, the speed button
(×1, ×10, ×100, ×1000 and more), the steps, and the time slider in the
time bar. Between reports, aircraft move in straight lines from one to
the next. An aircraft that has had no report for a minute (you can
change this) is no longer drawn.

Right-click the layer for **Go to the Start of the Recording** and
**Read Again**.

## A live receiver

**Tracks → Connect to Aircraft ADS-B Receiver…** shows what your receiver
hears now:

- **SBS** (port 30003), **raw AVR** (port 30002) or **Beast**
  (port 30005), from the receiver's address, or
- **aircraft.json over HTTP**, fetched every second or as often as you
  choose: the address of the receiver's web page's `data/aircraft.json`.

If the connection drops, or the receiver can't be reached, Terra Soar
tries again, waiting a little longer each time, and marks the layer
until it is back. **Connect Again** in the layer's menu tries at once.

**Record to** keeps the session in a file as it comes, which you can
import later: SBS as it was sent, raw and Beast messages as timed raw
messages, aircraft.json as one snapshot a line.

Live aircraft are drawn at the clock's time: leave the clock in real
time (**Now**) to see them where they are.

## What is drawn

- **An aircraft** at its position and height, nose along its track over
  the ground. On the flat maps it is at the point below it.
- **Its label**: the callsign, or the ICAO address before a callsign has
  been heard, with its altitude and speed if you want them.
- **A line down to the ground**, if you want it, which makes the heights
  readable when you tilt the view.
- **Its trail**, in 3D at the heights reported on the globe, as a ground
  track on the maps. A gap in reception (no report for 30 seconds, or what
  you choose) breaks the trail instead of joining it with a straight line.

## The layer's properties

Right-click the layer and choose **Aircraft Layer Properties…**:

- **Colour** and **size** of the aircraft, **labels** (with altitudes,
  with speeds), and **lines down to the ground**.
- **Gone after no reports for**: when an aircraft stops being drawn.
- **Trails**: **Off**, **a time span before the clock** (the last 10
  minutes by default), or **all the data up to the clock**. Trails move
  with the clock: as it plays, the span moves on, and moving the clock
  back takes the trail back.
- **Coloured by** the layer's colour, **altitude** (the legend shows the
  heights), or **climbing or descending** (orange climbing, grey level,
  blue descending).
- **Broken at gaps over**: how long without a report counts as a gap.
- **Show only**: callsigns containing some text, ICAO addresses starting
  with some digits, an altitude band, and an area (**Use the View** takes
  the area you are looking at). The altitude and area filters hide
  aircraft while they are outside them.

## Finding an aircraft

Right-click the layer and choose **Aircraft…** for the list of its
aircraft, with their callsigns, addresses, registrations and types
(when the data has them), altitudes and speeds:

- Type in the box to find aircraft by callsign, address, registration or
  type.
- Untick an aircraft to hide it; **Show All** and **Hide All**.
- Right-click an aircraft to give it **its own trail**: off, the last 5,
  15 or 60 minutes, or all the data.
- Double-click an aircraft for its details.

## Aircraft details

Click an aircraft for its details: ICAO address, callsign, registration,
type and category (when known), squawk and emergency status, barometric
and geometric altitude, its height above the ellipsoid, ground speed,
track, vertical rate, position and the time of its last report. They
follow the clock.

- **Follow** keeps the view centred on it as it flies.
- **Export Track…** saves its track as KML (a `gx:Track`, which Google
  Earth and Terra Soar play back), GPX or CSV.

## Heights

ADS-B gives two altitudes. The **geometric** altitude (from GPS) is a
height above the WGS 84 ellipsoid, which Terra Soar uses as it is. The
**barometric** altitude, which every aircraft sends, is a pressure
altitude: Terra Soar turns it into a height through the standard
atmosphere and puts it on sea level (the base pack's geoid). That is
exact only on a standard day; real weather moves it by up to a few
hundred metres, so the details say "about". When an aircraft sends both,
the geometric one is used.

## Privacy

ADS-B is broadcast openly, and your own receiver's data is yours to
view. Some operators ask online services not to show their aircraft
(the FAA's LADD and PIA programmes); online services Terra Soar connects
to later will honour such lists.
