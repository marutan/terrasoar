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

"""Writes the VMAP0 pack configurations, tools/packager/packs/vmap0-*.json.

Usage: make_vmap0_configs.py [<packs directory>]

The seventeen configurations share their sources, levels of detail and
much else, so they are written from this one description rather than
edited by hand: change it here and run it again. Hydrography, elevation
and vegetation are too big for one pack (see docs/data/vmap0.md), so each
is four regional packs, one per VMAP0 library, with the same dataset and
layers ("layerGroup"): Terra Soar reads the regions installed together.
See docs/tools/packager.md.
"""

import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The four VMAP0 edition 5 CD-ROMs, as the University of Minnesota keeps
# them (NGA no longer publishes VMAP0).
SOURCES = [
    {
        "id": "vmap0-noamer",
        "file": "vmap0-noamer.zip",
        "url": "https://geo.lib.umn.edu/gisdata/vmap0/noamer.zip",
        "mirror": "https://github.com/marutan/terrasoar-packs-sources/releases/download/vmap0-5.0/vmap0-noamer.zip",
        "sha256": "45241a1fb60d97eff7f69f7af836275440cc4dae0d8caeca3684b7d3f5dedd94",
        "name": "Vector Map Level 0 (VMAP0) edition 5, library NOAMER (North America)",
        "version": "5.0",
        "notices": [
            "README1.TXT",
            "FGDC_DAT.TXT"
        ],
        "localNotices": []
    },
    {
        "id": "vmap0-eurnasia",
        "file": "vmap0-eurnasia.zip",
        "url": "https://geo.lib.umn.edu/gisdata/vmap0/eurnasia.zip",
        "mirror": "https://github.com/marutan/terrasoar-packs-sources/releases/download/vmap0-5.0/vmap0-eurnasia.zip",
        "sha256": "bd2cad4328851023ede844c4e125ca1e0de10c2813c3d0c07ada25023b55e8f0",
        "name": "Vector Map Level 0 (VMAP0) edition 5, library EURNASIA (Europe and North Asia)",
        "version": "5.0",
        "notices": [
            "README1.TXT",
            "FGDC_DAT.TXT"
        ],
        "localNotices": []
    },
    {
        "id": "vmap0-soamafr",
        "file": "vmap0-soamafr.zip",
        "url": "https://geo.lib.umn.edu/gisdata/vmap0/soamafr.zip",
        "mirror": "https://github.com/marutan/terrasoar-packs-sources/releases/download/vmap0-5.0/vmap0-soamafr.zip",
        "sha256": "41367cbb4bbd875a2cd9361cd15a45cc031d4c03c238596c0b1ae4510f69d0fe",
        "name": "Vector Map Level 0 (VMAP0) edition 5, library SOAMAFR (South America, Africa and Antarctica)",
        "version": "5.0",
        "notices": [
            "README1.TXT",
            "FGDC_DAT.TXT"
        ],
        "localNotices": []
    },
    {
        "id": "vmap0-sasaus",
        "file": "vmap0-sasaus.zip",
        "url": "https://geo.lib.umn.edu/gisdata/vmap0/sasaus.zip",
        "mirror": "https://github.com/marutan/terrasoar-packs-sources/releases/download/vmap0-5.0/vmap0-sasaus.zip",
        "sha256": "b1af09093af52aa9d92edd91465f6311f28d94c7ef4d3b9b4e438a2ed337fbef",
        "name": "Vector Map Level 0 (VMAP0) edition 5, library SASAUS (South Asia and Australia)",
        "version": "5.0",
        "notices": [
            "README1.TXT",
            "FGDC_DAT.TXT"
        ],
        "localNotices": []
    }
]

LODS=[{"storageLevel":0,"minLevel":0,"maxLevel":1,"toleranceDeg":0.08},
      {"storageLevel":2,"minLevel":2,"maxLevel":3,"toleranceDeg":0.02},
      {"storageLevel":4,"minLevel":4,"maxLevel":5,"toleranceDeg":0.005},
      {"storageLevel":6,"minLevel":6,"maxLevel":-1,"toleranceDeg":0.001}]
