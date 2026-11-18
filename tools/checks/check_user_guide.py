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

"""Checks that the user guide built into the program (Help > User Guide)
matches docs/user/ (see docs/design/help.md):

- every page in docs/user/ is in app/userguide.qrc, and every file listed
  there exists, in the same place under /guide/ as under docs/;
- every image the pages use is listed (under /guide/images/) and exists,
  and every image listed is used;
- every link between pages, and every heading anchor, resolves (anchors
  as GitHub makes them, the same as the program does);
- links out of the guide name files that exist in the repository (the
  program opens them on GitHub);
- the contents page, docs/user/README.md, links to every other page.

  check_user_guide.py
"""

import os
import re
import sys
import unicodedata
import xml.etree.ElementTree as ElementTree

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QRC = os.path.join(ROOT, "app", "userguide.qrc")
DOCS = os.path.join(ROOT, "docs")
USER = os.path.join(DOCS, "user")
PREFIX = "/guide"

# Markdown links and images: [text](target) and ![alt](target), and HTML
# <img src="...">. Code spans are removed first.
LINK = re.compile(r"(!?)\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
IMG_TAG = re.compile(r"<img\s[^>]*src=\"([^\"]+)\"", re.I)
CODE = re.compile(r"`[^`]*`")
FENCE = re.compile(r"^```.*?^```", re.M | re.S)
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$", re.M)
EXTERNAL = ("http:", "https:", "mailto:", "ftp:")


def heading_slug(heading):
    """GitHub's anchor for a heading; the program's
    userguide::headingSlug() does the same."""
    slug = []
    for ch in heading.strip().lower():
        if ch == " ":
            slug.append("-")
        elif (ch.isalnum() or ch in "-_"
              or unicodedata.category(ch).startswith("M")
              or unicodedata.category(ch) == "Pc"):
            slug.append(ch)
    return "".join(slug)


def plain_heading(text):
    """A heading as shown: inline code, emphasis and links unwrapped."""
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"[*_]{1,3}([^*_]+)[*_]{1,3}", r"\1", text)
    return text


def anchors(markdown):
    """The page's heading anchors, with GitHub's numbering of repeats."""
    result = []
    for match in HEADING.finditer(FENCE.sub("", markdown)):
        slug = heading_slug(plain_heading(match.group(2)))
        anchor, repeat = slug, 1
        while anchor in result:
            anchor = f"{slug}-{repeat}"
            repeat += 1
        result.append(anchor)
    return result


def qrc_entries():
    """{resource path under /guide: source file}, from userguide.qrc."""
    entries = {}
    tree = ElementTree.parse(QRC)
    for resource in tree.getroot().iter("qresource"):
        prefix = resource.get("prefix", "/")
        for file in resource.iter("file"):
            alias = file.get("alias") or file.text
            path = "/".join([prefix.rstrip("/"), alias])
            source = os.path.normpath(
                os.path.join(os.path.dirname(QRC), file.text))
            entries[path] = source
    return entries


def links(markdown):
    """(is_image, target) for each link on the page, outside code."""
    text = CODE.sub("", FENCE.sub("", markdown))
    for match in LINK.finditer(text):
        yield match.group(1) == "!", match.group(2)
    for match in IMG_TAG.finditer(text):
        yield True, match.group(1)


def main():
    problems = []
    entries = qrc_entries()
    for path, source in sorted(entries.items()):
        if not path.startswith(PREFIX + "/"):
            problems.append(f"userguide.qrc: {path} is outside {PREFIX}/")
            continue
        expected = os.path.normpath(
            os.path.join(DOCS, path[len(PREFIX) + 1:]))
        if source != expected:
            problems.append(f"userguide.qrc: {path} should be "
                            f"{os.path.relpath(expected, ROOT)}, not "
                            f"{os.path.relpath(source, ROOT)}")
        if not os.path.isfile(source):
            problems.append(f"userguide.qrc: {os.path.relpath(source, ROOT)}"
                            " doesn't exist")

    pages = sorted(f for f in os.listdir(USER) if f.endswith(".md"))
    for page in pages:
        if f"{PREFIX}/user/{page}" not in entries:
            problems.append(f"docs/user/{page} isn't in app/userguide.qrc "
                            "(add it, and to docs/user/README.md)")
    listed_pages = {p for p in entries if p.startswith(f"{PREFIX}/user/")
                    and p.endswith(".md")}
    listed_images = {p for p in entries if p not in listed_pages}
    texts = {}
    for page in pages:
        with open(os.path.join(USER, page), encoding="utf-8") as stream:
            texts[page] = stream.read()
    page_anchors = {page: anchors(text) for page, text in texts.items()}

    used_images = set()
    linked_from_contents = set()
    for page, text in texts.items():
        for is_image, target in links(text):
            if target.lower().startswith(EXTERNAL):
                continue
            file_part, _, fragment = target.partition("#")
            if not file_part:
                resolved = os.path.join(USER, page)
            else:
                resolved = os.path.normpath(os.path.join(USER, file_part))
            relative = os.path.relpath(resolved, DOCS).replace(os.sep, "/")
            where = f"docs/user/{page}: '{target}'"
            if is_image:
                path = f"{PREFIX}/{relative}"
                used_images.add(path)
                if path not in entries:
                    problems.append(f"{where}: the image isn't in "
                                    "app/userguide.qrc")
                elif not os.path.isfile(resolved):
                    problems.append(f"{where}: the image doesn't exist")
                continue
            inside = os.path.dirname(resolved) == USER
            if inside:
                name = os.path.basename(resolved)
                if name not in texts:
                    problems.append(f"{where}: no such page in docs/user/")
                    continue
                if page == "README.md":
                    linked_from_contents.add(name)
                if fragment and fragment not in page_anchors[name]:
                    problems.append(f"{where}: {name} has no heading with "
                                    f"the anchor '{fragment}'")
            elif not os.path.exists(resolved):
                problems.append(f"{where}: {os.path.relpath(resolved, ROOT)}"
                                " doesn't exist (links out of the guide open "
                                "the file on GitHub)")
    for image in sorted(listed_images - used_images):
        problems.append(f"userguide.qrc: {image} isn't used by any page")
    for page in pages:
        if page != "README.md" and page not in linked_from_contents:
            problems.append(f"docs/user/README.md doesn't list {page}")

    for problem in problems:
        print(f"user guide: {problem}")
    print("user guide check: "
          f"{'OK' if not problems else f'{len(problems)} problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
