# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers for the source checks: which files to check, and a
C++ lexer that blanks out comments and string literals."""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SOURCE_DIRS = ("core", "app", "tools", "tests")
SOURCE_EXTENSIONS = (".h", ".hpp", ".cpp", ".cc")
# Directories never checked (build output, third-party code).
EXCLUDED_PARTS = {"build", ".git", "3rdparty", "third_party"}


def source_files(paths=None):
    """Returns the C++ files to check, relative to the repo root."""
    if paths:
        return sorted(os.path.relpath(os.path.abspath(p), ROOT) for p in paths
                      if p.endswith(SOURCE_EXTENSIONS))
    files = []
    try:
        out = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard",
             "--", *SOURCE_DIRS],
            cwd=ROOT, check=True, capture_output=True, text=True).stdout
        files = [f for f in out.splitlines() if f.endswith(SOURCE_EXTENSIONS)]
    except (OSError, subprocess.CalledProcessError):
        for top in SOURCE_DIRS:
            for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, top)):
                dirnames[:] = [d for d in dirnames if d not in EXCLUDED_PARTS]
                for name in filenames:
                    if name.endswith(SOURCE_EXTENSIONS):
                        files.append(os.path.relpath(os.path.join(dirpath, name), ROOT))
    files = [f for f in files
             if not EXCLUDED_PARTS.intersection(f.replace("\\", "/").split("/"))
             and os.path.exists(os.path.join(ROOT, f))]
    return sorted(set(files))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as handle:
        return handle.read()


_TOKEN = re.compile(
    r'//[^\n]*'                              # line comment
    r'|/\*.*?\*/'                            # block comment
    r'|R"(?P<delim>[^(\s]*)\(.*?\)(?P=delim)"'  # raw string
    r'|"(?:\\.|[^"\\\n])*"'                  # string
    r"|'(?:\\.|[^'\\\n])+'",                 # character
    re.S)


def strip_comments_and_strings(text, keep_strings=False):
    """Blanks out comments (and strings unless keep_strings), keeping
    newlines so line numbers stay correct."""

    def blank(match):
        token = match.group(0)
        if keep_strings and not token.startswith(("//", "/*")):
            return token
        if token.startswith('"') or token.startswith("R\"") or token.startswith("'"):
            # Keep the quotes so the code still looks like a literal.
            return token[0] + re.sub(r"[^\n]", " ", token[1:-1]) + token[-1]
        return re.sub(r"[^\n]", " ", token)

    return _TOKEN.sub(blank, text)


def report(errors, what):
    for error in errors:
        print(error)
    if errors:
        print(f"{what}: {len(errors)} problem(s).", file=sys.stderr)
        return 1
    print(f"{what}: OK")
    return 0
