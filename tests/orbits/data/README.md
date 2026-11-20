# Orbit test data

Used by `tests/orbits` (and, for the CelesTrak files, the satellite tests of
later parts). None of it is distributed with the program.

| File | Source | Licence | Contents |
| --- | --- | --- | --- |
| `SGP4-VER.TLE` | D. A. Vallado, P. Crawford, R. Hujsak and T. S. Kelso, "Revisiting Spacetrack Report #3", AIAA 2006-6753, companion files (<https://celestrak.org/publications/AIAA/2006-6753/>); this copy from python-sgp4 2.27 (`sgp4/SGP4-VER.TLE`) | Published with the paper for verification; redistributed by python-sgp4 under its MIT licence | The verification TLEs, each with its start, stop and step (minutes) after line 2 |
| `tcppver.out` | As above (`sgp4/tcppver.out`), output of Vallado's C++ reference code | As above | The reference positions (km) and velocities (km/s) in TEME at every listed time, then classical elements and dates |
| `stations.tle` | CelesTrak, `gp.php?GROUP=stations&FORMAT=tle`, downloaded 2026-11-20 | CelesTrak's data may be used freely, with credit to CelesTrak | A whole group file: three-line sets of the space stations and their modules |
| `stations.json` | CelesTrak, `gp.php?GROUP=stations&FORMAT=json`, same day | As above | The same group as OMM JSON |
| `gps-ops.tle` | CelesTrak, `gp.php?GROUP=gps-ops&FORMAT=tle`, same day | As above | The operational GPS satellites (12-hour orbits) |

The verification files are used as they are. Three cases in
`SGP4-VER.TLE` (33333, 33334, 33335) have wrong checksums on purpose, so
the suite reads that file without checking checksums.

The tests compare the first seven columns of each line of `tcppver.out`
(time, position, velocity) within 2×10⁻⁷ km and km/s, as python-sgp4's
own test does; the reference prints them to 10⁻⁸ km and 10⁻⁹ km/s.
