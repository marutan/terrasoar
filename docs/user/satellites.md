# Satellites

Terra Soar shows satellites where they are, where they have been and where
they are going, worked out from their two-line element sets (TLEs): the
orbits the US Space Force publishes for every tracked object, which
CelesTrak makes available by group.

## Adding satellites

**Tracks → Add Satellites…** has three tabs:

- **CelesTrak**: one of CelesTrak's groups, such as Space stations,
  100 brightest, Weather, GPS operational, Starlink or Amateur radio.
  Type in the box above the list to find a group. The group is
  downloaded and kept, so it is there offline too. Terra Soar fetches a
  group again after two hours at the soonest, as CelesTrak asks.
- **File**: a file of TLEs (two-line or three-line, as `.tle`, `.3le` or
  `.txt`) or of OMM JSON (CelesTrak's and Space-Track's JSON form). You
  can also drag a `.tle` file onto the window, or give it to Terra Soar
  when starting it (`terrasoar amateur.tle`).
- **Text**: TLEs pasted in. Give the layer a name; with one satellite it
  takes the satellite's name. The text is kept in the project.

Each TLE line's checksum is checked. Lines that can't be read are left
out, and the dialog says why if nothing can be read.

The satellites appear as one layer at the top of the Layers panel. Tick
and untick it, move it, and change its opacity as any layer. The layer
is saved in your project. A CelesTrak layer is downloaded again when the
project is opened (or taken from what was kept, offline); a file layer is
read again from its file.

## What is drawn

For each satellite shown:

- **A marker** at its place and height on the globe, with its name. On
  the flat maps the marker is at the point below it. Satellites behind
  the Earth are hidden; those beside it, such as GPS satellites past the
  horizon, are not.
- **Its past track**: where it has been, as a solid line that fades with
  age.
- **Its future track**: where it is going, as a dashed line.
- On the globe the tracks are drawn in space, at the satellite's height.
  On the flat maps they are **ground tracks**, the line of points below
  it, broken cleanly where the map ends or wraps round.
- If you choose, a **ground track** under the tracks on the globe too,
  and a **footprint**: the area of the Earth that sees the satellite above
  a chosen elevation.

A whole constellation can be shown at once: Starlink's thousands of
satellites move smoothly. With more than 20 satellites shown, only the
selected one draws its tracks unless you choose otherwise, so the globe
stays readable.

## The layer's properties

Right-click the layer and choose **Satellite Layer Properties…**:

- **Colour** and **Marker size**, and whether names are shown.
- **Tracks**: none, the selected satellite's, all shown (or only the
  selected one when more than 20 are shown, the default), or all shown.
- **Past track** and **Future track**: how far back and ahead they reach,
  in orbits or minutes. The default is one orbit back and one forward.
- **Ground tracks under them on the globe.**
- **Footprints**: none, the selected satellite's, or all shown, and the
  elevation they are seen **above** (10° by default; 0° is the horizon).
  Passes are found above the same elevation.
- **Elements old after**: how old elements may be before the satellite is
  marked (14 days by default, see below).

## Choosing satellites

Right-click the layer and choose **Satellites…** for the list of its
satellites, with their catalogue numbers and the age of their elements:

- Tick or untick a satellite to show or hide it.
- Type in the filter to find satellites by name or catalogue number.
  **Show Only These** shows just the satellites the filter leaves:
  type "NOAA" and click it to see only the NOAA satellites.
- **Show All** and **Hide All**.
- **Track Spans…** gives the selected satellites their own past and
  future spans, instead of the layer's.
- Clicking a row selects that satellite on the globe.

## Satellite details

Click a satellite's marker for its details: its name, catalogue number and
international designator, its elements' epoch and age, where it is
(latitude, longitude, altitude), its speed, period and inclination, and
its **next pass** over the point under the pointer: when it rises, how
high it gets and when it sets. Point at a place on the globe to see the
passes there. The details follow the clock as it runs. The selected
satellite's marker is larger, and its name is always shown.

## Old elements

A TLE describes an orbit at one moment, its epoch. Its accuracy falls as
time goes on: a kilometre or so at first, then a few kilometres a day,
more for low satellites. Satellites whose elements are older than the
layer allows (14 days) are drawn in grey and named "(old)", and the
layer is marked in the Layers panel. This is measured from the clock's
time, so it also applies when you look years back or ahead.

To get newer elements, right-click the layer and choose
**Refresh Elements** (or click Refresh Elements in a satellite's
details). A CelesTrak layer is downloaded again at once; a file layer is
read again from its file.

## The clock

The clock in the status bar shows the program's time, in UTC. Satellites
are drawn where they are at that time. It runs in real time at first.
In real time it shows the minute; paused, or at any other speed, it shows
the second too.

- **Play** and **Pause** (Ctrl+Space).
- **Step back** and **Step forward** (Ctrl+, and Ctrl+.) move it by the
  step chosen in the speed menu (a second to a day; a minute at first),
  and pause it.
- The **speed** button (×1, ×60, …) has a menu of speeds, from a day a
  second backwards to a day a second forwards, and **Faster** and
  **Slower** (Ctrl+] and Ctrl+[).
- Click the **time** to jump to a date and time (Ctrl+J).
- **Now** runs the clock in real time again.

The same commands are in **Tools → Clock**. The tracks and markers follow
the clock, so the past and future spans move with it. Layers with times
(see [Layers and projects](layers.md)) follow the same clock. Choosing a
time on their time slider pauses it there.

Projects keep the clock: a project saved in real time opens in real time,
one saved at another time opens there, running if it was running.

## Accuracy

Positions are worked out with SGP4, the model TLEs are made for, and
agree with the reference to a tenth of a millimetre. What limits them is
the TLE itself: from fresh elements a satellite is usually within a
kilometre or two of where it is drawn. Terra Soar leaves out the Earth's
polar motion and the difference between UT1 and UTC, which are much
smaller than that. See the design notes (`docs/design/satellites.md`)
for the details.

Space-Track, which needs an account, will come later.
