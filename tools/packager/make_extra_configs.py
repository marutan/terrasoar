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


NE_VERSION = "5.1.2"
NE_GEOJSON = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
              "v5.1.2/geojson/")
NE_RELEASE = "naturalearth-10m-vectors-5.1.2"

# The Natural Earth themes used, by file stem: the SHA-256 of
# ne_10m_<stem>.geojson at v5.1.2.
NE_THEMES = {
    "admin_1_states_provinces_lines":
        "1a1f30ccaaf4cc9c4bde34266f0b8cbb955d3a4cf254b756912255f2ec7c75b6",
    "airports":
        "28892a892f670f1cf654c62253c6169fdc2504b92c956addb4ac8224f50cebe0",
    "geography_regions_elevation_points":
        "f98a16867867146ec4146d6d4b18c823eeedb2825947de666116cf9a4e3f43cb",
    "lakes":
        "2d036f53dedec578001c5c30c2959ee7d4eebc1306900fa4367c49929ec8f2d9",
    "populated_places":
        "9b8e3de09048ef00dfc70357dbb9fa324493f214b5e0ae4daf1aa79a8d10116b",
    "ports":
        "c5a5028f5187a5e1b9fe7f0fab99804a8be7482099a2365d404fb7e9dd6b5457",
    "railroads":
        "a0962d7866060ae3e24df255f6d662389ac9ba7e0df40cf329c2281190b861e6",
    "rivers_lake_centerlines":
        "bb854a900ecbd3b408df46d5e16e3e0f974ba55993f9d8b5c26e855273c0905a",
    "roads":
        "66a0c7b438e92fd124822cc5921cfa11042f48c294ade5e0f03f2c6640fd0248",
    "time_zones":
        "aa52ab97e5f906693f31fe2e625c9d6be78a4e74fcd4142a7261235b355cacfe",
}


def ne_source(stem):
    """The source entry of theme ne_10m_<stem> at v5.1.2."""
    file = f"ne_10m_{stem}-{NE_VERSION}.geojson"
    return {
        "id": "ne-" + stem.replace("_", "-"),
        "file": file,
        "url": NE_GEOJSON + f"ne_10m_{stem}.geojson",
        "mirror": f"{MIRROR}/{NE_RELEASE}/{file}",
        "sha256": NE_THEMES[stem],
        "name": f"Natural Earth 1:10m, ne_10m_{stem} (GeoJSON)",
        "version": NE_VERSION,
    }


def tiers(stem, table, geometry, attributes, field, bounds, extra=""):
    """Classes of one theme split by its suggested zoom (Natural Earth's
    min_zoom, web map zoom z ≈ Terra Soar level z − 1): each (below, table
    suffix, first level) takes the features below that zoom not taken by
    an earlier one."""
    classes = []
    lower = None
    for below, suffix, first in bounds:
        parts = []
        if lower is not None:
            parts.append(f"{field} >= {lower}")
        if below is not None:
            parts.append(f"{field} < {below}")
        if extra:
            parts.append(extra)
        cls = {"source": "ne-" + stem.replace("_", "-"),
               "geometry": geometry, "table": table + suffix,
               "minLevel": first, "attributes": attributes}
        if parts:
            cls["where"] = " AND ".join(parts)
        classes.append(cls)
        lower = below
    return classes


def text(column, name):
    return {"column": column, "name": name}


def number(column, name):
    return {"column": column, "name": name, "kind": "number"}


