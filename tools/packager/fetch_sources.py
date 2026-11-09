#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Downloads the sources of a data pack and checks their SHA-256.

Usage: fetch_sources.py [--config basepack.json] <sources-directory>

Reads the "sources" list of the pack configuration (default: basepack.json
next to this script) and makes sure each file is in <sources-directory>
with the pinned SHA-256: files already there with the right checksum are
kept, others are downloaded again. A download whose checksum differs is
deleted and the script fails, so a changed or tampered upstream file never
reaches the pack.

GitHub release assets are fetched with `gh release download` when the GitHub
CLI is installed (it authenticates, so CI is not rate limited), otherwise
over plain HTTPS like every other source. See docs/tools/packager.md.
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


def fetch(source, directory):
    path = os.path.join(directory, source["file"])
    expected = source["sha256"].lower()
    if os.path.isfile(path):
        if sha256_of(path) == expected:
            print(f"{source['file']}: present, checksum OK")
            return True
        print(f"{source['file']}: checksum differs, downloading again")
    print(f"{source['file']}: downloading {source['url']}")
    with tempfile.TemporaryDirectory(dir=directory) as scratch:
        partial = os.path.join(scratch, source["file"])
        if not download_github(source["url"], partial):
            download_https(source["url"], partial)
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
    parser.add_argument("directory")
    args = parser.parse_args(argv)
    with open(args.config, encoding="utf-8") as handle:
        config = json.load(handle)
    os.makedirs(args.directory, exist_ok=True)
    ok = True
    for source in config["sources"]:
        try:
            ok = fetch(source, args.directory) and ok
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"{source['file']}: {error}", file=sys.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