F=[{"column":"f_code","name":"feature"}]
def A(col,name,kind=None):
    a={"column":col,"name":name}
    if kind: a["kind"]=kind
    return a
REGIONS={"noamer":"North America","eurnasia":"Europe and North Asia","soamafr":"South America, Africa and Antarctica","sasaus":"South Asia and Australia"}
def pack(id_, name, desc, dataset, dsname, classes, layers, licence="Public domain (NGA)", attribution="Vector Map Level 0 (VMAP0), NGA", size=150000000, version="5.1", notices=None, srcs=None, library=None, group=None, lods=None):
    srcs = copy.deepcopy(srcs or SOURCES)
    if library:
        srcs=[x for x in srcs if x["id"]=="vmap0-"+library]
    for x in srcs: x["localNotices"]=[]
    srcs[0]["localNotices"]=["../notices/vmap0.txt"]
    cfg={"id":id_,"version":version}
    if group: cfg["layerGroup"]=group
    cfg.update({"index":{"name":name,"description":desc,"licence":licence},
      "sizeLimitBytes":size,"reservedBytes":0,
      "sources":srcs})
    out=cfg
    out["vmap0"]={"dataset":dataset,"sources":[s["id"] for s in srcs],"name":dsname,"licence":licence,"attribution":attribution,
        "sourceName":"Vector Map Level 0 (VMAP0) edition 5, NGA","sourceVersion":"5.0","sourceUrl":"https://geo.lib.umn.edu/gisdata/vmap0/",
        "lods":lods or LODS,"classes":classes,"layers":layers}
    if library: out["vmap0"]["libraries"]=[library]
    return out
def regional(id_, name, desc, dataset, dsname, classes, layers, out, lods=None):
    for lib, region in REGIONS.items():
        write(pack(id_+"-"+lib, name+": "+region, desc+" This pack covers VMAP0's "+region+" library; install the regions you want.", dataset, dsname, classes, layers, library=lib, group=name, lods=lods), out+"/"+id_+"-"+lib+".json")
def write(cfg, path):
    with open(path,'w') as f:
        f.write(json.dumps(cfg,indent=2,ensure_ascii=False)+"\n")