def naturalearth(out):
    """The Natural Earth 1:10m vector themes Terra Soar lacks."""
    lods = [
        {"storageLevel": 0, "minLevel": 0, "maxLevel": 1, "toleranceDeg": 0.05},
        {"storageLevel": 2, "minLevel": 2, "maxLevel": 3, "toleranceDeg": 0.02},
        {"storageLevel": 4, "minLevel": 4, "maxLevel": 5, "toleranceDeg": 0.005},
        {"storageLevel": 6, "minLevel": 6, "maxLevel": -1, "toleranceDeg": 0.001},
    ]
    zoom = [(3, "_major", 0), (5, "", 3), (7, "_minor", 5), (None, "_local", 6)]
    classes = []
    classes += tiers("rivers_lake_centerlines", "rivers", "lines",
                     [text("name", "name"), text("featurecla", "feature")],
                     "min_zoom", zoom)
    classes += tiers("lakes", "lakes", "areas",
                     [text("name", "name"), text("featurecla", "feature")],
                     "min_zoom", zoom)
    classes += tiers("populated_places", "places", "points",
                     [text("NAME", "name"), text("FEATURECLA", "feature"),
                      text("ADM0_A3", "country"), number("POP_MAX", "population")],
                     "MIN_ZOOM", [(3, "_major", 1), (5, "", 3), (7, "_minor", 5),
                                  (None, "_local", 6)])
    classes += tiers("roads", "roads", "lines",
                     [text("type", "type"), text("name", "name"),
                      number("expressway", "expressway")],
                     "min_zoom", [(5, "_major", 3), (7, "", 5), (None, "_minor", 6)],
                     "featurecla <> 'Ferry'")
    classes += tiers("roads", "ferries", "lines", [text("name", "name")],
                     "min_zoom", [(None, "", 4)], "featurecla = 'Ferry'")
    classes += tiers("railroads", "railways", "lines",
                     [text("category", "category")],
                     "scalerank", [(None, "", 4)])
    classes += tiers("airports", "airports", "points",
                     [text("name", "name"), text("type", "type"),
                      text("iata_code", "iata"), text("gps_code", "icao")],
                     "scalerank", [(4, "_major", 3), (None, "", 5)])
    classes += tiers("ports", "ports", "points", [text("name", "name")],
                     "scalerank", [(None, "", 5)])
    classes += tiers("time_zones", "time_zones", "areas",
                     [text("name", "zone"), text("time_zone", "offset"),
                      text("places", "places")], "scalerank", [(None, "", 0)])
    classes += tiers("admin_1_states_provinces_lines", "state_borders", "lines",
                     [text("ADM0_NAME", "country")],
                     "MIN_ZOOM", [(5, "_major", 3), (None, "", 4)])
    classes += tiers("geography_regions_elevation_points", "peaks", "points",
                     [text("name", "name"), text("featurecla", "feature"),
                      number("elevation", "elevation_m")],
                     "min_zoom", [(5, "_major", 3), (None, "", 5)])
    layers = [
        {"id": "time_zones", "name": "Natural Earth Time Zones",
         "tables": ["time_zones"],
         "style": {"fill": "#00000000", "outline": "#b39ddbcc",
                   "outlineWidthPx": 1.0}},
        {"id": "lakes", "name": "Natural Earth Lakes",
         "tables": ["lakes_major", "lakes", "lakes_minor", "lakes_local"],
         "style": {"fill": "#3d8fe0b3", "outline": "#5aa9ffff",
                   "outlineWidthPx": 0.6}},
        {"id": "rivers", "name": "Natural Earth Rivers",
         "tables": ["rivers_major", "rivers", "rivers_minor", "rivers_local"],
         "style": {"outline": "#5aa9ffff", "outlineWidthPx": 1.0},
         "styleRules": [{"attribute": "feature", "equals": "Lake Centerline",
                         "outline": "#5aa9ff80"}]},
        {"id": "state_borders", "name": "Natural Earth State and Province Borders",
         "tables": ["state_borders_major", "state_borders"],
         "style": {"outline": "#e6ee9cff", "outlineWidthPx": 0.8}},
        {"id": "railways", "name": "Natural Earth Railways",
         "tables": ["railways"],
         "style": {"outline": "#d6604dff", "outlineWidthPx": 1.0}},
        {"id": "roads", "name": "Natural Earth Roads",
         "tables": ["roads_major", "roads", "roads_minor", "ferries"],
         "style": {"outline": "#f5c46eff", "outlineWidthPx": 1.0},
         "styleRules": [{"attribute": "type", "equals": "Major Highway",
                         "outline": "#f2a33aff", "outlineWidthPx": 1.8},
                        {"attribute": "expressway", "equals": "1",
                         "outline": "#e8503aff", "outlineWidthPx": 2.0},
                        {"attribute": "table", "equals": "ferries_4",
                         "outline": "#4fc3f7ff", "outlineWidthPx": 1.0},
                        {"attribute": "table", "equals": "ferries_6",
                         "outline": "#4fc3f7ff", "outlineWidthPx": 1.0}]},
        {"id": "airports", "name": "Natural Earth Airports",
         "tables": ["airports_major", "airports"],
         "style": {"outline": "#ff7ad9ff", "pointSizePx": 6, "labels": "name"}},
        {"id": "ports", "name": "Natural Earth Ports", "tables": ["ports"],
         "style": {"outline": "#4fc3f7ff", "pointSizePx": 5, "labels": "name"}},
        {"id": "peaks", "name": "Natural Earth Peaks and Regions",
         "tables": ["peaks_major", "peaks"],
         "style": {"outline": "#ffe0b2ff", "pointSizePx": 4, "labels": "name"}},
        {"id": "places", "name": "Natural Earth Populated Places",
         "tables": ["places_major", "places", "places_minor", "places_local"],
         "style": {"outline": "#fff3e0ff", "pointSizePx": 4, "labels": "name"},
         "styleRules": [{"attribute": "feature", "equals": "Admin-0 capital",
                         "outline": "#ffcc80ff", "pointSizePx": 6}]},
    ]
    stems = sorted({c["source"] for c in classes})
    sources = [ne_source(stem) for stem in NE_THEMES
               if "ne-" + stem.replace("_", "-") in stems]
    cfg = {
        "id": "naturalearth-10m",
        "version": "5.1.2.1",
        "index": {
            "name": "Natural Earth map layers",
            "description": "Natural Earth's 1:10m rivers, lakes, "
            "populated places, roads, railways, airports, ports, time zones, "
            "state and province borders, and peaks, with names, each "
            "shown from the zoom Natural Earth suggests for it.",
            "licence": "Public domain (Natural Earth)",
        },
        "sizeLimitBytes": 200000000,
        "reservedBytes": 0,
        "sources": sources,
        "vector": {
            "dataset": "naturalearth-10m",
            "name": "Natural Earth map layers",
            "licence": "Public domain (Natural Earth)",
            "attribution": "Made with Natural Earth",
            "sourceName": "Natural Earth 1:10m Cultural and Physical Vectors",
            "sourceVersion": NE_VERSION,
            "sourceUrl": "https://github.com/nvkelso/natural-earth-vector/tree/v5.1.2",
            "lods": lods,
            "classes": classes,
            "layers": layers,
        },
    }
    write(cfg, os.path.join(out, "naturalearth-10m.json"))


