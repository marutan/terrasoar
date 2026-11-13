#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the Linux AppImage, TerraSoar-<version>-x86_64.AppImage.

  1. `make install` of the built program into a fresh AppDir (PREFIX=/usr,
     with the base pack, desktop entry, icons and notices; see app/app.pro);
  2. linuxdeploy with its Qt plugin (pinned builds, checked by SHA-256)
     copies Qt, its plugins and every library the program needs into it;
  3. the bundled libraries are audited: each must come from a Debian
     package (its copyright file is shipped and listed in
     usr/share/doc/terrasoar/bundled-libraries.txt) or from the Qt in use
     (the LGPL and GPL texts are shipped); anything else fails the build;
  4. linuxdeploy packs the AppDir into the AppImage.

Usage:
  build_appimage.py --build-dir build --basepack build/basepack \\
                    --output-dir dist [--qmake /path/to/qmake]

The build directory must hold a release build (`qmake` and `make`). The
tools are downloaded into <build-dir>/appimage-tools once. AppImages run
without FUSE here (APPIMAGE_EXTRACT_AND_RUN=1). See
docs/release/packaging.md.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
APP_ID = "io.github.marutan.TerraSoar"

TOOLS = {
    "linuxdeploy-x86_64.AppImage": (
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/"
        "1-alpha-20251107-1/linuxdeploy-x86_64.AppImage",
        "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"),
    "linuxdeploy-plugin-qt-x86_64.AppImage": (
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/"
        "download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage",
        "15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"),
}


def version():
    with open(os.path.join(ROOT, "common.pri"), encoding="utf-8") as stream:
        match = re.search(r"^TS_VERSION\s*=\s*(\S+)", stream.read(), re.M)
    if not match:
        sys.exit("TS_VERSION not found in common.pri")
    return match.group(1)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_tools(directory):
    os.makedirs(directory, exist_ok=True)
    for name, (url, expected) in TOOLS.items():
        path = os.path.join(directory, name)
        if not os.path.exists(path) or sha256(path) != expected:
            print(f"Downloading {url}", flush=True)
            partial = path + ".part"
            urllib.request.urlretrieve(url, partial)
            if sha256(partial) != expected:
                os.remove(partial)
                sys.exit(f"{name}: unexpected SHA-256")
            os.replace(partial, path)
        os.chmod(path, 0o755)


def run(command, **kwargs):
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def qt_query(qmake, name):
    return subprocess.run([qmake, "-query", name], check=True,
                          capture_output=True, text=True).stdout.strip()


