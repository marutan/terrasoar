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

"""Writes the configurations of the Phase 26 packs, tools/packager/packs/*.json.

Usage: make_extra_configs.py [<packs directory>]

WDBII borders and rivers, the Natural Earth vector themes and raster maps,
Black Marble, OurAirports, place names, ETOPO 2022 at 30″ and GEBCO 2025
at 15″ (regional). Several of them repeat their classes for many files or
levels, so they are written from this description rather than edited by
hand: change it here and run it again. See docs/tools/packager.md.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MIRROR = "https://github.com/marutan/terrasoar-packs-sources/releases/download"

GSHHG = {
    "id": "gshhg",
    "file": "gshhg-shp-2.3.7.zip",
    "url": "https://github.com/GenericMappingTools/gshhg-gmt/releases/download/2.3.7/gshhg-shp-2.3.7.zip",
    "mirror": MIRROR + "/gshhg-2.3.7/gshhg-shp-2.3.7.zip",
    "sha256": "8dbbe7e071e77e9e75f2d639239099ebca8d5c16d6a07df8169729d49f15cf41",
    "name": "GSHHG (Global Self-consistent, Hierarchical, High-resolution Geography Database), shapefiles",
    "version": "2.3.7",
    "notices": ["LICENSE.TXT", "COPYING.LESSERv3"],
    "localNotices": ["../notices/gpl-3.0.txt"],
}


def write(cfg, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n")


def wdbii(out):
    """WDBII borders and rivers, from the GSHHG zip: a file per resolution
    and level, each resolution one level of detail."""
    resolutions = ["c", "l", "i", "h", "f"]
    lods = [
        {"storageLevel": 0, "minLevel": 0, "maxLevel": 1},
        {"storageLevel": 2, "minLevel": 2, "maxLevel": 3},
        {"storageLevel": 4, "minLevel": 4, "maxLevel": 5},
        {"storageLevel": 6, "minLevel": 6, "maxLevel": 7},
        {"storageLevel": 8, "minLevel": 8, "maxLevel": -1},
    ]
    borders = [(1, "national", 0), (2, "internal", 3), (3, "maritime", 2)]
    # WDBII's river levels: 1–4 permanent (major to minor), 5–8
    # intermittent, 9–11 canals. The table, and the level it is first
    # shown at (classes sharing a table share it).
    rivers = {1: ("major_rivers", 0), 2: ("rivers", 3),
              3: ("small_rivers", 5), 4: ("minor_rivers", 7),
              5: ("intermittent_rivers", 6), 6: ("intermittent_rivers", 6),
              7: ("intermittent_rivers", 6), 8: ("intermittent_rivers", 6),
              9: ("canals", 6), 10: ("canals", 6), 11: ("canals", 6)}
    classes = []
    for index, r in enumerate(resolutions):
        for level, kind, first in borders:
            classes.append({
                "source": "gshhg",
                "member": f"WDBII_shp/{r}/WDBII_border_{r}_L{level}.shp",
                "geometry": "lines", "table": f"{kind}_borders",
                "minLevel": first, "lods": [index], "dissolve": False,
                "attributes": [{"name": "kind", "value": kind}],
            })
        for level, (table, first) in rivers.items():
            classes.append({
                "source": "gshhg",
                "member": f"WDBII_shp/{r}/WDBII_river_{r}_L{level:02d}.shp",
                "geometry": "lines", "table": table,
                "minLevel": first, "lods": [index], "dissolve": False,
                "attributes": [{"name": "level", "value": str(level),
                                "kind": "number"}],
            })
    layers = [
        {"id": "national", "name": "WDBII National Borders",
         "tables": ["national_borders"],
         "style": {"outline": "#ffd54fff", "outlineWidthPx": 1.4}},
        {"id": "internal", "name": "WDBII Internal Borders",
         "tables": ["internal_borders"],
         "style": {"outline": "#e6ee9cff", "outlineWidthPx": 0.9}},
        {"id": "maritime", "name": "WDBII Maritime Boundaries",
         "tables": ["maritime_borders"],
         "style": {"outline": "#80deeaff", "outlineWidthPx": 1.0}},
        {"id": "rivers", "name": "WDBII Rivers",
         "tables": ["major_rivers", "rivers", "small_rivers", "minor_rivers"],
         "style": {"outline": "#5aa9ffff", "outlineWidthPx": 0.9},
         "styleRules": [{"attribute": "level", "equals": "1",
                         "outlineWidthPx": 1.5},
                        {"attribute": "level", "equals": "2",
                         "outlineWidthPx": 1.2}]},
        {"id": "intermittent", "name": "WDBII Intermittent Rivers",
         "tables": ["intermittent_rivers"],
         "style": {"outline": "#5aa9ff99", "outlineWidthPx": 0.8}},
        {"id": "canals", "name": "WDBII Canals", "tables": ["canals"],
         "style": {"outline": "#3fd0d0ff", "outlineWidthPx": 0.9}},
    ]
    cfg = {
        "id": "wdbii",
        "version": "2.3.7.1",
        "index": {
            "name": "WDBII borders and rivers",
            "description": "National, internal and maritime boundaries, "
            "rivers (permanent and intermittent) and canals from the World "
            "Data Bank II, as GSHHG 2.3.7 distributes it, at five "
            "resolutions down to the full one. The borders are those of "
            "the 1980s.",
            "licence": "LGPL-3.0-or-later",
        },
        "sizeLimitBytes": 150000000,
        "reservedBytes": 0,
        "sources": [GSHHG],
        "vector": {
            "dataset": "wdbii",
            "name": "WDBII borders and rivers",
            "licence": "LGPL-3.0-or-later",
            "attribution": "Borders and rivers: WDBII via GSHHG 2.3.7, "
            "P. Wessel and W. H. F. Smith (LGPL)",
            "sourceName": GSHHG["name"],
            "sourceVersion": "2.3.7",
            "sourceUrl": "https://www.soest.hawaii.edu/pwessel/gshhg/",
            "lods": lods,
            "classes": classes,
            "layers": layers,
        },
    }
    write(cfg, os.path.join(out, "wdbii.json"))


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "packs")
    wdbii(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
