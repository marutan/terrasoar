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
# Writes the fixture recordings of tests/adsb: three made-up flights
# (no real aircraft) in every format Terra Soar reads, from the same
# truth, so the tests can check that every format gives the same tracks.
#
#   python3 make_fixtures.py            # writes the files beside it
#
# The flights (truth.csv lists every second):
#   ABC001 TSO101  over England eastwards, climbing 30000 -> 35000 ft
#   ABC002 TSO202  over Fiji eastwards across the antimeridian, FL370
#   ABC003 TSO303  over France southwards, descending, with a gap in
#                  reception from 100 s to 180 s

import json
import math
import os

START = 1795176000  # 2026-11-20T12:00:00Z
DURATION = 300
HERE = os.path.dirname(os.path.abspath(__file__))
EARTH_NM = 6371008.8 / 1852.0


def flights():
    """Yields (address, callsign, [(t, lat, lon, alt_ft, gs_kt, trk, vr)])."""
    specs = [
        (0xABC001, "TSO101", 51.0, -1.0, 90.0, 450.0, 30000.0, 1000.0, None),
        (0xABC002, "TSO202", -16.5, 179.6, 90.0, 480.0, 37000.0, 0.0, None),
        (0xABC003, "TSO303", 48.5, 2.3, 180.0, 300.0, 20000.0, -1500.0,
         (100, 180)),
    ]
    for address, callsign, lat, lon, trk, gs, alt, vr, gap in specs:
        points = []
        for t in range(DURATION + 1):
            if gap and gap[0] < t < gap[1]:
                pass
            else:
                points.append((t, lat, lon, alt, gs, trk, vr))
            # One second on along the great circle.
            d = gs / 3600.0 / EARTH_NM
            la, lo, tr = map(math.radians, (lat, lon, trk))
            la2 = math.asin(math.sin(la) * math.cos(d) +
                            math.cos(la) * math.sin(d) * math.cos(tr))
            lo2 = lo + math.atan2(math.sin(tr) * math.sin(d) * math.cos(la),
                                  math.cos(d) - math.sin(la) * math.sin(la2))
            back = math.atan2(math.sin(lo - lo2) * math.cos(la),
                              math.cos(la2) * math.sin(la) -
                              math.sin(la2) * math.cos(la) * math.cos(lo - lo2))
            lat = math.degrees(la2)
            lon = (math.degrees(lo2) + 540.0) % 360.0 - 180.0
            trk = (math.degrees(back) + 180.0) % 360.0
            alt += vr / 60.0
        yield address, callsign, points


# --- Mode S encoding (DO-260B), the test's own ----------------------------

def crc(bits_hex):
    data = int(bits_hex, 16)
    n = len(bits_hex) * 4
    poly = 0x1FFF409
    data <<= 24
    for i in range(n + 24 - 1, 23, -1):
        if data & (1 << i):
            data ^= poly << (i - 24)
    return data & 0xFFFFFF


def frame(me):
    """A DF 17 message from its 56-bit ME field, as hex."""
    head = "%02X" % ((17 << 3) | 5)
    return head, me


def df17(address, me):
    body = "%02X%06X%014X" % ((17 << 3) | 5, address, me)
    return body + "%06X" % crc(body)


def nl(lat):
    a = 1 - math.cos(math.pi / 30)
    lat = abs(lat)
    for n in range(59, 1, -1):
        if lat < math.degrees(math.acos(math.sqrt(a / (1 - math.cos(2 * math.pi / n))))):
            return n
    return 1


def cpr(lat, lon, odd):
    i = 1 if odd else 0
    dlat = 360.0 / (60 - i)
    yz = math.floor(131072 * ((lat % dlat) / dlat) + 0.5)
    rlat = dlat * (yz / 131072 + math.floor(lat / dlat))
    dlon = 360.0 / max(nl(rlat) - i, 1)
    xz = math.floor(131072 * ((lon % dlon) / dlon) + 0.5)
    return int(yz) % 131072, int(xz) % 131072


