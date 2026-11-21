#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Terra Soar - A virtual globe
#
# Copyright (C) 2026 Peter Howkins
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.
#
# Writes the fixture recordings of tests/ais: a made-up harbour (no real
# vessels) in every format Terra Soar reads, from the same truth, with
# the script's own AIS encoder.
#
#   python3 make_fixtures.py            # writes the files beside it

import datetime
import math
import os

START = 1795176000  # 2026-11-20T12:00:00Z
DURATION = 600
HERE = os.path.dirname(os.path.abspath(__file__))
EARTH_M = 6371008.8
KNOT = 1852.0 / 3600.0
LAT0, LON0 = 50.85, -1.30

# mmsi, name, callsign, imo, type, (bow, stern, port, starboard), draught,
# class, status, start (north m, east m), speed kn, course, turn deg/min,
# heading offset (None: no heading), report interval s, gap (from, to)
VESSELS = [
    (235000001, "TSO CARGO ONE", "TSO1", 9000001, 70, (150, 50, 16, 16),
     11.5, "A", 0, (-2000.0, -2000.0), 12.0, 45.0, 0.0, 0.0, 10, None),
    (235000002, "TSO TUG", "TSO2", 9000002, 52, (20, 10, 5, 5), 4.5, "A", 0,
     (500.0, -500.0), 8.0, 0.0, 20.0, 3.0, 10, (200, 320)),
    (235000003, "TSO FERRY", "TSO3", 9000003, 60, (90, 30, 12, 12), 5.0,
     "A", 5, (1200.0, 800.0), 0.0, 90.0, 0.0, 0.0, 180, None),
    (235000004, "TSO YACHT", "TSO4", 0, 36, (8, 4, 2, 2), 0.0, "B", 15,
     (-800.0, 1500.0), 5.0, 300.0, 0.0, None, 30, None),
    (235000005, "TSO TANKER", "TSO5", 9000005, 80, (200, 50, 22, 22), 14.0,
     "A", 1, (-1500.0, 2500.0), 0.0, 200.0, 0.0, 0.0, 180, None),
]
BUOY = (992351001, "TSO BUOY 1", 14, (300.0, 300.0))
BASE = (2350001, (0.0, 0.0))
SAR = (111235001, 300, 120.0, 135.0, (2500.0, -2500.0))


def offset(north, east):
    lat = LAT0 + math.degrees(north / EARTH_M)
    lon = LON0 + math.degrees(east / (EARTH_M * math.cos(math.radians(LAT0))))
    return lat, lon


def state(v, t):
    """Position (north, east), speed, course and heading at time t."""
    (mmsi, name, cs, imo, typ, dims, draught, cls, status, (n, e), speed,
     course, turn, hoff, interval, gap) = v
    n0, e0 = n, e
    c = course
    step = 1.0
    x = 0.0
    while x < t:
        dist = speed * KNOT * step
        n0 += dist * math.cos(math.radians(c))
        e0 += dist * math.sin(math.radians(c))
        c = (c + turn * step / 60.0) % 360.0
        x += step
    heading = None if hoff is None else round((c + hoff) % 360.0)
    return n0, e0, speed, c, heading


# --- AIS encoding (ITU-R M.1371), the test's own -----------------------------

class Bits:
    def __init__(self):
        self.b = ""

    def u(self, value, n):
        self.b += format(value & ((1 << n) - 1), "0%db" % n)

    def s(self, value, n):
        self.u(value if value >= 0 else (1 << n) + value, n)

    def text(self, value, n):
        value = value.upper().ljust(n, "@")[:n]
        for ch in value:
            o = ord(ch)
            self.u(o - 64 if o >= 64 else o, 6)


def armour(bits):
    fill = (6 - len(bits) % 6) % 6
    bits += "0" * fill
    out = ""
    for i in range(0, len(bits), 6):
        v = int(bits[i:i + 6], 2)
        v += 48
        if v > 87:
            v += 8
        out += chr(v)
    return out, fill


def checksum(text):
    x = 0
    for ch in text:
        x ^= ord(ch)
    return "%02X" % x


SEQ = [0]


def sentences(bits, channel="A"):
    payload, fill = armour(bits)
    parts = [payload[i:i + 60] for i in range(0, len(payload), 60)]
    seq = ""
    if len(parts) > 1:
        SEQ[0] = (SEQ[0] + 1) % 10
        seq = str(SEQ[0])
    out = []
    for i, p in enumerate(parts):
        f = fill if i == len(parts) - 1 else 0
        body = "AIVDM,%d,%d,%s,%s,%s,%d" % (len(parts), i + 1, seq, channel,
                                           p, f)
        out.append("!%s*%s" % (body, checksum(body)))
    return out