OURAIRPORTS_COMMIT = "aaf2fe210c52ac2ff42e64f75b5f051759c61d50"
OURAIRPORTS_DATE = "2026-11-22"
OURAIRPORTS_RELEASE = "ourairports-2026-11-22"
OURAIRPORTS_SHA256 = {
    "airports": "60259f22797bf86a11e7ad2e074a91c97cf0e9b5e3ae899f4796ecc15a96e1ee",
    "runways": "26ebd760c6b0ad593cfb4e3153365e5063d01c7a3008f7f5847296ee10692fce",
    "navaids": "d7292380662c33a95214945023ec0a5b13189599305fd18be739e167a97a9bd1",
}


def ourairports_source(table):
    """The source entry of OurAirports' <table>.csv at the pinned commit."""
    file = f"ourairports-{table}-{OURAIRPORTS_DATE}.csv"
    return {
        "id": "ourairports-" + table,
        "file": file,
        "url": ("https://raw.githubusercontent.com/davidmegginson/"
                f"ourairports-data/{OURAIRPORTS_COMMIT}/{table}.csv"),
        "mirror": f"{MIRROR}/{OURAIRPORTS_RELEASE}/{file}",
        "sha256": OURAIRPORTS_SHA256[table],
        "name": f"OurAirports {table}.csv",
        "version": OURAIRPORTS_DATE,
    }


