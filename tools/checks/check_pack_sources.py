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

"""Checks every pack's sources are kept in marutan/terrasoar-packs-sources.

Every source of the base pack and the extra packs
(tools/packager/basepack.json, tools/packager/packs/*.json) must have a
"mirror", the release asset of that repository it is downloaded from, named
as its "file", and a "url", where it was first published. A file used by
several packs must have the same URL, mirror and SHA-256 in each. See
docs/design/packs.md, "Where the source data is kept".

Works offline: whether the release assets exist is checked by downloading
them (fetch_sources.py, in the base pack's CI job).

Usage: check_pack_sources.py
"""

import glob
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "packager"))

from fetch_sources import mirror_problem  # noqa: E402


def configurations():
    packager = os.path.join(ROOT, "tools", "packager")
    return [os.path.join(packager, "basepack.json")] + sorted(
        glob.glob(os.path.join(packager, "packs", "*.json")))


def main():
    problems = []
    seen = {}
    for path in configurations():
        name = os.path.relpath(path, ROOT)
        with open(path, encoding="utf-8") as handle:
            sources = json.load(handle).get("sources", [])
        for source in sources:
            file = source.get("file", "?")
            problem = mirror_problem(source)
            if problem:
                problems.append(f"{name}: {file}: {problem}")
            if not source.get("url"):
                problems.append(f"{name}: {file}: no \"url\" (where it was "
                                "first published)")
            pinned = (source.get("url"), source.get("mirror"),
                      source.get("sha256", "").lower())
            first = seen.setdefault(file, (name, pinned))
            if first[1] != pinned:
                problems.append(f"{name}: {file}: url, mirror or sha256 "
                                f"differs from {first[0]}")
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"Pack sources: {len(seen)} files, {len(problems)} problem(s).")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