def position_a(mmsi, status, turn, speed, lat, lon, course, heading, second):
    b = Bits()
    b.u(1, 6); b.u(0, 2); b.u(mmsi, 30); b.u(status, 4)
    rot = 0 if turn == 0 else int(round(math.copysign(4.733 * math.sqrt(abs(turn)), turn)))
    b.s(rot, 8)
    b.u(int(round(speed * 10)), 10); b.u(0, 1)
    b.s(int(round(lon * 600000)), 28); b.s(int(round(lat * 600000)), 27)
    b.u(int(round(course * 10)) % 3600, 12)
    b.u(511 if heading is None else heading, 9)
    # Manoeuvre, spare, RAIM, radio status.
    b.u(second, 6); b.u(0, 2); b.u(0, 3); b.u(0, 1); b.u(0, 19)
    return b.b


def static_a(v):
    (mmsi, name, cs, imo, typ, dims, draught) = v[:7]
    b = Bits()
    b.u(5, 6); b.u(0, 2); b.u(mmsi, 30); b.u(0, 2); b.u(imo, 30)
    b.text(cs, 7); b.text(name, 20); b.u(typ, 8)
    b.u(dims[0], 9); b.u(dims[1], 9); b.u(dims[2], 6); b.u(dims[3], 6)
    b.u(1, 4); b.u(11, 4); b.u(20, 5); b.u(18, 5); b.u(30, 6)
    b.u(int(round(draught * 10)), 8); b.text("SOUTHAMPTON", 20)
    b.u(0, 1); b.u(0, 1)
    return b.b


def position_b(mmsi, speed, lat, lon, course, heading, second):
    b = Bits()
    b.u(18, 6); b.u(0, 2); b.u(mmsi, 30); b.u(0, 8)
    b.u(int(round(speed * 10)), 10); b.u(0, 1)
    b.s(int(round(lon * 600000)), 28); b.s(int(round(lat * 600000)), 27)
    b.u(int(round(course * 10)) % 3600, 12)
    b.u(511 if heading is None else heading, 9)
    b.u(second, 6); b.u(0, 2); b.u(1, 1); b.u(0, 1); b.u(1, 1); b.u(1, 1)
    b.u(1, 1); b.u(0, 1); b.u(0, 1); b.u(0, 20)
    return b.b


def static_b(v, part):
    (mmsi, name, cs, imo, typ, dims) = v[:6]
    b = Bits()
    b.u(24, 6); b.u(0, 2); b.u(mmsi, 30); b.u(part, 2)
    if part == 0:
        b.text(name, 20)
    else:
        b.u(typ, 8); b.text("TSO", 3); b.u(0, 4); b.u(0, 20); b.text(cs, 7)
        b.u(dims[0], 9); b.u(dims[1], 9); b.u(dims[2], 6); b.u(dims[3], 6)
        b.u(0, 6)
    return b.b


def base(mmsi, lat, lon, t):
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    b = Bits()
    b.u(4, 6); b.u(0, 2); b.u(mmsi, 30); b.u(d.year, 14); b.u(d.month, 4)
    b.u(d.day, 5); b.u(d.hour, 5); b.u(d.minute, 6); b.u(d.second, 6)
    b.u(1, 1); b.s(int(round(lon * 600000)), 28)
    b.s(int(round(lat * 600000)), 27); b.u(7, 4); b.u(0, 10); b.u(0, 1)
    b.u(0, 19)
    return b.b


def aid(mmsi, name, aid_type, lat, lon):
    b = Bits()
    b.u(21, 6); b.u(0, 2); b.u(mmsi, 30); b.u(aid_type, 5); b.text(name, 20)
    b.u(1, 1); b.s(int(round(lon * 600000)), 28)
    b.s(int(round(lat * 600000)), 27)
    b.u(1, 9); b.u(1, 9); b.u(1, 6); b.u(1, 6); b.u(7, 4); b.u(60, 6)
    b.u(0, 1); b.u(0, 8); b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1)
    return b.b


def sar(mmsi, alt, speed, lat, lon, course, second):
    b = Bits()
    b.u(9, 6); b.u(0, 2); b.u(mmsi, 30); b.u(alt, 12)
    b.u(int(round(speed)), 10); b.u(0, 1)
    b.s(int(round(lon * 600000)), 28); b.s(int(round(lat * 600000)), 27)
    b.u(int(round(course * 10)), 12); b.u(second, 6); b.u(0, 8); b.u(0, 1)
    b.u(0, 3); b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 19)
    return b.b