def ourairports(out):
    """Airports, runways and navigation aids from OurAirports."""
    lods = [
        {"storageLevel": 2, "minLevel": 2, "maxLevel": 5},
        {"storageLevel": 6, "minLevel": 6, "maxLevel": -1},
    ]
    points = ["X_POSSIBLE_NAMES=longitude_deg", "Y_POSSIBLE_NAMES=latitude_deg",
              "KEEP_GEOM_COLUMNS=NO"]
    airport = [text("ident", "ident"), text("name", "name"),
               text("type", "type"), text("iata_code", "iata"),
               text("municipality", "municipality"),
               text("iso_country", "country"),
               number("elevation_ft", "elevation_ft")]
    classes = []
    for kind, table, first in [("large_airport", "large_airports", 3),
                               ("medium_airport", "medium_airports", 5),
                               ("small_airport", "small_airports", 7),
                               ("seaplane_base", "seaplane_bases", 7),
                               ("heliport", "heliports", 8)]:
        classes.append({"source": "ourairports-airports", "open": points,
                        "where": f"type = '{kind}'", "geometry": "points",
                        "table": table, "minLevel": first,
                        "attributes": airport})
    classes.append({
        "source": "ourairports-runways", "geometry": "lines",
        "table": "runways", "minLevel": 8, "lods": [1],
        "where": "closed = '0'",
        "lineColumns": ["le_longitude_deg", "le_latitude_deg",
                        "he_longitude_deg", "he_latitude_deg"],
        "attributes": [text("airport_ident", "airport"),
                       text("le_ident", "ends"),
                       number("length_ft", "length_ft"),
                       text("surface", "surface")]})
    classes.append({
        "source": "ourairports-navaids", "open": points, "geometry": "points",
        "table": "navaids", "minLevel": 6,
        "attributes": [text("ident", "ident"), text("name", "name"),
                       text("type", "type"),
                       number("frequency_khz", "frequency_khz"),
                       text("associated_airport", "airport")]})
    layers = [
        {"id": "runways", "name": "OurAirports Runways", "tables": ["runways"],
         "style": {"outline": "#eceff1ff", "outlineWidthPx": 3.0}},
        {"id": "airports", "name": "OurAirports Airports",
         "tables": ["large_airports", "medium_airports", "small_airports",
                    "seaplane_bases"],
         "style": {"outline": "#ff7ad9ff", "pointSizePx": 5, "labels": "name"},
         "styleRules": [{"attribute": "type", "equals": "large_airport",
                         "pointSizePx": 8},
                        {"attribute": "type", "equals": "medium_airport",
                         "pointSizePx": 6},
                        {"attribute": "type", "equals": "seaplane_base",
                         "outline": "#4fc3f7ff"}]},
        {"id": "heliports", "name": "OurAirports Heliports",
         "tables": ["heliports"],
         "style": {"outline": "#ce93d8ff", "pointSizePx": 4, "labels": "ident"}},
        {"id": "navaids", "name": "OurAirports Navigation Aids",
         "tables": ["navaids"],
         "style": {"outline": "#80cbc4ff", "pointSizePx": 5, "labels": "ident"},
         "styleRules": [{"attribute": "type", "equals": "NDB",
                         "outline": "#ffab91ff"}]},
    ]
    cfg = {
        "id": "ourairports",
        "version": OURAIRPORTS_DATE + ".1",
        "index": {
            "name": "Airports (OurAirports)",
            "description": "About 72,000 airports, heliports and seaplane "
            "bases, their runways and 11,000 navigation aids (VOR, NDB, DME) "
            "from OurAirports, as of " + OURAIRPORTS_DATE + ". Community "
            "data, not for navigation.",
            "licence": "Public domain (OurAirports, the Unlicense)",
        },
        "sizeLimitBytes": 100000000,
        "reservedBytes": 0,
        "sources": [ourairports_source(t) for t in ("airports", "runways", "navaids")],
        "vector": {
            "dataset": "ourairports",
            "name": "Airports (OurAirports)",
            "licence": "Public domain (OurAirports, the Unlicense)",
            "attribution": "Airports: OurAirports",
            "sourceName": "OurAirports data",
            "sourceVersion": OURAIRPORTS_DATE,
            "sourceUrl": "https://github.com/davidmegginson/ourairports-data",
            "lods": lods,
            "classes": classes,
            "layers": layers,
        },
    }
    write(cfg, os.path.join(out, "ourairports.json"))


NE_RASTERS = {
    # file stem: (pack id, name, description, SHA-256 of the zip)
    "NE1_HR_LC_SR_W_DR": (
        "naturalearth-relief", "Natural Earth I relief map",
        "Natural Earth I: land cover in natural colours with shaded relief, "
        "water bodies and an ocean in soft blue, 21600 × 10800 pixels "
        "(about 1.9 km per pixel). A map-like alternative to Blue Marble.",
        "8841d08155415f0556462846dcb845a4ccb8f12390b0e0ba36ffde55efadeb56"),
    "HYP_HR_SR_OB_DR": (
        "naturalearth-hypsometric", "Natural Earth hypsometric map",
        "Cross-blended hypsometric tints (lowland greens to highland browns, "
        "by height and climate) with shaded relief, water bodies and the "
        "ocean bottom's relief, 21600 × 10800 pixels.",
        "ee2cbbf5b682e8514374c0c318bbf49aa640ecfe60d0a2e09a90e0c575d30a78"),
    "SR_HR": (
        "naturalearth-shaded-relief", "Natural Earth shaded relief",
        "Grey shaded relief of the land, 21600 × 10800 pixels: a neutral "
        "background, or under a half-transparent layer.",
        "b2619fff2fc73c17152983c066adfaa25c4b626916b822bad2fae8bcd9be41a5"),
}
NE_RASTER_RELEASE = "naturalearth-10m-rasters-2.0.0"


def naturalearth_rasters(out):
    """One pack per Natural Earth 1:10m raster map."""
    for stem, (pack, name, description, sha) in NE_RASTERS.items():
        source = {
            "id": "ne-raster",
            "file": stem + ".zip",
            "url": f"https://naciscdn.org/naturalearth/10m/raster/{stem}.zip",
            "mirror": f"{MIRROR}/{NE_RASTER_RELEASE}/{stem}.zip",
            "sha256": sha,
            "name": f"Natural Earth 1:10m raster {stem}",
            "version": "2.0.0",
        }
        cfg = {
            "id": pack,
            "version": "2.0.0.1",
            "index": {"name": name, "description": description,
                      "licence": "Public domain (Natural Earth)"},
            "sizeLimitBytes": 120000000,
            "reservedBytes": 0,
            "sources": [source],
            "imageryDatasets": [{
                "dataset": pack, "source": "ne-raster",
                "member": stem + ".tif", "layer": name, "name": name,
                "licence": "Public domain (Natural Earth)",
                "attribution": "Made with Natural Earth",
                "maxLevel": 6, "tileSize": 256, "jpegQuality": 85,
            }],
        }
        write(cfg, os.path.join(out, pack + ".json"))


