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

"""Clips one tile of a VMAP0 library into a small VPF database for tests.

Usage: make_vmap0_fixture.py <VMAPLV0 directory> <library> <tile> <output>

For example, with the SOAMAFR CD-ROM unpacked:

    make_vmap0_fixture.py ~/vmap0/VMAPLV0 soamafr L/H tests/data/vmap0

writes <output>/VMAPLV0 with the database header, a library attribute
table naming only <library>, the library's coverage attribute, header and
tile reference tables, and every thematic coverage with only the features
of <tile> and that tile's primitives. Spatial indexes, bounding
rectangles and the coverage-wide thematic indexes are left out (the reader
doesn't use them, and the thematic indexes index rows that are gone); the
variable-length indexes are written again.

The parts of VMAP0 that are ESRI's (README1.TXT's ESRI Data License
Agreement) are left out, so the fixture is public domain: in the
Boundaries coverage only coastlines, depth contours and political
boundary lines other than first-order administrative boundaries (USE 26)
are kept, with only their edges and nodes (renumbered); its area, point
and face tables are dropped. The Reference library is not copied.

Feature tables keep their rows' original ids, which are no longer 1, 2,
3...; the reader looks rows up by id, so this is still readable. Run it
with python3 -I, as for every script reading downloaded data.
"""

import os
import shutil
import struct
import sys

THEMATIC = (".tti", ".ati", ".lti", ".pti", ".nti", ".cti")
# Spatial indexes and bounding rectangles: the reader doesn't use them.
SPATIAL = ("csi", "esi", "fsi", "nsi", "tsi", "ebr", "fbr")
FEATURE_TABLES = (".aft", ".lft", ".pft", ".tft", ".cft")
BND_KEPT = {"coastl", "depthl", "polbndl", "bndtxt"}


def find(directory, name):
    for entry in os.listdir(directory):
        if entry.lower() == name.lower():
            return os.path.join(directory, entry)
    return None


class Table:
    """A VPF table: header text, columns and rows (lists of values)."""

    def __init__(self, path):
        data = open(path, "rb").read()
        self.big = data[4:5] in (b"M", b"m")
        order = ">" if self.big else "<"
        self.order = order
        length = struct.unpack(order + "i", data[:4])[0]
        self.header = data[4:4 + length]
        text = self.header.decode("latin-1").strip()
        if text[1:2] == ";":
            text = text[2:]
        parts = text.split(";", 2)
        self.columns = []
        for definition in parts[2].rstrip(";").split(":"):
            if not definition.strip():
                continue
            name, rest = definition.split("=", 1)
            fields = rest.split(",")
            count = -1 if fields[1].strip() == "*" else int(fields[1])
            self.columns.append((name.strip().lower(), fields[0].strip(),
                                 count))
        self.rows = []
        pos = 4 + length
        while pos < len(data):
            row = []
            for _, kind, count in self.columns:
                value, pos = self._read(data, pos, kind, count)
                row.append(value)
            self.rows.append(row)

    def _read(self, data, pos, kind, count):
        o = self.order
        if count < 0:
            count = struct.unpack(o + "i", data[pos:pos + 4])[0]
            pos += 4
        if kind in "TLNM":
            return data[pos:pos + count], pos + count
        if kind == "D":
            return data[pos:pos + 20 * count], pos + 20 * count
        if kind == "X":
            return None, pos
        if kind == "K":
            kind_byte = data[pos]
            pos += 1
            ids = []
            for shift in (6, 4, 2):
                size = (kind_byte >> shift) & 3
                if size == 0:
                    ids.append(None)
                    continue
                width = {1: 1, 2: 2, 3: 4}[size]
                fmt = {1: "B", 2: "H", 3: "i"}[size]
                ids.append(struct.unpack(o + fmt, data[pos:pos + width])[0])
                pos += width
            return tuple(ids), pos
        size, fmt = {"S": (2, "h"), "I": (4, "i"), "F": (4, "f"),
                     "R": (8, "d"), "C": (8, "ff"), "B": (16, "dd"),
                     "Z": (12, "fff"), "Y": (24, "ddd")}[kind]
        values = [struct.unpack(o + fmt, data[pos + i * size:
                                              pos + (i + 1) * size])
                  for i in range(count)]
        pos += size * count
        if kind in "SIFR" and count == 1:
            return values[0][0], pos
        return values, pos

    def column(self, name):
        for index, (column, _, _) in enumerate(self.columns):
            if column == name:
                return index
        return -1

    def variable(self):
        return any(count < 0 or kind == "K" for _, kind, count in self.columns)

    def encode_row(self, row):
        o = self.order
        out = b""
        for value, (_, kind, count) in zip(row, self.columns):
            if kind in "TLNMD":
                if count < 0:
                    out += struct.pack(o + "i", len(value))
                out += value
            elif kind == "X":
                pass
            elif kind == "K":
                kind_byte = 0
                body = b""
                for shift, part in zip((6, 4, 2), value):
                    if part is None:
                        continue
                    if part < 256:
                        kind_byte |= 1 << shift
                        body += struct.pack(o + "B", part)
                    elif part < 65536:
                        kind_byte |= 2 << shift
                        body += struct.pack(o + "H", part)
                    else:
                        kind_byte |= 3 << shift
                        body += struct.pack(o + "i", part)
                out += bytes([kind_byte]) + body
            else:
                fmt = {"S": "h", "I": "i", "F": "f", "R": "d", "C": "ff",
                       "B": "dd", "Z": "fff", "Y": "ddd"}[kind]
                values = value if isinstance(value, list) else [(value,)]
                if count < 0:
                    out += struct.pack(o + "i", len(values))
                for item in values:
                    out += struct.pack(o + fmt, *item)
        return out

    def write(self, path, rows=None):
        rows = self.rows if rows is None else rows
        o = self.order
        offsets = []
        body = b""
        start = 4 + len(self.header)
        for row in rows:
            encoded = self.encode_row(row)
            offsets.append((start + len(body), len(encoded)))
            body += encoded
        with open(path, "wb") as out:
            out.write(struct.pack(o + "i", len(self.header)) + self.header +
                      body)
        if self.variable():
            # The index's name ends in X instead (EDG → EDX, .AFT → .AFX).
            with open(path[:-1] + "X", "wb") as out:
                out.write(struct.pack(o + "ii", len(offsets),
                                      len(self.header)))
                for offset, size in offsets:
                    out.write(struct.pack(o + "ii", offset, size))