def events():
    """(time, mmsi, [sentences], truth row or None), in time order."""
    out = []
    for v in VESSELS:
        mmsi, interval, gap, cls = v[0], v[14], v[15], v[7]
        for t in range(0, DURATION + 1, interval):
            tt = t + (mmsi % 7)  # stagger
            if tt > DURATION or (gap and gap[0] < tt < gap[1]):
                continue
            n, e, speed, course, heading = state(v, tt)
            lat, lon = offset(n, e)
            sec = (START + tt) % 60
            if cls == "A":
                bits = position_a(mmsi, v[8], v[12], speed, lat, lon, course,
                                  heading, sec)
            else:
                bits = position_b(mmsi, speed, lat, lon, course, heading, sec)
            out.append((START + tt, mmsi, sentences(bits),
                        (START + tt, mmsi, lat, lon, speed, course, heading)))
        for t in range(3, DURATION + 1, 360):
            if cls == "A":
                out.append((START + t, mmsi, sentences(static_a(v), "B"), None))
            else:
                out.append((START + t, mmsi, sentences(static_b(v, 0)), None))
                out.append((START + t + 1, mmsi,
                            sentences(static_b(v, 1)), None))
    blat, blon = offset(*BUOY[3])
    for t in range(5, DURATION + 1, 180):
        out.append((START + t, BUOY[0],
                    sentences(aid(BUOY[0], BUOY[1], BUOY[2], blat, blon)),
                    (START + t, BUOY[0], blat, blon, 0.0, 0.0, None)))
    slat, slon = offset(*BASE[1])
    for t in range(1, DURATION + 1, 10):
        out.append((START + t, BASE[0],
                    sentences(base(BASE[0], slat, slon, START + t)), None))
    sv = (SAR[0], "", "", 0, 0, (0, 0, 0, 0), 0, "A", 0, SAR[4], SAR[2],
          SAR[3], 0.0, None, 10, None)
    for t in range(2, DURATION + 1, 10):
        n, e, speed, course, _ = state(sv, t)
        lat, lon = offset(n, e)
        out.append((START + t, SAR[0],
                    sentences(sar(SAR[0], SAR[1], speed, lat, lon, course,
                                  (START + t) % 60)),
                    (START + t, SAR[0], lat, lon, speed, course, None)))
    out.sort(key=lambda x: x[0])
    return out


def tag(t):
    block = "c:%d,s:tso" % t
    return "\\%s*%s\\" % (block, checksum(block))


def iso(t):
    return datetime.datetime.fromtimestamp(
        t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def write(name, lines):
    with open(os.path.join(HERE, name), "w", newline="") as f:
        f.write("".join(l + "\n" for l in lines))


def main():
    ev = events()
    # Multi-sentence messages: the second fragment after the next message
    # (interleaved, as receivers send them).
    tagged, prefixed, untimed = [], [], []
    held = []
    for t, mmsi, sent, truth in ev:
        lines = [(t, s) for s in sent]
        if len(lines) > 1:
            first, rest = lines[0], lines[1:]
            for when, s in held:
                tagged.append(tag(when) + s)
                prefixed.append(iso(when) + " " + s)
                untimed.append(s)
            held = rest
            lines = [first]
        else:
            lines = lines + held
            held = []
        for when, s in lines:
            tagged.append(tag(when) + s)
            prefixed.append(iso(when) + " " + s)
            untimed.append(s)
    write("harbour-tagged.nmea", tagged)
    write("harbour-prefixed.nmea", prefixed)
    write("harbour-untimed.nmea", untimed[:200])

    rows = ["time,mmsi,lat,lon,speed_kn,course,heading"]
    for t, mmsi, sent, truth in ev:
        if truth:
            rows.append("%d,%d,%.7f,%.7f,%.1f,%.2f,%s" % (
                truth[0], truth[1], truth[2], truth[3], truth[4], truth[5],
                "" if truth[6] is None else truth[6]))
    write("truth.csv", rows)

    # NOAA MarineCadastre's columns (vessels only).
    noaa = ["MMSI,BaseDateTime,LAT,LON,SOG,COG,Heading,VesselName,IMO,"
            "CallSign,VesselType,Status,Length,Width,Draft,Cargo,"
            "TransceiverClass"]
    names = {v[0]: v for v in VESSELS}
    for t, mmsi, sent, truth in ev:
        if not truth or mmsi not in names:
            continue
        v = names[mmsi]
        d = v[5]
        noaa.append("%d,%s,%.5f,%.5f,%.1f,%.1f,%s,%s,%s,%s,%d,%s,%d,%d,%s,,%s"
                    % (mmsi, iso(truth[0])[:-1], truth[2], truth[3], truth[4],
                       truth[5], 511 if truth[6] is None else truth[6], v[1],
                       ("IMO%d" % v[3]) if v[3] else "", v[2], v[4],
                       "" if v[8] == 15 else v[8], d[0] + d[1], d[2] + d[3],
                       ("%.1f" % v[6]) if v[6] else "", v[7]))
    write("harbour-noaa.csv", noaa)


if __name__ == "__main__":
    main()
