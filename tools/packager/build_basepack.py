#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the base pack into <build-dir>/basepack (`make basepack`).

Usage: build_basepack.py --build-dir <dir> [--sources <dir>] [--report <file>]

1. Fetches the sources named in basepack.json (fetch_sources.py) into
   --sources, default $TS_BASEPACK_SOURCES or <build-dir>/basepack-sources.
2. Runs the packager built in <build-dir>/tools/packager into a new
   directory next to <build-dir>/basepack.
3. Replaces <build-dir>/basepack with it only when the packager succeeded,
   so a failed build never leaves a half-written pack behind.

The application finds <build-dir>/basepack next to app/ on its own; see
docs/tools/packager.md.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def find_packager(build_dir):
    base = os.path.join(build_dir, "tools", "packager")
    names = ["terrasoar-packager", "terrasoar-packager.exe"]
    for sub in ["", "release", "debug"]:
        for name in names:
            path = os.path.join(base, sub, name)
            if os.path.isfile(path):
                return path
    return None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--sources",
                        default=os.environ.get("TS_BASEPACK_SOURCES"))
    parser.add_argument("--report")
    args = parser.parse_args(argv)
    build_dir = os.path.abspath(args.build_dir)
    sources = os.path.abspath(
        args.sources or os.path.join(build_dir, "basepack-sources"))
    config = os.path.join(HERE, "basepack.json")

    packager = find_packager(build_dir)
    if packager is None:
        print(f"No terrasoar-packager under {build_dir}/tools/packager; "
              "build the tree first.", file=sys.stderr)
        return 1

    fetch = subprocess.run(
        [sys.executable, os.path.join(HERE, "fetch_sources.py"),
         "--config", config, sources])
    if fetch.returncode != 0:
        return fetch.returncode

    target = os.path.join(build_dir, "basepack")
    staging = target + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    command = [packager, "--config", config, "--sources", sources,
               "--output", staging]
    if args.report:
        command += ["--report", os.path.abspath(args.report)]
    if subprocess.run(command).returncode != 0:
        shutil.rmtree(staging, ignore_errors=True)
        return 1
    shutil.rmtree(target, ignore_errors=True)
    os.replace(staging, target)
    print(f"Base pack ready in {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
