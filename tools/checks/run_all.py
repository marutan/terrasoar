#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs every style check CI runs: clang-format, include order, `auto`,
translatable strings, translation file up to date, and Doxygen warnings.

Usage: run_all.py [--skip-doxygen] [--skip-translations]
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def main(argv):
    checks = [
        [sys.executable, os.path.join(HERE, "check_format.py")],
        [sys.executable, os.path.join(HERE, "check_include_order.py")],
        [sys.executable, os.path.join(HERE, "check_no_auto.py")],
        [sys.executable, os.path.join(HERE, "check_translatable.py")],
        [sys.executable, os.path.join(HERE, "check_notices.py")],
    ]
    if "--skip-translations" not in argv:
        checks.append([sys.executable,
                       os.path.join(ROOT, "tools", "i18n", "update_translations.py"),
                       "--check"])
    if "--skip-doxygen" not in argv:
        checks.append([sys.executable, os.path.join(HERE, "check_doxygen.py")])
    failed = 0
    for check in checks:
        print(f"--- {os.path.basename(check[1])}", flush=True)
        if subprocess.run(check, cwd=ROOT).returncode != 0:
            failed += 1
    print(f"{len(checks) - failed}/{len(checks)} checks passed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