BLACK_MARBLE = {
    # year: (image record, SHA-256)
    "2012": ("144875", "e77d333de16835ab7abbdb3ee67c199de277faf48037dc50552d8e67dfd6897e"),
    "2016": ("144898", "e915ef2a20d84e2a59e1547d3ad564463ad4bcf22bfa02e0e0b8ed1cd722e9c0"),
}


def black_marble(out):
    """Black Marble, the Earth at night, 2012 and 2016 at 3 km."""
    sources = []
    datasets = []
    for year, (record, sha) in BLACK_MARBLE.items():
        file = f"BlackMarble_{year}_3km_geo.tif"
        sources.append({
            "id": "blackmarble-" + year,
            "file": file,
            "url": ("https://assets.science.nasa.gov/content/dam/science/esd/"
                    f"eo/images/imagerecords/144000/{record}/{file}"),
            "mirror": f"{MIRROR}/blackmarble-3km-2012-2016/{file}",
            "sha256": sha,
            "name": f"Black Marble {year}, global, 3 km (13500 × 6750)",
            "version": year,
        })
        datasets.append({
            "dataset": "blackmarble-" + year, "source": "blackmarble-" + year,
            "layer": f"Black Marble {year} (Earth at night)",
            "name": f"Black Marble {year}",
            "licence": "Public domain (NASA)",
            "attribution": "Black Marble: NASA Earth Observatory",
            "maxLevel": 5, "tileSize": 256, "jpegQuality": 85,
        })
    cfg = {
        "id": "blackmarble",
        "version": "2016.1",
        "index": {
            "name": "Black Marble (Earth at night)",
            "description": "NASA's Black Marble: the Earth's city lights "
            "at night from the Suomi NPP satellite, for 2012 and for 2016, "
            "each as its own layer, at about 3 km per pixel.",
            "licence": "Public domain (NASA)",
        },
        "sizeLimitBytes": 100000000,
        "reservedBytes": 0,
        "sources": sources,
        "imageryDatasets": datasets,
    }
    write(cfg, os.path.join(out, "blackmarble.json"))


GNIS_DATE = "2026-11-02"
GNIS_SHA256 = "1ccd7aaa8f50ba7e2340e131bce815154e6c8288df72bbff01e0e99033ac9acd"
# GNIS feature classes kept: class, table, first level shown.
GNIS_CLASSES = [
    ("Sea", "us_seas", 3),
    ("Range", "us_ranges", 5),
    ("Populated Place", "us_places", 7),
    ("Island", "us_islands", 7),
    ("Isthmus", "us_isthmuses", 7),
    ("Glacier", "us_glaciers", 7),
    ("Summit", "us_summits", 8),
    ("Lake", "us_lakes", 8),
    ("Bay", "us_bays", 8),
    ("Cape", "us_capes", 8),
    ("Falls", "us_falls", 8),
]


def gnis_classes():
    """GNIS Domestic Names: the United States' places and major features."""
    file = f"DomesticNames_National_Text-{GNIS_DATE}.zip"
    source = {
        "id": "gnis",
        "file": file,
        "url": ("https://prd-tnm.s3.amazonaws.com/StagedProducts/"
                "GeographicNames/DomesticNames/DomesticNames_National_Text.zip"),
        "mirror": f"{MIRROR}/gnis-domestic-names-{GNIS_DATE}/{file}",
        "sha256": GNIS_SHA256,
        "name": "GNIS Domestic Names, national file (USGS)",
        "version": GNIS_DATE,
    }
    open_options = ["SEPARATOR=PIPE", "AUTODETECT_TYPE=YES",
                    "X_POSSIBLE_NAMES=prim_long_dec",
                    "Y_POSSIBLE_NAMES=prim_lat_dec"]
    classes = []
    for feature_class, table, first in GNIS_CLASSES:
        classes.append({
            "source": "gnis", "member": "Text/DomesticNames_National.txt",
            "driver": "CSV", "open": open_options, "geometry": "points",
            # Without a position, or no longer there.
            "where": (f"feature_class = '{feature_class}' AND prim_lat_dec <> 0"
                      " AND feature_name NOT LIKE '%(historical)%'"),
            "table": table, "minLevel": first,
            "attributes": [text("feature_name", "name"),
                           {"name": "feature", "value": feature_class},
                           text("state_name", "region")]})
    return source, classes