def transportation(out):
    roads_attr=F+[A("rtt","route_type"),A("med","median"),A("exs","existence"),A("acc","accuracy")]
    classes=[
     {"coverage":"trans","class":"roadl","table":"main_roads","minLevel":3,"attributes":roads_attr,"filters":[{"column":"rtt","values":["14"]}]},
     {"coverage":"trans","class":"roadl","table":"roads","minLevel":6,"attributes":roads_attr,"filters":[{"column":"rtt","values":["14"],"exclude":True}]},
     {"coverage":"trans","class":"railrdl","table":"railways","minLevel":4,"attributes":F+[A("exs","existence"),A("fco","tracks")]},
     {"coverage":"trans","class":"traill","table":"trails","minLevel":7,"attributes":F+[A("exs","existence")]},
     {"coverage":"trans","class":"transtrl","table":"ferries","minLevel":4,"attributes":F,"filters":[{"column":"f_code","values":["AQ070"]}]},
     {"coverage":"trans","class":"transtrl","table":"crossings","minLevel":7,"attributes":F+[A("tuc","carries")],"filters":[{"column":"f_code","values":["AQ070"],"exclude":True}]},
     {"coverage":"trans","class":"transtrc","table":"crossing_points","minLevel":8,"attributes":F+[A("tuc","carries")]},
     {"coverage":"trans","class":"aerofacp","table":"airports","minLevel":5,"attributes":F+[A("nam","name"),A("iko","icao","text"),A("na3","designation","text"),A("use","use"),A("zv3","elevation_m","number")]},
     {"coverage":"trans","class":"rryardp","table":"railway_yards","minLevel":8,"attributes":F},
     {"coverage":"trans","class":"mistranl","table":"cableways","minLevel":8,"attributes":F},
    ]
    layers=[
     {"id":"roads","name":"VMAP0 Roads","tables":["main_roads","roads"],"style":{"outline":"#f5d9a6ff","outlineWidthPx":1.0},
      "styleRules":[{"attribute":"route_type","equals":"Primary Route","outline":"#f2a33aff","outlineWidthPx":1.8},
                    {"attribute":"route_type","equals":"Secondary Route","outline":"#f5c46eff","outlineWidthPx":1.3}]},
     {"id":"railways","name":"VMAP0 Railways","tables":["railways"],"style":{"outline":"#d6604dff","outlineWidthPx":1.2},
      "styleRules":[{"attribute":"existence","equals":"Under Construction","outline":"#d6604d80","outlineWidthPx":1.0},
                    {"attribute":"existence","equals":"Not Usable","outline":"#a0505080","outlineWidthPx":1.0}]},
     {"id":"trails","name":"VMAP0 Trails","tables":["trails"],"style":{"outline":"#cdb48bcc","outlineWidthPx":0.8}},
     {"id":"ferries","name":"VMAP0 Ferry Routes","tables":["ferries"],"style":{"outline":"#4fc3f7ff","outlineWidthPx":1.2}},
     {"id":"crossings","name":"VMAP0 Bridges, Tunnels and Fords","tables":["crossings","crossing_points"],"style":{"outline":"#e0e0e0ff","outlineWidthPx":1.6,"pointSizePx":5}},
     {"id":"airports","name":"VMAP0 Airports","tables":["airports"],"style":{"outline":"#ff7ad9ff","pointSizePx":7,"labels":"name"},
      "styleRules":[{"attribute":"use","equals":"Military","outline":"#c792eaff"}]},
     {"id":"other","name":"VMAP0 Railway Yards and Cableways","tables":["railway_yards","cableways"],"style":{"outline":"#bdbdbdff","outlineWidthPx":1.0,"pointSizePx":5}},
    ]
    cfg=pack("vmap0-transportation","VMAP0 transportation",
      "Roads (by class), railways, trails, ferry routes, bridges and tunnels, and airports with their names, from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000).",
      "vmap0-transportation","VMAP0 transportation",classes,layers)
    write(cfg, os.path.join(out, 'vmap0-transportation.json'))


