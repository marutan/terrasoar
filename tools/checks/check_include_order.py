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

"""Checks the include order in C++ sources:

  1. the file's own header (.cpp files only)
  2. standard library and system headers   <vector>, <windows.h>
  3. Qt                                    <QString>, <QtTest>
  4. third-party: GDAL, PROJ, GEOS, SQLite <gdal.h>, <proj.h>
  5. project                               "ts/base/buildinfo.h"

Each group is a separate block, separated by a blank line; groups appear in
this order. Inside a block clang-format sorts the lines.

Usage: check_include_order.py [files...]   (default: all project sources)
"""

import os
import re
import sys

from sources import read, report, source_files

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

OWN, STD, QT, THIRD, PROJECT = range(5)
NAMES = {OWN: "own header", STD: "standard/system", QT: "Qt",
         THIRD: "third-party", PROJECT: "project"}

THIRD_PARTY = re.compile(
    r"^(gdal|gdal_\w+|ogr_\w+|ogrsf_frmts|cpl_\w+|proj|proj_\w+|proj/.*|"
    r"geos_c|geos/.*|geos\.h|sqlite3|sqlite3ext|GeographicLib/.*)(\.h)?$")
QT_HEADER = re.compile(r"^(Q[A-Za-z0-9_]+|Qt[A-Za-z0-9]+/.*)$")


def classify(path, bracket, name):
    base = os.path.splitext(os.path.basename(path))[0]
    if path.endswith((".cpp", ".cc")) and bracket == '"' and \
            os.path.splitext(os.path.basename(name))[0] == base:
        return OWN
    if bracket == '"':
        return PROJECT
    if QT_HEADER.match(name):
        return QT
    if THIRD_PARTY.match(name):
        return THIRD
    return STD


def check_file(path):
    errors = []
    lines = read(path).splitlines()
    previous_group = -1
    block_group = None
    first_include = True
    conditional_depth = 0
    for number, line in enumerate(lines, 1):
        stripped = line.strip()
        if re.match(r"#\s*if", stripped):
            conditional_depth += 1
        elif re.match(r"#\s*endif", stripped):
            conditional_depth -= 1
        match = INCLUDE.match(line)
        if not match:
            if stripped == "":
                block_group = None
            continue
        # moc output included at the end of a file is not part of the order.
        if match.group(2).endswith(".moc") or \
                os.path.basename(match.group(2)).startswith("moc_"):
            continue
        group = classify(path, match.group(1), match.group(2))
        if group == OWN:
            if not first_include:
                errors.append(f"{path}:{number}: own header must be the first include")
        first_include = False
        if block_group is not None and block_group != group:
            errors.append(f"{path}:{number}: {NAMES[group]} include in a "
                          f"{NAMES[block_group]} block; separate groups with "
                          "a blank line")
        if group < previous_group and conditional_depth == 0:
            errors.append(f"{path}:{number}: {NAMES[group]} include after "
                          f"{NAMES[previous_group]} includes")
        previous_group = max(previous_group, group)
        block_group = group
    return errors


def main(argv):
    errors = []
    for path in source_files(argv[1:]):
        errors.extend(check_file(path))
    return report(errors, "include order check")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