def copy_table(source, target, keep=None):
    table = Table(source)
    rows = table.rows if keep is None else [r for r in table.rows if keep(
        table, r)]
    table.write(target, rows)
    return table, rows


def text_value(value):
    return value.decode("latin-1").strip() if isinstance(value, bytes) else \
        value


def tile_id_of(library, tile):
    table = Table(find(find(library, "tileref"), "tileref.aft"))
    name = tile.replace("/", "\\").upper()
    for row in table.rows:
        if text_value(row[table.column("tile_name")]).upper() == name:
            return row[table.column("id")]
    raise SystemExit(f"no tile {tile}")


def boundaries(coverage, tile_dir, out_coverage, out_tile, tile_id):
    """The Boundaries coverage without ESRI's features (see the docstring)."""
    kept_rows = {}
    for name in ("coastl.lft", "depthl.lft", "polbndl.lft", "bndtxt.tft"):
        path = find(coverage, name)
        if path is None:
            continue
        table = Table(path)
        tile_column = table.column("tile_id")
        use = table.column("use")
        rows = [r for r in table.rows if r[tile_column] == tile_id and
                (use < 0 or r[use] != 26)]
        kept_rows[name] = (table, rows)
    # The edges those lines use, renumbered; the nodes at their ends.
    edges = Table(find(tile_dir, "edg"))
    nodes = Table(find(tile_dir, "cnd"))
    edge_ids = {}
    for name in ("coastl.lft", "depthl.lft", "polbndl.lft"):
        if name not in kept_rows:
            continue
        table, rows = kept_rows[name]
        key = table.column("edg_id")
        for row in rows:
            edge_ids.setdefault(row[key], len(edge_ids) + 1)
    node_ids = {}
    new_edges = []
    by_id = {row[0]: row for row in edges.rows}
    start = edges.column("start_node")
    end = edges.column("end_node")
    for old, new in sorted(edge_ids.items(), key=lambda item: item[1]):
        row = list(by_id[old])
        for column in (start, end):
            row[column] = node_ids.setdefault(row[column], len(node_ids) + 1)
        row[0] = new
        for name in ("coastl.lft_id", "depthl.lft_id", "polbndl.lft_id"):
            column = edges.column(name)
            if column >= 0:
                row[column] = -2147483647
        for name in ("right_face", "left_face"):
            row[edges.column(name)] = (1, None, None)
        for name in ("right_edge", "left_edge"):
            row[edges.column(name)] = (new, None, None)
        new_edges.append(row)
    # Each edge names its line feature again.
    for name, column_name in (("coastl.lft", "coastl.lft_id"),
                              ("depthl.lft", "depthl.lft_id"),
                              ("polbndl.lft", "polbndl.lft_id")):
        if name not in kept_rows:
            continue
        table, rows = kept_rows[name]
        key = table.column("edg_id")
        column = edges.column(column_name)
        for row in rows:
            new_edges[edge_ids[row[key]] - 1][column] = row[0]
            row[key] = edge_ids[row[key]]
    edges.write(os.path.join(out_tile, "EDG"), new_edges)
    node_rows = {row[0]: row for row in nodes.rows}
    new_nodes = []
    for old, new in sorted(node_ids.items(), key=lambda item: item[1]):
        row = list(node_rows[old])
        row[0] = new
        first = next(e[0] for e in new_edges
                     if e[start] == new or e[end] == new)
        row[nodes.column("first_edge")] = (first, None, None)
        new_nodes.append(row)
    nodes.write(os.path.join(out_tile, "CND"), new_nodes)
    for name in ("txt",):
        path = find(tile_dir, name)
        if path:
            copy_table(path, os.path.join(out_tile, name.upper()))
    for name, (table, rows) in kept_rows.items():
        table.write(os.path.join(out_coverage, name.upper()), rows)
    # The schema and value tables without the dropped classes.
    copy_table(find(coverage, "fcs"), os.path.join(out_coverage, "FCS"),
               lambda t, r: text_value(r[t.column("feature_class")])
               in BND_KEPT)
    for name in ("char.vdt", "int.vdt"):
        copy_table(find(coverage, name),
                   os.path.join(out_coverage, name.upper()),
                   lambda t, r: text_value(r[t.column("table")]).split(".")[0]
                   in BND_KEPT)


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    database, library_name, tile, output = argv
    out_database = os.path.join(output, "VMAPLV0")
    if os.path.exists(out_database):
        shutil.rmtree(out_database)
    os.makedirs(out_database)
    shutil.copyfile(find(database, "dht"), os.path.join(out_database, "DHT"))
    copy_table(find(database, "lat"), os.path.join(out_database, "LAT"),
               lambda t, r: text_value(r[t.column("library_name")]).lower()
               == library_name)
    library = find(database, library_name)
    out_library = os.path.join(out_database, library_name.upper())
    os.makedirs(out_library)
    for name in ("lht", "grt", "dqt", "dqx", "lineage.doc"):
        path = find(library, name)
        if path:
            shutil.copyfile(path, os.path.join(out_library, name.upper()))
    copy_table(find(library, "cat"), os.path.join(out_library, "CAT"),
               lambda t, r: text_value(r[t.column("coverage_name")]).lower()
               != "libref")
    tileref = find(library, "tileref")
    os.makedirs(os.path.join(out_library, "TILEREF"))
    for file in sorted(os.listdir(tileref)):
        if file.lower() not in SPATIAL or file.lower() == "fbr":
            shutil.copyfile(os.path.join(tileref, file),
                            os.path.join(out_library, "TILEREF", file.upper()))
    tile_id = tile_id_of(library, tile)
    parts = tile.upper().split("/")
    for entry in sorted(os.listdir(library)):
        coverage = os.path.join(library, entry)
        name = entry.lower()
        if not os.path.isdir(coverage) or name in ("tileref", "libref"):
            continue
        out_coverage = os.path.join(out_library, entry.upper())
        os.makedirs(out_coverage)
        tile_dir = coverage
        for part in parts:
            tile_dir = find(tile_dir, part) if tile_dir else None
        out_tile = os.path.join(out_coverage, *parts)
        os.makedirs(out_tile)
        if name == "bnd":
            boundaries(coverage, tile_dir, out_coverage, out_tile, tile_id)
            continue
        for file in sorted(os.listdir(coverage)):
            path = os.path.join(coverage, file)
            lower = file.lower()
            if os.path.isdir(path) or lower.endswith(THEMATIC):
                continue
            if lower.endswith(FEATURE_TABLES):
                copy_table(path, os.path.join(out_coverage, file.upper()),
                           lambda t, r: r[t.column("tile_id")] == tile_id
                           if t.column("tile_id") >= 0 else True)
            elif lower in ("fcs", "fca", "char.vdt", "int.vdt"):
                shutil.copyfile(path, os.path.join(out_coverage, file.upper()))
        if tile_dir:
            for file in sorted(os.listdir(tile_dir)):
                if file.lower() in SPATIAL:
                    continue
                shutil.copyfile(os.path.join(tile_dir, file),
                            os.path.join(out_tile, file.upper()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