def others(out):
    NAMED={"column":"nam","values":["UNK"],"exclude":True}
    UNNAMED={"column":"nam","values":["UNK"]}
    # --- hydrography
    cls=[
     {"coverage":"bnd","class":"coastl","table":"coastlines","minLevel":2,"attributes":F+[A("exs","existence"),A("acc","accuracy")]},
     {"coverage":"hydro","class":"watrcrsl","table":"named_rivers","minLevel":4,"attributes":F+[A("nam","name"),A("hyc","hydrologic_category")],"filters":[NAMED]},
     {"coverage":"hydro","class":"watrcrsl","table":"rivers","minLevel":7,"attributes":F+[A("hyc","hydrologic_category")],"filters":[UNNAMED,{"column":"hyc","values":["6"],"exclude":True}]},
     {"coverage":"hydro","class":"watrcrsl","table":"intermittent_rivers","minLevel":7,"attributes":F+[A("hyc","hydrologic_category")],"filters":[UNNAMED,{"column":"hyc","values":["6"]}]},
     {"coverage":"hydro","class":"inwatera","table":"lakes","minLevel":3,"attributes":F+[A("nam","name"),A("hyc","hydrologic_category")],"filters":[{"column":"f_code","values":["BH000"]}]},
     {"coverage":"hydro","class":"inwatera","table":"inundation","minLevel":7,"attributes":F+[A("nam","name"),A("hyc","hydrologic_category")],"filters":[{"column":"f_code","values":["BH000"],"exclude":True}]},
     {"coverage":"hydro","class":"aquecanl","table":"canals","minLevel":6,"attributes":F+[A("nam","name"),A("exs","existence"),A("loc","location")]},
     {"coverage":"hydro","class":"miscl","table":"dams","minLevel":7,"attributes":F},
     {"coverage":"hydro","class":"miscp","table":"water_points","minLevel":7,"attributes":F},
     {"coverage":"hydro","class":"dangerp","table":"hazards","minLevel":7,"attributes":F},
    ]
    lay=[
     {"id":"lakes","name":"VMAP0 Lakes","tables":["lakes","inundation"],"style":{"fill":"#3d8fe0b3","outline":"#5aa9ffff","outlineWidthPx":0.8},
      "styleRules":[{"attribute":"hydrologic_category","equals":"Non-Perennial/Intermittent/Fluctuating","fill":"#3d8fe059"},
                    {"attribute":"feature","equals":"Land Subject to Inundation","fill":"#6fb7c440","outline":"#6fb7c4aa"}]},
     {"id":"rivers","name":"VMAP0 Rivers","tables":["named_rivers","rivers","intermittent_rivers"],"style":{"outline":"#5aa9ffff","outlineWidthPx":1.0},
      "styleRules":[{"attribute":"hydrologic_category","equals":"Non-Perennial/Intermittent/Fluctuating","outline":"#5aa9ff99","outlineWidthPx":0.8},
                    {"attribute":"table","equals":"named_rivers_2","outlineWidthPx":1.4},{"attribute":"table","equals":"named_rivers_4","outlineWidthPx":1.4},{"attribute":"table","equals":"named_rivers_6","outlineWidthPx":1.4}]},
     {"id":"coastlines","name":"VMAP0 Coastlines","tables":["coastlines"],"style":{"outline":"#8fd3ffff","outlineWidthPx":1.1}},
     {"id":"canals","name":"VMAP0 Canals and Aqueducts","tables":["canals","dams"],"style":{"outline":"#3fd0d0ff","outlineWidthPx":1.0}},
     {"id":"points","name":"VMAP0 Waterfalls, Springs and Hazards","tables":["water_points","hazards"],"style":{"outline":"#9fdcffff","pointSizePx":5},
      "styleRules":[{"attribute":"table","equals":"hazards_6","outline":"#ff8a65ff"}]},
    ]
    regional("vmap0-hydrography","VMAP0 hydrography","Rivers (perennial and intermittent, with names), lakes, land subject to inundation, canals and aqueducts, dams, waterfalls and springs, and coastlines, from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000).","vmap0-hydrography","VMAP0 hydrography",cls,lay,out)
    # --- boundaries (public domain)
    cls=[
     {"coverage":"bnd","class":"polbndl","table":"boundaries","minLevel":0,"attributes":F+[A("bst","status"),A("use","use"),A("acc","accuracy")],"filters":[{"column":"use","values":["26"],"exclude":True}]},
    ]
    lay=[
     {"id":"boundaries","name":"VMAP0 International Boundaries","tables":["boundaries"],"style":{"outline":"#ffd54fff","outlineWidthPx":1.4},
      "styleRules":[{"attribute":"status","equals":"In Dispute","outline":"#ff7043ff","outlineWidthPx":1.4},
                    {"attribute":"feature","equals":"Claim Line","outline":"#ffab91cc","outlineWidthPx":1.0},
                    {"attribute":"feature","equals":"Defacto Boundary","outline":"#ffcc80ff","outlineWidthPx":1.2},
                    {"attribute":"feature","equals":"International Date Line","outline":"#b39ddbff","outlineWidthPx":1.0}]},
    ]
    write(pack("vmap0-boundaries","VMAP0 boundaries","International boundaries, disputed boundaries (drawn differently), claim lines, de facto boundaries and the International Date Line, from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000). Boundaries as compiled then: no statement on any boundary.","vmap0-boundaries","VMAP0 boundaries",cls,lay),out+"/vmap0-boundaries.json")
    # --- admin (ESRI)
    cls=[
     {"coverage":"bnd","class":"polbndl","table":"first_order_boundaries","minLevel":3,"attributes":F+[A("bst","status"),A("use","use"),A("acc","accuracy")],"filters":[{"column":"use","values":["26"]}]},
     {"coverage":"bnd","class":"polbnda","table":"admin_areas","minLevel":3,"attributes":F+[A("nam","name"),A("na2","country"),A("na3","region")],"filters":[NAMED]},
    ]
    lay=[
     {"id":"boundaries","name":"VMAP0 First-Order Administrative Boundaries","tables":["first_order_boundaries"],"style":{"outline":"#e6ee9cff","outlineWidthPx":0.9}},
     {"id":"areas","name":"VMAP0 Administrative Areas","tables":["admin_areas"],"style":{"fill":"#fff59d14"}},
    ]
    write(pack("vmap0-admin","VMAP0 administrative areas (ESRI)","First-order administrative boundaries (states, provinces) and named administrative areas from VMAP0 edition 5. These features are ESRI's intellectual property, licensed for internal or non-commercial use only (ESRI Data License Agreement, included).","vmap0-admin","VMAP0 administrative areas",cls,lay,licence="ESRI Data License Agreement (internal or non-commercial use only)",attribution="Administrative areas: Vector Map Level 0 (VMAP0), NGA; © 1992–2000 ESRI and its licensors"),out+"/vmap0-admin.json")
    # --- population
    cls=[
     {"coverage":"pop","class":"builtupa","table":"built_up_areas","minLevel":5,"attributes":F+[A("nam","name")]},
     {"coverage":"pop","class":"builtupp","table":"built_up_points","minLevel":5,"attributes":F+[A("nam","name")]},
     {"coverage":"pop","class":"mispopp","table":"settlements","minLevel":6,"attributes":F+[A("nam","name")],"filters":[{"column":"f_code","values":["AL105","AL135"]}]},
     {"coverage":"pop","class":"mispopp","table":"landmarks","minLevel":8,"attributes":F+[A("nam","name"),A("txt","note","text")],"filters":[{"column":"f_code","values":["AL105","AL135"],"exclude":True}]},
    ]
    lay=[
     {"id":"areas","name":"VMAP0 Built-Up Areas","tables":["built_up_areas"],"style":{"fill":"#ffb74d66","outline":"#ffb74dcc","outlineWidthPx":0.8}},
     {"id":"places","name":"VMAP0 Places","tables":["built_up_points","settlements"],"style":{"outline":"#fff3e0ff","pointSizePx":4,"labels":"name"}},
     {"id":"landmarks","name":"VMAP0 Landmarks","tables":["landmarks"],"style":{"outline":"#ffccbcff","pointSizePx":4}},
    ]
    write(pack("vmap0-population","VMAP0 population","Built-up areas, settlements with their names (shown as labels), and landmarks (lighthouses, ruins, monuments, camps), from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000).","vmap0-population","VMAP0 population",cls,lay),out+"/vmap0-population.json")
    # --- elevation
    cls=[
     {"coverage":"elev","class":"contourl","table":"contours","minLevel":7,"attributes":F+[A("zv2","elevation_m","number"),A("hqc","category")]},
     {"coverage":"elev","class":"elevp","table":"spot_heights","minLevel":7,"attributes":F+[A("zv2","elevation_m","number"),A("acc","accuracy")]},
     {"coverage":"bnd","class":"depthl","table":"depth_contours","minLevel":4,"attributes":F+[A("crv","depth_m","number")]},
    ]
    lay=[
     {"id":"contours","name":"VMAP0 Contours","tables":["contours"],"style":{"outline":"#c8a165b3","outlineWidthPx":0.8}},
     {"id":"heights","name":"VMAP0 Spot Heights","tables":["spot_heights"],"style":{"outline":"#ffe0b2ff","pointSizePx":4,"labels":"elevation_m"}},
     {"id":"depths","name":"VMAP0 Depth Contours","tables":["depth_contours"],"style":{"outline":"#80deeab3","outlineWidthPx":0.8}},
    ]
    regional("vmap0-elevation","VMAP0 elevation","Contour lines (with their heights in metres), spot heights and depth contours, from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000).","vmap0-elevation","VMAP0 elevation",cls,lay,out,lods=LODS[:-1]+[dict(LODS[-1],toleranceDeg=0.002)])
    # --- vegetation
    cls=[
     {"coverage":"veg","class":"treesa","table":"trees","minLevel":5,"attributes":F+[A("veg","vegetation")]},
     {"coverage":"veg","class":"cropa","table":"cropland","minLevel":5,"attributes":F+[A("veg","vegetation")]},
     {"coverage":"veg","class":"grassa","table":"grassland","minLevel":5,"attributes":F},
     {"coverage":"veg","class":"swampa","table":"swamps","minLevel":5,"attributes":F},
     {"coverage":"veg","class":"tundraa","table":"tundra","minLevel":5,"attributes":F},
    ]
    lay=[
     {"id":"vegetation","name":"VMAP0 Vegetation","tables":["trees","cropland","grassland","swamps","tundra"],"style":{"fill":"#66bb6a59"},
      "styleRules":[{"attribute":"feature","equals":"Cropland","fill":"#e6c35c59"},{"attribute":"feature","equals":"Grassland","fill":"#aed58159"},
                    {"attribute":"feature","equals":"Scrub/Brush","fill":"#bcaaa459"},{"attribute":"feature","equals":"Marsh/Swamp","fill":"#4db6ac66"},
                    {"attribute":"feature","equals":"Bog","fill":"#4db6ac66"},{"attribute":"feature","equals":"Tundra","fill":"#b0bec559"}]},
    ]
    regional("vmap0-vegetation","VMAP0 vegetation","Woodland (deciduous, evergreen, mixed), cropland, grassland and scrub, marsh and swamp, and tundra, from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000).","vmap0-vegetation","VMAP0 vegetation",cls,lay,out)
    # --- other themes
    cls=[
     {"coverage":"phys","class":"landicea","table":"ice_fields","minLevel":3,"attributes":F},
     {"coverage":"phys","class":"seaicea","table":"sea_ice","minLevel":2,"attributes":F+[A("nam","name")]},
     {"coverage":"phys","class":"grounda","table":"ground","minLevel":5,"attributes":F+[A("smc","material"),A("swc","wetness")]},
     {"coverage":"ind","class":"extracta","table":"extraction_areas","minLevel":7,"attributes":F},
     {"coverage":"ind","class":"extractp","table":"extraction_points","minLevel":8,"attributes":F+[A("txt","note","text")]},
     {"coverage":"ind","class":"misindp","table":"plants","minLevel":8,"attributes":F+[A("txt","note","text")]},
     {"coverage":"ind","class":"storagep","table":"storage","minLevel":8,"attributes":F+[A("txt","note","text")]},
     {"coverage":"util","class":"pipel","table":"pipelines","minLevel":6,"attributes":F+[A("loc","location")]},
     {"coverage":"util","class":"utill","table":"power_lines","minLevel":6,"attributes":F},
     {"coverage":"util","class":"utilp","table":"utility_points","minLevel":8,"attributes":F+[A("txt","note","text")]},
    ]
    lay=[
     {"id":"ice","name":"VMAP0 Ice and Snow","tables":["ice_fields","sea_ice"],"style":{"fill":"#e3f2fd80","outline":"#bbdefbcc","outlineWidthPx":0.8}},
     {"id":"ground","name":"VMAP0 Ground Surface (Sand, Lava)","tables":["ground"],"style":{"fill":"#d7ccc859"},
      "styleRules":[{"attribute":"material","equals":"Sand","fill":"#ffe08259"},{"attribute":"material","equals":"Lava","fill":"#8d6e6366"}]},
     {"id":"industry","name":"VMAP0 Industry","tables":["extraction_areas","extraction_points","plants","storage"],"style":{"fill":"#ce93d859","outline":"#ce93d8ff","outlineWidthPx":0.8,"pointSizePx":4}},
     {"id":"utilities","name":"VMAP0 Pipelines and Power Lines","tables":["pipelines","power_lines","utility_points"],"style":{"outline":"#f48fb1cc","outlineWidthPx":0.9,"pointSizePx":4},
      "styleRules":[{"attribute":"table","equals":"pipelines_6","outline":"#a1887fcc"}]},
    ]
    write(pack("vmap0-other","VMAP0 ice, ground, industry and utilities","Ice fields, sea ice and ice shelves, sand and lava fields, mines, quarries and oil fields, plants and storage, pipelines and power lines, from NGA's Vector Map Level 0 (1:1,000,000, edition 5, 2000).","vmap0-other","VMAP0 other themes",cls,lay),out+"/vmap0-other.json")


def main(argv):
    out = argv[0] if argv else os.path.join(HERE, "packs")
    transportation(out)
    others(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
