# AIS test data

Used by `tests/ais`, and by `tests/globe` and `tests/ui` for ships. None
of it is distributed with the program.

## A made-up harbour

Every recording here is written by `make_fixtures.py` (in this directory,
GPL-2.0-or-later like the rest of Terra Soar, with its own AIS encoder)
from a made-up harbour off Southampton, so no real vessel's data is in
the repository. The stations' MMSIs and names are invented (`TSO …`):

| MMSI | Name | What it does |
| --- | --- | --- |
| 235000001 | TSO CARGO ONE | Class A cargo ship, 12 knots on 045° |
| 235000002 | TSO TUG | Class A tug turning at 20°/min, with a gap in reception from 200 s to 320 s |
| 235000003 | TSO FERRY | Class A passenger ship, moored |
| 235000004 | TSO YACHT | Class B pleasure craft, no heading |
| 235000005 | TSO TANKER | Class A tanker at anchor |
| 992351001 | TSO BUOY 1 | Aid to navigation |
| 2350001 | – | Base station |
| 111235001 | – | Search-and-rescue aircraft at 300 m |

They start at 2026-11-20T12:00:00Z and last 600 s.

| File | Contents |
| --- | --- |
| `truth.csv` | Every station's position each second: what the others are checked against |
| `harbour-tagged.nmea` | `!AIVDM` sentences with NMEA 4.0 tag-block times (types 1, 4, 5, 9, 18, 21, 24) |
| `harbour-prefixed.nmea` | The same with an ISO time before each sentence |
| `harbour-untimed.nmea` | The same without times |
| `harbour-noaa.csv` | The positions in NOAA MarineCadastre's CSV columns |

To change them, edit the script and run it (`python3 make_fixtures.py`);
the harbour golden images in `tests/data/golden` may then need making
again.

## Published sentences

The decoder tests use sentences and their decoded values from the test
suite of pyais (<https://github.com/M0r13n/pyais>, by Leon Morten
Richter), under the MIT licence. Several of its sentences have
wrong checksums in their source; those are checked through
`decodeAis()` on the payload. pyais gives rates of turn rounded to whole
degrees a minute, so they are compared within half a degree.