GNS_DATE = "2026-09-03"
GNS_RELEASE = "gns-feature-classes-" + GNS_DATE
# The feature class files used: file stem, its Internet Archive capture
# (NGA's server answers few networks), SHA-256.
GNS_FILES = {
    "Populated_Places": ("20260903061713", "d5d35d2afe119b8b46ab2c3c30e3d78dc68ecd121d9b53a419fd594986495819"),
    "Hypsographic": ("20260903061805", "d395834fb3c03525a7a5b1c78466d68e902b4eb38d675b56b5582bbc5a10d090"),
    "Hydrographic": ("20260903061628", "604f68abbf40cdab2bd01a4bea36af136ebf5802368fd169e07e474a6d983ac8"),
    "Undersea": ("20260903061525", "91e2389673e3e621d29c4b4d16327e8b2dca5ffd71ed6e39f2be7116c9681ebe"),
}
# Display bands: GNS lists the scale bands a name is shown at, from 1
# (the largest scale) up; a name shown at band 9 is shown on the smallest
# scale maps. Lists are contiguous, so "shown at band k or smaller" is
# "display contains k".
GNS_LOCAL = ("display LIKE '%5%'")
GNS_CLASSES = [
    # file, table, first level, filter (approved names, "nt = 'N'", only)
    ("Populated_Places", "world_capitals", 1, "desig_cd = 'PPLC'"),
    ("Populated_Places", "admin_seats", 4, "desig_cd = 'PPLA'"),
    ("Populated_Places", "places_major", 5,
     "desig_cd NOT IN ('PPLC', 'PPLA', 'PPLQ', 'PPLW', 'PPLH', 'PPLCH') "
     "AND display LIKE '%9%'"),
    ("Populated_Places", "places", 6,
     "desig_cd NOT IN ('PPLC', 'PPLA', 'PPLQ', 'PPLW', 'PPLH', 'PPLCH') "
     "AND display LIKE '%7%' AND display NOT LIKE '%9%'"),
    ("Populated_Places", "places_minor", 7,
     "desig_cd NOT IN ('PPLC', 'PPLA', 'PPLQ', 'PPLW', 'PPLH', 'PPLCH') "
     "AND display LIKE '%5%' AND display NOT LIKE '%7%'"),
    ("Hypsographic", "ranges", 5, "desig_cd = 'MTS' AND " + GNS_LOCAL),
    ("Hypsographic", "mountains_major", 5,
     "desig_cd IN ('MT', 'PK', 'VLC') AND display LIKE '%9%'"),
    ("Hypsographic", "mountains", 6,
     "desig_cd IN ('MT', 'PK', 'VLC') AND display LIKE '%7%' AND "
     "display NOT LIKE '%9%'"),
    ("Hypsographic", "mountains_minor", 7,
     "desig_cd IN ('MT', 'PK', 'VLC') AND display LIKE '%5%' AND "
     "display NOT LIKE '%7%'"),
    ("Hypsographic", "islands_major", 6,
     "desig_cd IN ('ISL', 'ISLS') AND display LIKE '%7%'"),
    ("Hypsographic", "islands", 7,
     "desig_cd IN ('ISL', 'ISLS') AND display LIKE '%5%' AND "
     "display NOT LIKE '%7%'"),
    ("Hypsographic", "capes", 7,
     "desig_cd IN ('CAPE', 'PEN') AND " + GNS_LOCAL),
    ("Hydrographic", "seas", 4,
     "desig_cd IN ('SEA', 'GULF', 'BAY', 'STRT') AND display LIKE '%7%'"),
    ("Hydrographic", "bays", 6,
     "desig_cd IN ('SEA', 'GULF', 'BAY', 'STRT') AND display LIKE '%5%' AND "
     "display NOT LIKE '%7%'"),
    ("Hydrographic", "lakes_major", 6,
     "desig_cd IN ('LK', 'LKS', 'RSV', 'LGN') AND display LIKE '%7%'"),
    ("Hydrographic", "lakes", 7,
     "desig_cd IN ('LK', 'LKS', 'RSV', 'LGN') AND display LIKE '%5%' AND "
     "display NOT LIKE '%7%'"),
    ("Hydrographic", "falls_glaciers", 7,
     "desig_cd IN ('FLLS', 'GLCR') AND " + GNS_LOCAL),
    ("Undersea", "undersea", 4, "1 = 1"),
]


