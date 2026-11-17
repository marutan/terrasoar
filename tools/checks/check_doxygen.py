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

"""Runs Doxygen with the repository Doxyfile and fails on any warning.

The Doxyfile already sets WARN_AS_ERROR = FAIL_ON_WARNINGS; this wrapper also
prints the warning log so CI shows it. Set DOXYGEN to choose the binary.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    binary = os.environ.get("DOXYGEN", "doxygen")
    os.makedirs(os.path.join(ROOT, "build", "docs"), exist_ok=True)
    result = subprocess.run([binary, "Doxyfile"], cwd=ROOT,
                            capture_output=True, text=True)
    log_path = os.path.join(ROOT, "build", "docs", "doxygen-warnings.log")
    warnings = ""
    if os.path.exists(log_path):
        with open(log_path, encoding="utf-8") as handle:
            warnings = handle.read().strip()
    if warnings:
        print(warnings)
    if result.returncode != 0 or warnings:
        print(result.stderr.strip(), file=sys.stderr)
        print("Doxygen check: FAILED", file=sys.stderr)
        return 1
    print("Doxygen check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
