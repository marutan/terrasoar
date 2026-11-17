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

"""Fails if the `auto` keyword is used anywhere in the C++ sources.

Usage: check_no_auto.py [files...]   (default: all project sources)
"""

import re
import sys

from sources import read, report, source_files, strip_comments_and_strings

AUTO = re.compile(r"\bauto\b")


def main(argv):
    errors = []
    for path in source_files(argv[1:]):
        code = strip_comments_and_strings(read(path))
        for number, line in enumerate(code.splitlines(), 1):
            if AUTO.search(line):
                errors.append(f"{path}:{number}: `auto` is not allowed; "
                              "write the type out (docs/coding-standards.md)")
    return report(errors, "auto keyword check")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