def gns_classes():
    """GNS: the world's places and major features outside the US."""
    sources = []
    for stem, (capture, sha) in GNS_FILES.items():
        file = f"gns-{stem}-{GNS_DATE}.zip"
        sources.append({
            "id": "gns-" + stem.lower().replace("_", "-"),
            "file": file,
            "url": ("https://geonames.nga.mil/geonames/GNSData/fc_files/"
                    f"{stem}.zip"),
            "mirror": f"{MIRROR}/{GNS_RELEASE}/{file}",
            "sha256": sha,
            "name": f"GEOnet Names Server, feature class {stem} (NGA)",
            "version": GNS_DATE,
            "notices": ["disclaimer.txt"],
        })
    open_options = ["SEPARATOR=TAB", "X_POSSIBLE_NAMES=long_dd",
                    "Y_POSSIBLE_NAMES=lat_dd"]
    classes = []
    for stem, table, first, where in GNS_CLASSES:
        classes.append({
            "source": "gns-" + stem.lower().replace("_", "-"),
            "member": stem + ".txt", "driver": "CSV", "open": open_options,
            "geometry": "points", "where": "nt = 'N' AND " + where,
            "table": table, "minLevel": first,
            "attributes": [text("full_name", "name"),
                           text("desig_cd", "feature"),
                           text("cc_ft", "region")]})
    return sources, classes


def names(out):
    """Place names: populated places and major features, GNIS for the US
    and GNS for the rest of the world."""
    # Capitals from the whole globe on; everything else from level 3.
    lods = [
        {"storageLevel": 0, "minLevel": 0, "maxLevel": 2},
        {"storageLevel": 3, "minLevel": 3, "maxLevel": 5},
        {"storageLevel": 6, "minLevel": 6, "maxLevel": -1},
    ]
    gnis_source, gnis = gnis_classes()
    gns_sources, gns = gns_classes()
    sources = gns_sources + [gnis_source]
    classes = gns + gnis
    place_tables = ["world_capitals", "admin_seats", "places_major", "places",
                    "places_minor", "us_places"]
    places = [c["table"] for c in classes if c["table"] in place_tables]
    features = [c["table"] for c in classes if c["table"] not in place_tables]
    water = ["Lake", "Bay", "Sea", "LK", "LKS", "RSV", "LGN", "SEA", "GULF",
             "BAY", "STRT"]
    heights = ["Summit", "MT", "PK", "VLC", "Range", "MTS"]
    layers = [
        {"id": "places", "name": "Place Names", "tables": places,
         "style": {"outline": "#fff3e0ff", "pointSizePx": 3, "labels": "name"},
         "styleRules": [{"attribute": "feature", "equals": "PPLC",
                         "outline": "#ffcc80ff", "pointSizePx": 6},
                        {"attribute": "feature", "equals": "PPLA",
                         "pointSizePx": 5}]},
        {"id": "features", "name": "Feature Names (Mountains, Lakes, Islands…)",
         "tables": features,
         "style": {"outline": "#c5e1a5ff", "pointSizePx": 3, "labels": "name"},
         "styleRules": [{"attribute": "feature", "equals": f,
                         "outline": "#ffe0b2ff"} for f in heights] +
                       [{"attribute": "feature", "equals": f,
                         "outline": "#90caf9ff"} for f in water]},
    ]
    cfg = {
        "id": "names",
        "version": "2026.1",
        "index": {
            "name": "Place names",
            "description": "Names of populated places and major features "
            "(mountains and ranges, lakes, bays and seas, islands, capes, "
            "glaciers, falls) as labels: the US Board on Geographic Names' "
            "names, from USGS GNIS for the United States and NGA GNS for "
            "the rest of the world. Main names only.",
            "licence": "Public domain (USGS, NGA)",
        },
        "sizeLimitBytes": 400000000,
        "reservedBytes": 0,
        "sources": sources,
        "vector": {
            "dataset": "names",
            "name": "Place names",
            "licence": "Public domain (USGS, NGA)",
            "attribution": "Names: USGS GNIS, NGA GNS",
            "sourceName": "GNIS Domestic Names; GEOnet Names Server",
            "sourceVersion": GNIS_DATE,
            "sourceUrl": "https://www.usgs.gov/tools/geographic-names-information-system-gnis",
            "lods": lods,
            "classes": classes,
            "layers": layers,
        },
    }
    write(cfg, os.path.join(out, "names.json"))


ETOPO_30S = {
    "id": "etopo-30s",
    "file": "ETOPO_2022_v1_30s_N90W180_surface.tif",
    "url": ("https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/30s/"
            "30s_surface_elev_gtif/ETOPO_2022_v1_30s_N90W180_surface.tif"),
    "mirror": MIRROR + ("/etopo-2022-v1-30s-surface/"
                        "ETOPO_2022_v1_30s_N90W180_surface.tif"),
    "sha256": "8630abc401cc6bdd30b507a68d3eb9eda5b65f5636f7199e4b1eefd476b5a9e2",
    "name": "ETOPO 2022 Global Relief Model, 30 arc-second, surface elevation",
    "version": "2022 v1",
}