def altitude12(feet):
    n = int(round((feet + 1000) / 25.0))
    return ((n & 0x7F0) << 1) | 0x10 | (n & 0x0F)


def position_me(lat, lon, alt, odd):
    yz, xz = cpr(lat, lon, odd)
    tc = 11
    return (tc << 51) | (altitude12(alt) << 36) | ((1 if odd else 0) << 34) | \
        (yz << 17) | xz


def velocity_me(gs, trk, vr):
    vx = gs * math.sin(math.radians(trk))
    vy = gs * math.cos(math.radians(trk))
    sew, vew = (1 if vx < 0 else 0), int(round(abs(vx))) + 1
    sns, vns = (1 if vy < 0 else 0), int(round(abs(vy))) + 1
    svr = 1 if vr < 0 else 0
    vrv = int(round(abs(vr) / 64.0)) + 1
    me = (19 << 51) | (1 << 48)
    me |= (sew << 42) | (vew << 32) | (sns << 31) | (vns << 21)
    me |= (1 << 20) | (svr << 19) | (vrv << 10)  # barometric rate
    return me


CHARS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######"


def ident_me(callsign):
    me = (4 << 51) | (3 << 48)
    text = callsign.ljust(8)
    for k, c in enumerate(text):
        me |= CHARS.index(c) << (42 - 6 * k)
    return me


def messages():
    """Yields (time, address, hex) in time order, and the SBS fields."""
    out = []
    for address, callsign, points in flights():
        for t, lat, lon, alt, gs, trk, vr in points:
            odd = t % 2 == 1
            out.append((START + t, address,
                        df17(address, position_me(lat, lon, alt, odd))))
            out.append((START + t + 0.3, address,
                        df17(address, velocity_me(gs, trk, vr))))
            if t % 5 == 0:
                out.append((START + t + 0.6, address,
                            df17(address, ident_me(callsign))))
    out.sort()
    return out


