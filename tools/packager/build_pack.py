#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds an extra data pack and its release files and index entry.

Usage: build_pack.py --build-dir <dir> --config <packs/x.json>
                     [--sources <dir>] [--report <file>]
                     [--base-url <url>] [--index <index.json>]

1. Fetches the sources the configuration names (fetch_sources.py) into
   --sources, default $TS_BASEPACK_SOURCES or <build-dir>/basepack-sources
   (the same files as the base pack's, so they are downloaded once).
2. Runs the packager into <build-dir>/packs/<id>, replacing an earlier
   build only when the packager succeeded.
3. Writes <build-dir>/packs/<id>-release/: the pack's files under the flat
   names a GitHub release takes ("licences/gshhg/LICENSE.TXT" becomes
   "licences.gshhg.LICENSE.TXT"), and index-entry.json, the pack's entry
   for index.json (see docs/design/packs.md), whose URLs point at
   <base-url>/<id>-<version>/<asset>.
4. With --index, puts that entry into the given index.json, replacing an
   entry with the same id, and keeps the entries sorted by id.

Nothing is uploaded: publishing the release and the index is a separate,
manual step (docs/tools/packager.md#extra-packs).
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASE_URL = \
    "https://github.com/marutan/terrasoar-packs/releases/download"
INDEX_FORMAT = 1


def find_packager(build_dir):
    base = os.path.join(build_dir, "tools", "packager")
    for sub in ["", "release", "debug"]:
        for name in ["terrasoar-packager", "terrasoar-packager.exe"]:
            path = os.path.join(base, sub, name)
            if os.path.isfile(path):
                return path
    return None


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def asset_name(path):
    """The release asset name of a pack file path."""
    return path.replace("/", ".")


def write_release(pack_dir, release_dir, config, base_url):
    """Fills release_dir with the assets and returns the index entry."""
    with open(os.path.join(pack_dir, "manifest.json"),
              encoding="utf-8") as stream:
        manifest = json.load(stream)
    meta = config.get("index", {})
    tag = f"{manifest['id']}-{manifest['version']}"
    url = f"{base_url.rstrip('/')}/{tag}/"

    shutil.rmtree(release_dir, ignore_errors=True)
    os.makedirs(release_dir)
    names = {}
    total = 0
    for entry in manifest["files"]:
        name = asset_name(entry["path"])
        if name in names.values() or name == "manifest.json":
            raise SystemExit(f"Two pack files share the asset name {name}")
        names[entry["path"]] = name
        shutil.copyfile(os.path.join(pack_dir, entry["path"]),
                        os.path.join(release_dir, name))
        total += entry["bytes"]
    manifest_path = os.path.join(pack_dir, "manifest.json")
    shutil.copyfile(manifest_path,
                    os.path.join(release_dir, "manifest.json"))

    entry = {
        "id": manifest["id"],
        "version": manifest["version"],
        "name": meta.get("name", manifest["id"]),
        "description": meta.get("description", ""),
        "licence": meta.get("licence", ""),
        "bytes": total,
        "manifest": {
            "url": url + "manifest.json",
            "sha256": sha256(manifest_path),
            "bytes": os.path.getsize(manifest_path),
        },
        "files": {path: url + name for path, name in names.items()},
    }
    with open(os.path.join(release_dir, "index-entry.json"), "w",
              encoding="utf-8") as stream:
        json.dump(entry, stream, indent=2, ensure_ascii=False)
        stream.write("\n")
    return entry


def merge_index(index_path, entry):
    index = {"format": INDEX_FORMAT, "packs": []}
    if os.path.exists(index_path):
        with open(index_path, encoding="utf-8") as stream:
            index = json.load(stream)
    if index.get("format") != INDEX_FORMAT:
        raise SystemExit(f"{index_path}: format {index.get('format')}, "
                         f"this script writes {INDEX_FORMAT}")
    packs = [p for p in index.get("packs", []) if p.get("id") != entry["id"]]
    packs.append(entry)
    index["packs"] = sorted(packs, key=lambda p: p["id"])
    with open(index_path, "w", encoding="utf-8") as stream:
        json.dump(index, stream, indent=2, ensure_ascii=False)
        stream.write("\n")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--sources",
                        default=os.environ.get("TS_BASEPACK_SOURCES"))
    parser.add_argument("--report")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--index")
    args = parser.parse_args(argv)
    build_dir = os.path.abspath(args.build_dir)
    config_path = os.path.abspath(args.config)
    sources = os.path.abspath(
        args.sources or os.path.join(build_dir, "basepack-sources"))
    with open(config_path, encoding="utf-8") as stream:
        config = json.load(stream)

    packager = find_packager(build_dir)
    if packager is None:
        print(f"No terrasoar-packager under {build_dir}/tools/packager; "
              "build the tree first.", file=sys.stderr)
        return 1

    fetch = subprocess.run(
        [sys.executable, os.path.join(HERE, "fetch_sources.py"),
         "--config", config_path, sources])
    if fetch.returncode != 0:
        return fetch.returncode

    target = os.path.join(build_dir, "packs", config["id"])
    staging = target + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    command = [packager, "--config", config_path, "--sources", sources,
               "--output", staging]
    if args.report:
        command += ["--report", os.path.abspath(args.report)]
    if subprocess.run(command).returncode != 0:
        shutil.rmtree(staging, ignore_errors=True)
        return 1
    shutil.rmtree(target, ignore_errors=True)
    os.replace(staging, target)

    entry = write_release(target, target + "-release", config, args.base_url)
    if args.index:
        merge_index(os.path.abspath(args.index), entry)
    print(f"Pack ready in {target}; release files and index entry in "
          f"{target}-release ({entry['bytes']} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
