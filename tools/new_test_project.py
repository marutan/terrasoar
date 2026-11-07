#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Creates the unit test project for a new subsection from tests/template.

Usage: new_test_project.py <subsection>

<subsection> is the lower-case name used under core/include/ts/, e.g.
`maths`. Writes tests/<subsection>/<subsection>.pro and
tests/<subsection>/tst_<subsection>.cpp, and adds the project to
tests/tests.pro.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
TEMPLATE = os.path.join(TESTS, "template")


def fill(path, values):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    return text


def add_to_tests_pro(subsection):
    path = os.path.join(TESTS, "tests.pro")
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"SUBDIRS\s*=((?:[^\n]*\\\n)*[^\n]*)", text)
    if not match:
        raise SystemExit("tests/tests.pro: no SUBDIRS list found.")
    names = match.group(1).replace("\\", " ").split()
    if subsection in names:
        return
    names.append(subsection)
    listing = "SUBDIRS = \\\n" + " \\\n".join(f"    {n}" for n in names)
    text = text[:match.start()] + listing + text[match.end():]
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def main(argv):
    if len(argv) != 2 or not re.fullmatch(r"[a-z][a-z0-9]*", argv[1]):
        print(__doc__, file=sys.stderr)
        return 2
    subsection = argv[1]
    target = os.path.join(TESTS, subsection)
    if os.path.exists(target):
        print(f"tests/{subsection} already exists.", file=sys.stderr)
        return 1
    values = {
        "SUBSECTION": subsection,
        "CLASS": "Test" + subsection[0].upper() + subsection[1:],
    }
    os.makedirs(target)
    outputs = {
        f"{subsection}.pro": "template.pro.in",
        f"tst_{subsection}.cpp": "tst_template.cpp.in",
    }
    for name, source in outputs.items():
        with open(os.path.join(target, name), "w", encoding="utf-8") as out:
            out.write(fill(os.path.join(TEMPLATE, source), values))
    add_to_tests_pro(subsection)
    print(f"Created tests/{subsection}/ and added it to tests/tests.pro.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
