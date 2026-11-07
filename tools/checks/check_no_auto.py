#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
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
