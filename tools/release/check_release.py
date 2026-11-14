#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks that a release's version is the same everywhere, and prints its
release notes.

  check_release.py check <tag>   fails unless the tag is v<TS_VERSION>,
                                 CHANGELOG.md has a dated section for the
                                 version, and the AppStream metadata and
                                 the installation guide name it
  check_release.py notes <version>
                                 prints the version's CHANGELOG.md section
                                 (the GitHub release's notes)

See docs/release/process.md.
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def read(relative):
    with open(os.path.join(ROOT, relative), encoding="utf-8") as stream:
        return stream.read()


def ts_version():
    match = re.search(r"^TS_VERSION\s*=\s*(\S+)", read("common.pri"), re.M)
    return match.group(1) if match else None


def changelog_section(version):
    """The heading and body of the version's CHANGELOG.md section."""
    text = read("CHANGELOG.md")
    heading = re.search(rf"^## \[{re.escape(version)}\](.*)$", text, re.M)
    if not heading:
        return None, None
    rest = text[heading.end():]
    end = re.search(r"^## |^\[[^\]]+\]: ", rest, re.M)
    body = rest[:end.start()] if end else rest
    return heading.group(1).strip(), body.strip()


def check(tag):
    problems = []
    version = ts_version()
    if version is None:
        return ["TS_VERSION not found in common.pri"]
    if tag != f"v{version}":
        problems.append(f"the tag {tag} is not v{version} (TS_VERSION in "
                        "common.pri)")
    heading, body = changelog_section(version)
    if heading is None:
        problems.append(f"CHANGELOG.md has no section [{version}]")
    else:
        if not re.search(r"\d{4}-\d{2}-\d{2}", heading):
            problems.append(f"CHANGELOG.md's [{version}] heading has no date "
                            "(## [x.y.z] – YYYY-MM-DD)")
        if not body:
            problems.append(f"CHANGELOG.md's [{version}] section is empty")
    if f"[{version}]: " not in read("CHANGELOG.md"):
        problems.append(f"CHANGELOG.md has no link for [{version}]")
    metainfo = read(
        "packaging/linux/io.github.marutan.TerraSoar.metainfo.xml")
    if not re.search(rf'<release version="{re.escape(version)}" '
                     r'date="\d{4}-\d{2}-\d{2}"', metainfo):
        problems.append(f"the AppStream metadata has no dated release "
                        f"{version}")
    if f"TerraSoar-{version}" not in read("docs/user/installation.md"):
        problems.append(f"docs/user/installation.md doesn't name the "
                        f"{version} downloads")
    return problems


def main(argv):
    if len(argv) != 2 or argv[0] not in ("check", "notes"):
        sys.exit(__doc__)
    if argv[0] == "notes":
        _, body = changelog_section(argv[1])
        if body is None:
            sys.exit(f"CHANGELOG.md has no section [{argv[1]}]")
        print(body)
        return 0
    problems = check(argv[1])
    for problem in problems:
        print(f"release check: {problem}")
    print(f"release check of {argv[1]}: "
          f"{'OK' if not problems else f'{len(problems)} problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