def etopo_30s(out):
    """ETOPO 2022 at 30″ in place of the base pack's 2.64′ terrain."""
    cfg = {
        "id": "elevation-etopo-30s",
        "version": "2022.1",
        "index": {
            "name": "Terrain, finer (ETOPO 2022)",
            "description": "The Earth's land and sea floor heights from "
            "ETOPO 2022 at 30″, about 0.9 km, instead of the base pack's 4.9 km: "
            "the Terrain layer shows mountains, valleys and the sea floor "
            "in more detail. Replaces the base pack's terrain while "
            "installed.",
            "licence": "CC0-1.0 (NOAA)",
        },
        "sizeLimitBytes": 1500000000,
        "reservedBytes": 0,
        "sources": [ETOPO_30S],
        "elevation": {
            "dataset": "elevation-etopo-30s", "source": "etopo-30s",
            "replaces": "elevation", "name": "Elevation, finer",
            "licence": "CC0-1.0 (NOAA)",
            "attribution": "Elevation: ETOPO 2022, NOAA NCEI",
            # 85 samples a side: 84 intervals of a level-8 tile are 30.1″,
            # the source's spacing (65 would be 40″).
            "maxLevel": 8, "samples": 85, "verticalDatum": "egm2008",
        },
    }
    write(cfg, os.path.join(out, "elevation-etopo-30s.json"))


GEBCO = {
    "id": "gebco",
    "file": "gebco_2026_geotiff.zip",
    "url": ("https://dap.ceda.ac.uk/bodc/gebco/global/gebco_2026/"
            "ice_surface_elevation/geotiff/gebco_2026_geotiff.zip"),
    "mirror": MIRROR + "/gebco-2026/gebco_2026_geotiff.zip",
    # 4.2 GB, more than a release asset may be: kept in three parts.
    "parts": 3,
    "sha256": "39bb6eb45a785101da606749a00cf0ec3217f32160d25859b48fd5db9c6d9368",
    "name": "The GEBCO_2026 Grid, ice surface elevation, Data GeoTIFF",
    "version": "2026",
    "notices": ["GEBCO_Grid_terms_of_use.pdf"],
}
GEBCO_TILES = [f"gebco_2026_n{n}_s{s}_w{w}_e{e}_geotiff.tif"
               for n, s in (("90.0", "0.0"), ("0.0", "-90.0"))
               for w, e in (("-180.0", "-90.0"), ("-90.0", "0.0"),
                            ("0.0", "90.0"), ("90.0", "180.0"))]
# The regional packs: id suffix, region, name.
GEBCO_REGIONS = [
    ("w180-w90", (-180, -90), "180° W to 90° W: the eastern Pacific and "
     "western North and South America"),
    ("w90-0", (-90, 0), "90° W to 0°: eastern North and South America and "
     "the Atlantic"),
    ("0-e90", (0, 90), "0° to 90° E: Europe, Africa, the Middle East and "
     "the Indian Ocean"),
    ("e90-e180", (90, 180), "90° E to 180°: East and Southeast Asia, "
     "Australia and the western Pacific"),
]


def gebco(out):
    """GEBCO 2026 at 15″, land and sea floor, in four regional packs of one
    dataset, shown as a layer of its own above the Terrain layer."""
    for suffix, (west, east), where in GEBCO_REGIONS:
        cfg = {
            "id": "gebco-" + suffix,
            "version": "2026.1",
            "layerGroup": "GEBCO 2026 terrain",
            "index": {
                "name": "GEBCO terrain and sea floor, " + where.split(":")[0],
                "description": "The GEBCO 2026 Grid at 15″ (about 460 m): "
                "the sea floor and the land, in more detail than the Terrain "
                "layer, for " + where.split(": ")[1] + ". One of four "
                "regional packs; their GEBCO layer shows every region "
                "installed, above the Terrain layer. Not for navigation.",
                "licence": "Public domain (GEBCO)",
            },
            "sizeLimitBytes": 1900000000,
            "reservedBytes": 0,
            "sources": [GEBCO],
            "elevation": {
                "dataset": "gebco-2026", "source": "gebco",
                "members": GEBCO_TILES,
                "layer": "GEBCO 2026 terrain and sea floor",
                "name": "GEBCO 2026",
                "licence": "Public domain (GEBCO)",
                "attribution": "Bathymetry: GEBCO Compilation Group (2026) "
                "GEBCO 2026 Grid",
                # 84 intervals of a level-9 tile are 15.1″, the grid's
                # spacing; levels 0 to 1 come from the Terrain layer.
                "minLevel": 1, "maxLevel": 9, "samples": 85,
                "region": [west, east], "verticalDatum": "egm2008",
            },
        }
        write(cfg, os.path.join(out, "gebco-" + suffix + ".json"))


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "packs")
    wdbii(out)
    naturalearth(out)
    ourairports(out)
    naturalearth_rasters(out)
    black_marble(out)
    names(out)
    etopo_30s(out)
    gebco(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
