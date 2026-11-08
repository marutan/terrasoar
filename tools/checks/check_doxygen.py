#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
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
