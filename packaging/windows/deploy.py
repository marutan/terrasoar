#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stages the Windows installation of Terra Soar for the NSIS installer.

Into a new, empty staging directory:
  - terrasoar.exe, and Qt with its plugins (windeployqt);
  - the DLLs the program and those DLLs import from vcpkg (GDAL, PROJ,
    GEOS, SQLite, GeographicLib and theirs), found with `dumpbin
    /dependents`;
  - the Microsoft C++ runtime, app-local;
  - Mesa llvmpipe as opengl32sw.dll, for --software-gl;
  - PROJ's database (share/proj/proj.db), where the program points PROJ;
  - the base pack (basepack/), the notices and licence texts (licences/,
    with each vcpkg package's copyright file).
Every DLL is then audited: it must come from vcpkg (by vcpkg's own lists of
installed files), Qt, the C++ runtime or Mesa; anything else fails.

Also writes the NSIS include that removes exactly the staged files on
uninstall (--uninstall-list).

Run in a Visual Studio developer environment (dumpbin, VCToolsRedistDir).

Usage:
  deploy.py --exe build\\app\\terrasoar.exe --qt-bin %QT_ROOT_DIR%\\bin
            --vcpkg vcpkg_installed\\x64-windows --mesa mesa-dir
            --basepack basepack --stage stage --uninstall-list uninstall.nsh
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MESA = ("opengl32sw.dll", "libgallium_wgl.dll")


def run(command):
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def dependents(path):
    """The DLL names `path` imports."""
    output = subprocess.run(["dumpbin", "/nologo", "/dependents", path],
                            check=True, capture_output=True, text=True).stdout
    return [line.strip() for line in output.splitlines()
            if line.strip().lower().endswith(".dll")]


def vcpkg_owners(vcpkg):
    """DLL file name (lower case) -> vcpkg package, from vcpkg's lists of
    installed files (installed/vcpkg/info/<package>_<version>_<triplet>.list)."""
    triplet = os.path.basename(os.path.normpath(vcpkg))
    info = os.path.join(os.path.dirname(os.path.normpath(vcpkg)), "vcpkg",
                        "info")
    owners = {}
    for listing in glob.glob(os.path.join(info, "*.list")):
        package = os.path.basename(listing).split("_")[0]
        with open(listing, encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line.lower().startswith(f"{triplet.lower()}/bin/") and \
                        line.lower().endswith(".dll"):
                    owners[os.path.basename(line).lower()] = package
    return owners


def deploy_vcpkg_dlls(stage, vcpkg):
    """Copies the vcpkg DLLs the program needs, following their imports."""
    vcpkg_bin = os.path.join(vcpkg, "bin")
    available = {name.lower(): name for name in os.listdir(vcpkg_bin)
                 if name.lower().endswith(".dll")}
    pending = [os.path.join(stage, "terrasoar.exe")]
    copied = set()
    while pending:
        for name in dependents(pending.pop()):
            key = name.lower()
            if key in available and key not in copied:
                copied.add(key)
                target = os.path.join(stage, available[key])
                shutil.copyfile(os.path.join(vcpkg_bin, available[key]),
                                target)
                pending.append(target)
    return copied


def copy_crt(stage):
    redist = os.environ.get("VCToolsRedistDir")
    if not redist:
        sys.exit("VCToolsRedistDir is not set: run in a developer environment")
    folders = glob.glob(os.path.join(redist, "x64", "Microsoft.VC*.CRT"))
    if not folders:
        sys.exit(f"No C++ runtime under {redist}")
    names = set()
    for path in glob.glob(os.path.join(folders[0], "*.dll")):
        shutil.copyfile(path, os.path.join(stage, os.path.basename(path)))
        names.add(os.path.basename(path).lower())
    return names


def copy_licences(stage, vcpkg, packages):
    licences = os.path.join(stage, "licences")
    os.makedirs(licences)
    for name in ("THIRD_PARTY_NOTICES", "COPYING", "CHANGELOG.md"):
        shutil.copyfile(os.path.join(ROOT, name), os.path.join(stage, name))
    for text in os.listdir(os.path.join(ROOT, "packaging", "licences")):
        shutil.copyfile(os.path.join(ROOT, "packaging", "licences", text),
                        os.path.join(licences, text))
    for package in sorted(packages):
        source = os.path.join(vcpkg, "share", package, "copyright")
        if not os.path.exists(source):
            sys.exit(f"vcpkg package {package} has no copyright file")
        target = os.path.join(licences, "vcpkg", package)
        os.makedirs(target, exist_ok=True)
        shutil.copyfile(source, os.path.join(target, "copyright"))


def audit(stage, qt_bin, owners, crt):
    qt_root = os.path.dirname(os.path.normpath(qt_bin))
    qt_plugins = os.path.join(qt_root, "plugins")
    lines = []
    packages = set()
    problems = []
    for directory, _, files in os.walk(stage):
        for name in sorted(files):
            if not name.lower().endswith(".dll"):
                continue
            relative = os.path.relpath(os.path.join(directory, name), stage)
            key = name.lower()
            in_plugins = os.path.dirname(relative) != ""
            if key in owners and not in_plugins:
                lines.append(f"{relative}\tvcpkg: {owners[key]}")
                packages.add(owners[key])
            elif key in crt:
                lines.append(f"{relative}\tMicrosoft Visual C++ runtime "
                             "(redistributable)")
            elif key in MESA:
                lines.append(f"{relative}\tMesa llvmpipe (mesa-dist-win)")
            elif (not in_plugins and os.path.exists(os.path.join(qt_bin, name))) \
                    or (in_plugins and os.path.exists(
                        os.path.join(qt_plugins, relative))):
                lines.append(f"{relative}\tQt")
            else:
                problems.append(f"{relative}: not from vcpkg, Qt, the C++ "
                                "runtime or Mesa")
    for problem in problems:
        print(f"audit: {problem}")
    if problems:
        sys.exit(f"Licence audit of the bundled DLLs: {len(problems)} "
                 "problem(s)")
    print(f"Licence audit: {len(lines)} bundled DLLs accounted for.")
    return lines, packages


def write_uninstall_list(stage, path):
    """NSIS commands removing exactly the staged files, then the folders
    they leave empty, deepest first."""
    files = []
    folders = []
    for directory, subdirectories, names in os.walk(stage):
        relative = os.path.relpath(directory, stage)
        if relative != ".":
            folders.append(relative)
        files += [os.path.normpath(os.path.join(relative, n)) for n in names]
    with open(path, "w", encoding="utf-8") as stream:
        stream.write("; Generated by packaging/windows/deploy.py.\n")
        for name in sorted(files):
            stream.write(f'Delete "$INSTDIR\\{name}"\n')
        for folder in sorted(folders, key=lambda f: -f.count(os.sep)):
            stream.write(f'RMDir "$INSTDIR\\{os.path.normpath(folder)}"\n')


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for option in ("--exe", "--qt-bin", "--vcpkg", "--mesa", "--basepack",
                   "--stage", "--uninstall-list"):
        parser.add_argument(option, required=True)
    args = parser.parse_args(argv)
    stage = os.path.abspath(args.stage)
    if os.path.exists(stage) and os.listdir(stage):
        sys.exit(f"{stage} is not empty: stage into a new directory")
    os.makedirs(stage, exist_ok=True)
    vcpkg = os.path.abspath(args.vcpkg)

    shutil.copyfile(args.exe, os.path.join(stage, "terrasoar.exe"))
    run([os.path.join(args.qt_bin, "windeployqt.exe"), "--release",
         "--no-compiler-runtime", "--no-opengl-sw", "--no-system-d3d-compiler",
         os.path.join(stage, "terrasoar.exe")])
    deploy_vcpkg_dlls(stage, vcpkg)
    crt = copy_crt(stage)
    shutil.copyfile(os.path.join(args.mesa, "opengl32.dll"),
                    os.path.join(stage, "opengl32sw.dll"))
    shutil.copyfile(os.path.join(args.mesa, "libgallium_wgl.dll"),
                    os.path.join(stage, "libgallium_wgl.dll"))

    proj = os.path.join(stage, "share", "proj")
    os.makedirs(proj)
    for name in ("proj.db", "proj.ini"):
        source = os.path.join(vcpkg, "share", "proj", name)
        if os.path.exists(source):
            shutil.copyfile(source, os.path.join(proj, name))
    if not os.path.exists(os.path.join(proj, "proj.db")):
        sys.exit("vcpkg's PROJ has no proj.db")

    if not os.path.exists(os.path.join(args.basepack, "manifest.json")):
        sys.exit(f"No base pack in {args.basepack}")
    shutil.copytree(args.basepack, os.path.join(stage, "basepack"))

    lines, packages = audit(stage, os.path.abspath(args.qt_bin),
                            vcpkg_owners(vcpkg), crt)
    packages.add("proj")  # proj.db
    copy_licences(stage, vcpkg, packages)
    with open(os.path.join(stage, "licences", "bundled-libraries.txt"), "w",
              encoding="utf-8") as stream:
        stream.write("# DLLs bundled with Terra Soar and where they come "
                     "from. vcpkg packages'\n# copyright files are in "
                     "licences\\vcpkg\\<package>\\; Qt is LGPL v3\n"
                     "# (licences\\LGPL-3.0.txt); Mesa and LLVM: "
                     "licences\\Mesa-*, licences\\LLVM-LICENSE.txt.\n")
        stream.write("share\\proj\\proj.db\tvcpkg: proj\n")
        stream.write("\n".join(lines) + "\n")
    write_uninstall_list(stage, os.path.abspath(args.uninstall_list))
    total = sum(os.path.getsize(os.path.join(d, f))
                for d, _, names in os.walk(stage) for f in names)
    print(f"Staged {stage} ({total // (1 << 20)} MB)")


if __name__ == "__main__":
    main(sys.argv[1:])
