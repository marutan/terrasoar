# Ships

Terra Soar shows ships from AIS, the Automatic Identification System:
the reports ships, aids to navigation and coastal stations broadcast on
VHF. They can come from a recording you have, from your own receiver
(AIS-catcher or rtl-ais with an SDR stick, or a ship's AIS), or from an
open online feed. Each ship is drawn at its position, turned to its
heading and coloured by its type, with its name and a trail if you want
them.

## Importing a recording

**File → Import AIS Data…** opens a recording. Terra Soar reads:

- **NMEA logs**: `!AIVDM` and `!AIVDO` sentences, one a line. Messages
  sent in several sentences are put back together, and sentences with a
  wrong checksum are left out. Times come from the sentences' NMEA 4.0
  tag blocks (`\c:1700000000*hh\`), or from a time at the start or the
  end of each line (Unix seconds or ISO 8601), as most receivers and
  loggers write them.
- **CSV**: NOAA MarineCadastre's AIS files (their columns are
  recognised), and other CSV with columns you choose: time, MMSI,
  latitude and longitude, and if there are any, speed, course, heading,
  name and ship type.

Files may be gzip-compressed. The dialog tells you which format it thinks
the file is. A log whose sentences have no times plays a second a
sentence, from a time you give (by default, when the file was last
written).

You can also drag a recording onto the window. Logs with times and
NOAA's CSV are added at once; the others open the dialog.

Each recording becomes a layer in the Layers panel. When it has been
read, the clock moves to its start, paused, and the time slider spans
it: press **Play** (Ctrl+Space). Lines that can't be read are left out,
and the layer says how many and the first of them. Messages Terra Soar
doesn't show (safety messages, binary data and others) are counted, not
errors.

## Playing it back

Recordings play on the program's clock (see
[Satellites](satellites.md#the-clock)). Between reports, ships move in
straight lines from one to the next. Ships report at different rates —
every few seconds under way, every three minutes at anchor, less often
for small craft — so how long a ship stays drawn without a report
depends on its class and speed: ten of its reporting intervals, and at
least six minutes (you can change this). Aids to navigation and base
stations stay for an hour.

Right-click the layer for **Go to the Start of the Recording** and
**Read Again**.

## Live ships

**File → Add AIS Source…** shows ships now, from:

- **Your receiver**: NMEA sentences over **TCP** from a host and port
  (AIS-catcher, rtl-ais, a ship's AIS on a network), or by listening for
  **UDP** on a port (where AIS-catcher and most receivers send them by
  default). **Record to** keeps the session in a file, with tag-block
  times, which you can import later.
- **An open online feed**, which needs no receiver and no key:
  - **Digitraffic** (Fintraffic): ships around Finland and the northern
    Baltic, CC BY 4.0. Fetched every 30 seconds. Its credit,
    "Fintraffic / digitraffic.fi", is shown while the layer is.
  - **Kystverket** (the Norwegian Coastal Administration): ships along
    Norway's coast, under the NLOD licence. Its stream did not answer
    when this version was tested, so it may not work from everywhere.

  The dialog shows each feed's coverage, licence and credit.

If the connection drops, or a feed can't be reached, Terra Soar tries
again, waiting a little longer each time, and marks the layer until it is
back. **Connect Again** in the layer's menu tries at once. Live ships are
drawn at the clock's time: leave the clock in real time (**Now**).

## What is drawn

- **A ship**: a pointed symbol along its heading (or its course over the
  ground when it doesn't send a heading), coloured by its type. Ships
  moored, at anchor or aground, and ships whose direction isn't known,
  are drawn as a round symbol.
- **Its hull, to scale**, when you are close enough that the hull is
  larger than the symbol: from the dimensions the ship reports, placed by
  where its antenna is on board.
- **Aids to navigation** as diamonds, **base stations** as squares, and
  **search-and-rescue aircraft** as aircraft at their altitude.
- **Its label**: the name (or the MMSI before a name has been heard),
  with its speed and destination if you want them.
- **A course vector**, if you want one: a line to where the ship will be
  in the minutes you choose, at its speed and course.
- **Its trail**, at sea level. A gap in reception (no report for 10
  minutes, or what you choose) breaks it.

The colours of the types:

| Colour | Ships |
| --- | --- |
| Green | Cargo |
| Red | Tankers |
| Blue | Passenger ships and ferries |
| Yellow | High-speed craft |
| Cyan | Tugs, pilots, dredgers, law enforcement and other special craft |
| Orange | Fishing |
| Magenta | Sailing and pleasure craft |
| Grey | Other and not known |
| White | Aids to navigation and base stations |

The legend is in the layer's properties.

## The layer's properties

Right-click the layer and choose **Ship Layer Properties…**:

- **Colour**: by ship type, or one colour for the layer; the **size**;
  **labels** (with speeds, with destinations); **course vectors** of up
  to two hours.
- **Shown at least for**: the shortest time a ship stays drawn after its
  last report.
- **Trails**: **Off**, **a time span before the clock**, or **all the
  data up to the clock**, coloured by **ship type**, **the layer's
  colour** or **speed**, and broken at gaps over a time you choose.
- **Show only**: names or MMSIs containing some text, some ship types,
  some navigational statuses (under way, at anchor, moored, fishing and
  so on), a speed band, and an area (**Use the View** takes the area you
  are looking at).

## Finding a ship

Right-click the layer and choose **Ships…** for the list of its stations,
with their names, MMSIs, types, statuses, speeds and destinations:

- Type in the box to find ships by name, MMSI or destination.
- Untick a ship to hide it; **Show All** and **Hide All**.
- Right-click a ship to give it **its own trail**: off, the last 15, 60
  or 240 minutes, or all the data.
- Double-click a ship for its details.

## Ship details

Click a ship for its details: MMSI, name, callsign, IMO number, type,
length and beam, draught, destination and ETA, navigational status,
speed, course, heading and rate of turn, position, and the times of its
last position and static reports. They follow the clock.

- **Follow** keeps the view centred on it.
- **Export Track…** saves its track as KML (a `gx:Track`), GPX or CSV.

## Privacy

AIS is broadcast openly so ships can see each other. Your own receiver's
data is yours to view; a recording made near your home can show where
you live if you share it.
