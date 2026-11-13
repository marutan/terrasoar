#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Licence audit: fails if THIRD_PARTY_NOTICES misses something Terra Soar
uses or ships.

Checked:
  - every Qt module named in a QT += line of the qmake project files;
  - every library deps.pri links through pkg-config;
  - every data source of the base pack and the extra packs
    (tools/packager/basepack.json, tools/packager/packs/*.json), by the
    file name of its download.

Libraries bundled into the Windows and Linux packages are audited by the
packaging jobs against the files they actually ship
(docs/release/process.md).

Usage: check_notices.py
"""

import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NOTICES = os.path.join(ROOT, "THIRD_PARTY_NOTICES")

# qmake module name -> how the notices name it.
QT_MODULES = {
    "core": "Core",
    "gui": "Gui",
    "widgets": "Widgets",
    "network": "Network",
    "opengl": "OpenGL",
    "openglwidgets": "OpenGLWidgets",
    "testlib": "Test",
}
# Variables the project files use for modules that differ between Qt 5 and 6.
QT_VARIABLES = {
    "$$TS_QT_OPENGL": "opengl",
    "$$TS_QT_OPENGLWIDGETS": "openglwidgets",
}
# pkg-config name -> how the notices name the library.
LIBRARIES = {
    "gdal": "GDAL",
    "proj": "PROJ",
    "geos": "GEOS",
    "sqlite3": "SQLite",
    "geographiclib": "GeographicLib",
}


def project_files():
    files = []
    for pattern in ("**/*.pro", "**/*.pri"):
        files += glob.glob(os.path.join(ROOT, pattern), recursive=True)
    return [f for f in files
            if not os.path.relpath(f, ROOT).startswith(("build", "."))]


def qt_modules():
    modules = set()
    line_re = re.compile(r"^\s*QT\s*\+=\s*(.*)$")
    for path in project_files():
        with open(path, encoding="utf-8") as stream:
            for line in stream:
                match = line_re.match(line)
                if not match:
                    continue
                for word in match.group(1).split():
                    if word.startswith("#"):
                        break
                    modules.add(QT_VARIABLES.get(word, word))
    return modules


def pkgconfig_libraries():
    with open(os.path.join(ROOT, "deps.pri"), encoding="utf-8") as stream:
        text = stream.read()
    match = re.search(r"PKGCONFIG\s*\+=\s*(.*)", text)
    return match.group(1).split() if match else []


def data_sources():
    configs = [os.path.join(ROOT, "tools", "packager", "basepack.json")]
    configs += sorted(glob.glob(
        os.path.join(ROOT, "tools", "packager", "packs", "*.json")))
    sources = []
    for path in configs:
        with open(path, encoding="utf-8") as stream:
            config = json.load(stream)
        for source in config.get("sources", []):
            name = source["url"].rstrip("/").rsplit("/", 1)[-1]
            sources.append((os.path.relpath(path, ROOT), name))
    return sources


def main():
    with open(NOTICES, encoding="utf-8") as stream:
        notices = stream.read()
    qt_line = next((line for line in notices.splitlines()
                    if line.startswith("Qt ")), "")
    errors = []
    for module in sorted(qt_modules()):
        name = QT_MODULES.get(module)
        if name is None:
            errors.append(f"Qt module '{module}' has no notices name "
                          "(add it to QT_MODULES in check_notices.py)")
        elif not re.search(rf"\b{re.escape(name)}\b", qt_line):
            errors.append(f"THIRD_PARTY_NOTICES: Qt {name} is used but not "
                          "listed on the Qt line")
    for library in pkgconfig_libraries():
        name = LIBRARIES.get(library)
        if name is None:
            errors.append(f"library '{library}' has no notices name "
                          "(add it to LIBRARIES in check_notices.py)")
        elif name not in notices:
            errors.append(f"THIRD_PARTY_NOTICES: {name} is linked but not "
                          "listed")
    for config, file_name in data_sources():
        if file_name not in notices:
            errors.append(f"THIRD_PARTY_NOTICES: the source {file_name} of "
                          f"{config} is not named")
    for error in errors:
        print(error)
    print(f"licence audit: {'OK' if not errors else f'{len(errors)} problem(s)'}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