def owning_package(path):
    """The Debian package a library on this system comes from, or None."""
    for candidate in (path, os.path.realpath(path)):
        result = subprocess.run(["dpkg", "-S", candidate],
                                capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.split(":", 1)[0].strip()
    return None


def original_of(relative, qt_libs, qt_plugins):
    """Where linuxdeploy took the bundled file `relative` (usr/...) from."""
    parts = relative.split(os.sep)
    if "plugins" in parts:
        path = os.path.join(qt_plugins, *parts[parts.index("plugins") + 1:])
        return path if os.path.exists(path) else None
    name = parts[-1]
    for directory in (qt_libs, "/usr/lib/x86_64-linux-gnu",
                      "/lib/x86_64-linux-gnu", "/usr/lib", "/lib"):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            return path
    return None


def audit(appdir, qt_libs, qt_plugins, qt_prefix):
    """Ships the copyright of every bundled library; fails on unknown ones."""
    doc = os.path.join(appdir, "usr", "share", "doc")
    qt_root = os.path.realpath(qt_prefix) + os.sep
    lines = []
    problems = []
    for directory, _, files in sorted(os.walk(os.path.join(appdir, "usr"))):
        for name in sorted(files):
            if not re.search(r"\.so(\.\d+)*$", name):
                continue
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, appdir)
            if relative == os.path.join("usr", "bin", "terrasoar"):
                continue
            original = original_of(relative, qt_libs, qt_plugins)
            package = owning_package(original) if original else None
            if package:
                package = package.split(",")[0].split(":")[0]
                lines.append(f"{relative}\t{package}")
                copyright_file = f"/usr/share/doc/{package}/copyright"
                if not os.path.exists(copyright_file):
                    problems.append(f"{relative}: {package} has no copyright")
                    continue
                target = os.path.join(doc, package)
                os.makedirs(target, exist_ok=True)
                shutil.copyfile(copyright_file,
                                os.path.join(target, "copyright"))
            elif original and os.path.realpath(original).startswith(qt_root):
                lines.append(f"{relative}\tQt ({qt_prefix})")
            else:
                problems.append(f"{relative}: not from a package or the Qt "
                                "in use")
    licences = os.path.join(doc, "terrasoar", "licences")
    os.makedirs(licences, exist_ok=True)
    for text in ("LGPL-3.0.txt", "GPL-3.0.txt"):
        shutil.copyfile(os.path.join(ROOT, "packaging", "licences", text),
                        os.path.join(licences, text))
    with open(os.path.join(doc, "terrasoar", "bundled-libraries.txt"), "w",
              encoding="utf-8") as stream:
        stream.write("# Libraries bundled in this AppImage and where they "
                     "come from: a Debian package\n# (its copyright file is "
                     "in usr/share/doc/<package>/) or the Qt build used\n"
                     "# (LGPL v3, usr/share/doc/terrasoar/licences/).\n")
        stream.write("\n".join(lines) + "\n")
    for problem in problems:
        print(f"audit: {problem}")
    if problems:
        sys.exit(f"Licence audit of the bundled libraries: {len(problems)} "
                 "problem(s)")
    print(f"Licence audit: {len(lines)} bundled libraries accounted for.")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--basepack", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--qmake", default=os.environ.get("QMAKE", "qmake6"))
    args = parser.parse_args(argv)
    build = os.path.abspath(args.build_dir)
    basepack = os.path.abspath(args.basepack)
    output = os.path.abspath(args.output_dir)
    qmake = shutil.which(args.qmake) or args.qmake
    if not os.path.exists(os.path.join(basepack, "manifest.json")):
        sys.exit(f"No base pack in {basepack}")
    os.makedirs(output, exist_ok=True)
    tools = os.path.join(build, "appimage-tools")
    fetch_tools(tools)
    name = f"TerraSoar-{version()}-x86_64.AppImage"

    env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1", QMAKE=qmake,
               PATH=tools + os.pathsep + os.environ.get("PATH", ""))
    # The Wayland platform plugins too, where the Qt in use has them.
    qt_plugins = qt_query(qmake, "QT_INSTALL_PLUGINS")
    wayland = [p for p in ("libqwayland-egl.so", "libqwayland-generic.so")
               if os.path.exists(os.path.join(qt_plugins, "platforms", p))]
    if wayland:
        env["EXTRA_PLATFORM_PLUGINS"] = ";".join(wayland)
    env["LDAI_OUTPUT"] = env["OUTPUT"] = os.path.join(output, name)

    with tempfile.TemporaryDirectory(prefix="terrasoar-appdir-") as work:
        appdir = os.path.join(work, "AppDir")
        app_build = os.path.join(build, "app")
        run([qmake, os.path.join(ROOT, "app", "app.pro"), "PREFIX=/usr",
             f"BASEPACK_DIR={basepack}"], cwd=app_build)
        run(["make", "-j", str(os.cpu_count() or 2)], cwd=app_build)
        run(["make", "install", f"INSTALL_ROOT={appdir}"], cwd=app_build)
        linuxdeploy = os.path.join(tools, "linuxdeploy-x86_64.AppImage")
        desktop = os.path.join(appdir, "usr", "share", "applications",
                               f"{APP_ID}.desktop")
        icon = os.path.join(appdir, "usr", "share", "icons", "hicolor",
                            "256x256", "apps", f"{APP_ID}.png")
        run([linuxdeploy, "--appdir", appdir, "--plugin", "qt",
             "--desktop-file", desktop, "--icon-file", icon], env=env)
        audit(appdir, qt_query(qmake, "QT_INSTALL_LIBS"), qt_plugins,
              qt_query(qmake, "QT_INSTALL_PREFIX"))
        run([linuxdeploy, "--appdir", appdir, "--output", "appimage"],
            env=env, cwd=output)
    path = os.path.join(output, name)
    with open(path + ".sha256", "w", encoding="utf-8") as stream:
        stream.write(f"{sha256(path)}  {name}\n")
    print(f"Built {path} ({os.path.getsize(path) // (1 << 20)} MB)")


if __name__ == "__main__":
    main(sys.argv[1:])
