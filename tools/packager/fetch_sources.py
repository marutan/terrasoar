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

"""Downloads the sources of a data pack and checks their SHA-256.

Usage: fetch_sources.py [--config basepack.json] [--upstream] <sources-directory>

Reads the "sources" list of the pack configuration (default: basepack.json
next to this script) and makes sure each file is in <sources-directory>
with the pinned SHA-256: files already there with the right checksum are
kept, others are downloaded again. A download whose checksum differs is
deleted and the script fails, so a changed or tampered file never reaches
the pack.

Each source is downloaded from its "mirror": a release asset of
marutan/terrasoar-packs-sources, the repository that keeps every pack's
source data with where it came from and its licence. Its "url" is where the
file was first downloaded from; the packager records it in the pack.
--upstream downloads from "url" instead, to add a new source to the
repository (docs/design/packs.md, "Where the source data is kept").

A file larger than a release asset may be (2 GB; GEBCO's grid) is kept as
"parts" pieces, <mirror>.part00, .part01, …: they are downloaded in turn and
joined, and the whole file is checked.

GitHub release assets are fetched with `gh release download` when the GitHub
CLI is installed (it authenticates, so CI is not rate limited), otherwise
over plain HTTPS. See docs/tools/packager.md.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CHUNK = 1 << 20
SOURCES_REPOSITORY = "marutan/terrasoar-packs-sources"
GITHUB_ASSET = re.compile(
    r"^https://github\.com/([^/]+/[^/]+)/releases/download/([^/]+)/([^/]+)$")


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_https(url, target):
    request = urllib.request.Request(
        url, headers={"User-Agent": "terrasoar-fetch-sources"})
    with urllib.request.urlopen(request, timeout=120) as response, \
            open(target, "wb") as out:
        shutil.copyfileobj(response, out, CHUNK)


def download_github(url, target):
    match = GITHUB_ASSET.match(url)
    if not match or shutil.which("gh") is None:
        return False
    repo, tag, asset = match.groups()
    directory = os.path.dirname(target)
    subprocess.run(
        ["gh", "release", "download", tag, "--repo", repo, "--pattern", asset,
         "--dir", directory, "--clobber"],
        check=True)
    os.replace(os.path.join(directory, asset), target)
    return True


def download_parts(url, count, target):
    """Downloads <url>.part00 … and joins them into @p target."""
    with open(target, "wb") as out:
        for index in range(count):
            piece = f"{target}.part{index:02d}"
            part_url = f"{url}.part{index:02d}"
            if not download_github(part_url, piece):
                download_https(part_url, piece)
            with open(piece, "rb") as handle:
                shutil.copyfileobj(handle, out, CHUNK)
            os.remove(piece)


def mirror_problem(source):
    """Why the source's "mirror" is unusable, or None if it is fine."""
    mirror = source.get("mirror")
    if not mirror:
        return ("no \"mirror\": add the file to "
                f"{SOURCES_REPOSITORY} first (docs/design/packs.md)")
    match = GITHUB_ASSET.match(mirror)
    parts = source.get("parts", 0)
    if not isinstance(parts, int) or parts < 0 or parts == 1:
        return f"\"parts\" {parts!r} is not a number of pieces (2 or more)"
    if (not match or match.group(1) != SOURCES_REPOSITORY
            or match.group(3) != source["file"]):
        return (f"\"mirror\" {mirror} is not a release asset of "
                f"{SOURCES_REPOSITORY} named {source['file']}")
    return None


def fetch(source, directory, upstream=False):
    # Checked even when the file is already there, so a configuration
    # without its mirror fails on every machine, not only on a clean one.
    problem = None if upstream else mirror_problem(source)
    if problem:
        print(f"{source['file']}: {problem}", file=sys.stderr)
        return False
    url = source["url"] if upstream else source["mirror"]
    path = os.path.join(directory, source["file"])
    expected = source["sha256"].lower()
    if os.path.isfile(path):
        if sha256_of(path) == expected:
            print(f"{source['file']}: present, checksum OK")
            return True
        print(f"{source['file']}: checksum differs, downloading again")
    parts = 0 if upstream else int(source.get("parts", 0))
    print(f"{source['file']}: downloading {url}"
          + (f" in {parts} parts" if parts else ""))
    with tempfile.TemporaryDirectory(dir=directory) as scratch:
        partial = os.path.join(scratch, source["file"])
        if parts:
            download_parts(url, parts, partial)
        elif not download_github(url, partial):
            download_https(url, partial)
        actual = sha256_of(partial)
        if actual != expected:
            print(f"{source['file']}: SHA-256 {actual}, expected {expected}; "
                  "not kept", file=sys.stderr)
            return False
        os.replace(partial, path)
    print(f"{source['file']}: downloaded, checksum OK")
    return True


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default=os.path.join(HERE, "basepack.json"))
    parser.add_argument("--upstream", action="store_true",
                        help="download from each source's \"url\", where it "
                        "was first published, instead of its \"mirror\"")
    parser.add_argument("directory")
    args = parser.parse_args(argv)
    with open(args.config, encoding="utf-8") as handle:
        config = json.load(handle)
    os.makedirs(args.directory, exist_ok=True)
    ok = True
    for source in config["sources"]:
        try:
            ok = fetch(source, args.directory, args.upstream) and ok
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"{source['file']}: {error}", file=sys.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
