# ADS-B test data

Used by `tests/adsb`, and by `tests/globe` and `tests/ui` for aircraft.
None of it is distributed with the program.

## Made-up flights

Every recording here is written by `make_fixtures.py` (in this directory,
GPL-2.0-or-later like the rest of Terra Soar) from three made-up flights,
so no real aircraft's data is in the repository:

| Address | Callsign | Flight |
| --- | --- | --- |
| ABC001 | TSO101 | over England eastwards, climbing from 30 000 to 35 000 ft |
| ABC002 | TSO202 | over Fiji eastwards across the antimeridian at FL370 |
| ABC003 | TSO303 | over France southwards, descending, with a gap in reception from 100 s to 180 s |

They start at 2026-11-20T12:00:00Z and last 300 s.

| File | Contents |
| --- | --- |
| `truth.csv` | Every second of every flight: the positions the others are checked against |
| `flights-timed.txt` | Raw DF 17 messages (identification, airborne position, velocity) as `time,hex`, encoded by the script's own Mode S encoder |
| `flights-mlat.avr` | The same as AVR with a 12 MHz counter (`@…;`) |
| `flights.beast` | The same as Beast binary (with 0x1a escapes) |
| `flights.sbs` | SBS-1 lines (MSG 1, 3, 4), times in UTC |
| `flights-aircraft.jsonl` | readsb `aircraft.json` snapshots every 5 s, one a line, with a non-ICAO aircraft (skipped) |
| `trace_full_abc001.json` | A tar1090 trace of the first flight |
| `trace_full_abc001-gzip.json` | The same, gzip-compressed as tar1090 serves it |
| `flights.csv` | CSV with its own column names and two bad rows |

To change them, edit the script and run it (`python3 make_fixtures.py`);
the golden images of the aircraft in `tests/data/golden` may then need
making again.

## Published examples

The decoder tests use the example messages of *The 1090 Megahertz Riddle*
(Junzi Sun, 2nd edition, TU Delft OPEN Publishing, 2021,
<https://mode-s.org/1090mhz/>) and compare them with the values printed
there: the identification (KLM1023), the airborne and surface position
pairs and their local decoding, the two velocity subtypes, the DF 4
altitude and DF 5 identity replies, and the parity examples. They are a
few dozen hex digits of recorded broadcasts and their decoded values,
quoted in the test with the source named; the book's text (CC BY-NC-SA
4.0) is not copied.

## A live receiver

`liveFeed` (skipped unless asked for) reads a receiver for 20 seconds:
`TS_ADSB_LIVE=host:30002:avr` or `TS_ADSB_LIVE=host:30005:beast`. Its
data isn't kept.
