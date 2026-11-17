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

"""Runs clang-format in check mode over the C++ sources.

Set CLANG_FORMAT to choose the binary (CI pins version 18). With --fix the
files are reformatted in place instead.

Usage: check_format.py [--fix] [files...]
"""

import os
import subprocess
import sys

from sources import ROOT, source_files

REQUIRED_MAJOR = 18


def main(argv):
    fix = "--fix" in argv
    files = source_files([a for a in argv[1:] if a != "--fix"])
    binary = os.environ.get("CLANG_FORMAT", "clang-format")
    try:
        version = subprocess.run([binary, "--version"], check=True,
                                 capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        print(f"{binary} not found; set CLANG_FORMAT.", file=sys.stderr)
        return 2
    if f"version {REQUIRED_MAJOR}." not in version:
        print(f"warning: CI uses clang-format {REQUIRED_MAJOR}; found "
              f"{version.strip()}", file=sys.stderr)
    args = [binary, "--style=file"]
    args += ["-i"] if fix else ["--dry-run", "--Werror"]
    result = subprocess.run(args + files, cwd=ROOT)
    if result.returncode == 0:
        print("clang-format check: OK" if not fix else "clang-format: done")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
