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

"""Fails on the C integer types whose size the platform chooses: short,
int, long, long long and their unsigned (and signed) spellings. Use the
fixed-width <cstdint> types instead (int16_t, int32_t, int64_t, their
uint*_t forms, and intptr_t/uintptr_t for pointer-sized values). See
docs/coding-standards.md.

  check_int_types.py [--fix] [--list] [files...]

--fix rewrites them in place: short -> int16_t, int -> int32_t,
long long -> int64_t, the unsigned forms to uint*_t, and LL literals
(1LL, 2ULL) to int64_t{1}, uint64_t{2}. Plain long and L literals (32
bits on Windows, 64 on Linux) are reported but never rewritten: choose
int32_t, int64_t or intptr_t for each by hand. With no files it checks
every C++ source in the repository.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SPECIFIERS = {"signed", "unsigned", "short", "long", "int", "char"}
SUFFIXES = (".cpp", ".h", ".cpp.in")


def code_words(text):
    """Yields (start, end, word) for every identifier or keyword outside
    comments, string and character literals and #include lines."""
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start:
            stripped = text[i:].lstrip(" \t")
            if stripped.startswith("#") and re.match(
                    r"#\s*include\b", stripped):
                i = text.find("\n", i)
                i = n if i < 0 else i
                continue
        if c == "\n":
            line_start = True
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        line_start = False
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif c.isalpha() or c == "_":
            m = re.compile(r"\w+").match(text, i)
            word = m.group(0)
            # Raw string literals: R"x(...)x", u8R"(...)", LR"(...)".
            if word.endswith("R") and m.end() < n and text[m.end()] == '"':
                open_paren = text.find("(", m.end())
                delimiter = text[m.end() + 1:open_paren]
                close = text.find(")" + delimiter + '"', open_paren)
                i = n if close < 0 else close + len(delimiter) + 2
                continue
            if word in ("u8", "u", "U", "L") and m.end() < n and \
                    text[m.end()] in "'\"":
                i = m.end()
                continue
            yield m.start(), m.end(), word
            i = m.end()
        elif c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            # A number, with C++14 ' digit separators.
            m = re.compile(r"[\w.]+(?:'[\w.]+)*").match(text, i)
            j = m.end()
            while j < n and text[j] in "+-" and text[j - 1] in "eEpP":
                j = re.compile(r"[+-][\w.]*").match(text, j).end()
            yield i, j, text[i:j]
            i = j
        else:
            i += 1


def type_runs(text):
    """Yields (start, end, words) for each run of integer type specifiers."""
    run = []
    for start, end, word in code_words(text):
        if word[0].isdigit() or word[0] == ".":
            if run:
                yield run[0][0], run[-1][1], [w for _, _, w in run]
            run = []
            continue
        if word in SPECIFIERS and (
                not run or text[run[-1][1]:start].strip() == ""):
            run.append((start, end, word))
            continue
        if run:
            yield run[0][0], run[-1][1], [w for _, _, w in run]
        run = [(start, end, word)] if word in SPECIFIERS else []
    if run:
        yield run[0][0], run[-1][1], [w for _, _, w in run]


def replacement(words):
    """The fixed-width type for a run, None to leave it, or "long" for a
    plain long that needs a person's choice."""
    if "char" in words:
        return None
    unsigned = "unsigned" in words
    longs = words.count("long")
    if "short" in words:
        bits = 16
    elif longs >= 2:
        bits = 64
    elif longs == 1:
        return "long"
    else:
        bits = 32
    return f"{'u' if unsigned else ''}int{bits}_t"


# An integer literal with a long or long long suffix (its type is long or
# long long, which then mixes badly with int64_t, e.g. in std::max).
LONG_LITERAL = re.compile(
    r"^((?:0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*))"
    r"(?:([uU]?)(ll|LL)([uU]?)|([uU]?)([lL])([uU]?))$")


def literal_replacement(token):
    """int64_t{N}/uint64_t{N} for an LL literal, "long" for an L one, None
    for any other number."""
    m = LONG_LITERAL.match(token)
    if not m:
        return None
    if m.group(3):
        unsigned = bool(m.group(2) or m.group(4))
        return f"{'u' if unsigned else ''}int64_t{{{m.group(1)}}}"
    return "long"


def sources():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, check=True,
                         capture_output=True, text=True).stdout.split()
    return [os.path.join(ROOT, f) for f in out if f.endswith(SUFFIXES)]


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


QT_OR_LIBRARY = re.compile(r"#\s*include\s*<(Q|Qt|gdal|ogr|cpl|proj|geos|sqlite|GeographicLib|GL/)")


def ensure_cstdint(text):
    """Adds #include <cstdint> in the standard-library block (see
    check_include_order.py), starting that block if there is none."""
    if re.search(r"^#\s*include\s*<cstdint>", text, re.M):
        return text
    lines = text.split("\n")
    standard = [k for k, l in enumerate(lines)
                if re.match(r"#\s*include\s*<", l)
                and not QT_OR_LIBRARY.match(l)]
    if standard:
        first = standard[0]
        k = first
        while k < len(lines) and re.match(r"#\s*include\s*<", lines[k]) \
                and not QT_OR_LIBRARY.match(lines[k]):
            k += 1
        block = sorted(lines[first:k] + ["#include <cstdint>"])
        lines[first:k] = block
        return "\n".join(lines)
    angled = [k for k, l in enumerate(lines) if re.match(r"#\s*include\s*<", l)]
    if angled:
        lines[angled[0]:angled[0]] = ["#include <cstdint>", ""]
        return "\n".join(lines)
    quoted = [k for k, l in enumerate(lines)
              if re.match(r'#\s*include\s*"', l)]
    if quoted:
        # After a .cpp's own header, which comes first.
        k = quoted[0] + 1
        lines[k:k] = ["", "#include <cstdint>"]
        return "\n".join(lines)
    guard = [k for k, l in enumerate(lines) if re.match(r"#\s*define\s", l)]
    k = (guard[0] + 1) if guard else 0
    lines[k:k] = ["", "#include <cstdint>"]
    return "\n".join(lines)


def main(argv):
    fix = "--fix" in argv
    files = [a for a in argv if not a.startswith("--")] or sources()
    problems = 0
    for path in files:
        with open(path, encoding="utf-8") as stream:
            text = stream.read()
        edits = []
        for start, end, token in code_words(text):
            new = literal_replacement(token)
            if new is None:
                continue
            rel = os.path.relpath(path, ROOT)
            if new == "long" or not fix:
                problems += 1
                hint = ("long literal: write int32_t{N}, int64_t{N} or "
                        "intptr_t{N}" if new == "long" else f"use {new}")
                print(f"{rel}:{line_of(text, start)}: '{token}': {hint}")
            if fix and new != "long":
                edits.append((start, end, new))
        for start, end, words in type_runs(text):
            new = replacement(words)
            if new is None:
                continue
            rel = os.path.relpath(path, ROOT)
            if new == "long" or not fix:
                problems += 1
                hint = ("plain long: choose int32_t, int64_t or intptr_t"
                        if new == "long" else f"use {new}")
                print(f"{rel}:{line_of(text, start)}: "
                      f"'{' '.join(words)}': {hint}")
            if fix and new != "long":
                edits.append((start, end, new))
        if edits:
            for start, end, new in sorted(edits, reverse=True):
                text = text[:start] + new + text[end:]
            text = ensure_cstdint(text)
            with open(path, "w", encoding="utf-8") as stream:
                stream.write(text)
    print(f"integer type check: "
          f"{'OK' if not problems else f'{problems} problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