def fmt_time(t):
    import datetime
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    return d.strftime("%Y/%m/%d"), d.strftime("%H:%M:%S.") + "%03d" % (
        d.microsecond // 1000)


def write(name, text, binary=False):
    with open(os.path.join(HERE, name), "wb" if binary else "w",
              newline="" if not binary else None) as f:
        f.write(text)


def main():
    all_flights = list(flights())
    # Truth: every second of every flight.
    rows = ["time,icao,lat,lon,alt_ft,gs_kt,track,vr_ftmin,callsign"]
    for address, callsign, points in all_flights:
        for t, lat, lon, alt, gs, trk, vr in points:
            rows.append("%d,%06X,%.7f,%.7f,%.1f,%.1f,%.2f,%.0f,%s" % (
                START + t, address, lat, lon, alt, gs, trk, vr, callsign))
    write("truth.csv", "\n".join(rows) + "\n")

    msgs = messages()
    # Raw, "time,hex".
    write("flights-timed.txt",
          "".join("%.3f,%s\n" % (t, h) for t, _, h in msgs))
    # AVR with the 12 MHz counter ("@").
    base = 0x00001000_0000
    write("flights-mlat.avr", "".join(
        "@%012X%s;\n" % (base + int(round((t - START) * 12e6)), h)
        for t, _, h in msgs))
    # Beast binary.
    beast = bytearray()
    for t, _, h in msgs:
        counter = base + int(round((t - START) * 12e6))
        body = counter.to_bytes(6, "big") + bytes([0x80]) + bytes.fromhex(h)
        body = body.replace(b"\x1a", b"\x1a\x1a")
        beast += b"\x1a3" + body
    write("flights.beast", bytes(beast), binary=True)

    # SBS (UTC times), one MSG,3 per second and MSG,4 / MSG,1.
    lines = []
    for address, callsign, points in all_flights:
        for t, lat, lon, alt, gs, trk, vr in points:
            d, tm = fmt_time(START + t)
            hexid = "%06X" % address
            lines.append((START + t, "MSG,3,1,1,%s,1,%s,%s,%s,%s,,%d,,,%.5f,%.5f,,,0,0,0,0" % (
                hexid, d, tm, d, tm, int(round(alt / 25.0)) * 25, lat, lon)))
            d2, tm2 = fmt_time(START + t + 0.3)
            lines.append((START + t + 0.3, "MSG,4,1,1,%s,1,%s,%s,%s,%s,,,%.0f,%.1f,,,%d,,,,,0" % (
                hexid, d2, tm2, d2, tm2, gs, trk, int(round(vr / 64.0)) * 64)))
            if t % 5 == 0:
                d3, tm3 = fmt_time(START + t + 0.6)
                lines.append((START + t + 0.6, "MSG,1,1,1,%s,1,%s,%s,%s,%s,%s,,,,,,,,0,0,0,0" % (
                    hexid, d3, tm3, d3, tm3, callsign)))
    lines.sort()
    write("flights.sbs", "".join(l + "\r\n" for _, l in lines))

    # readsb aircraft.json snapshots every 5 s (JSON lines).
    snaps = []
    for s in range(0, DURATION + 1, 5):
        ac = []
        for address, callsign, points in all_flights:
            by_t = {p[0]: p for p in points}
            if s not in by_t:
                continue
            t, lat, lon, alt, gs, trk, vr = by_t[s]
            ac.append({"hex": "%06x" % address, "flight": callsign.ljust(8),
                       "alt_baro": int(round(alt / 25.0)) * 25, "gs": round(gs, 1),
                       "track": round(trk, 2), "baro_rate": int(round(vr / 64.0)) * 64,
                       "squawk": "1000", "category": "A3", "lat": round(lat, 6),
                       "lon": round(lon, 6), "seen_pos": 0.0, "seen": 0.0,
                       "r": "G-TSOA", "t": "A320"})
        ac.append({"hex": "~0000aa", "lat": 10.0, "lon": 10.0, "seen": 1.0})
        snaps.append(json.dumps({"now": START + s, "messages": 1000 + s,
                                 "aircraft": ac}, separators=(",", ":")))
    write("flights-aircraft.jsonl", "\n".join(snaps) + "\n")

    # A tar1090 trace of the first flight, a point every 2 s.
    address, callsign, points = all_flights[0]
    trace = []
    for t, lat, lon, alt, gs, trk, vr in points:
        if t % 2:
            continue
        details = {"flight": callsign.ljust(8), "squawk": "1000"} if t % 60 == 0 else None
        trace.append([float(t), round(lat, 6), round(lon, 6),
                      int(round(alt / 25.0)) * 25, round(gs, 1), round(trk, 1),
                      0, int(round(vr / 64.0)) * 64, details, "adsb_icao"])
    text = json.dumps({"icao": "abc001", "r": "G-TSOA", "t": "A320",
                       "timestamp": START, "trace": trace},
                      separators=(",", ":"))
    write("trace_full_abc001.json", text)
    # As tar1090 serves them: gzip-compressed, with a .json name.
    import gzip
    write("trace_full_abc001-gzip.json",
          gzip.compress(text.encode(), mtime=0), binary=True)

    # CSV with its own column names (and two bad rows).
    rows = ["when;hex_ident;latitude;longitude;height;spd;hdg;flight"]
    for address, callsign, points in all_flights:
        for t, lat, lon, alt, gs, trk, vr in points:
            rows.append("%d;%06X;%.6f;%.6f;%.0f;%.1f;%.1f;%s" % (
                START + t, address, lat, lon, alt, gs, trk, callsign))
    rows.insert(5, "not a time;ABC001;51;0;1000;1;1;X")
    rows.insert(9, "1795176005;ABC001;95.0;0;1000;1;1;X")
    write("flights.csv", "\n".join(rows) + "\n")


if __name__ == "__main__":
    main()
