#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install, upgrade and uninstall test of the Windows installer.

  1. Installs silently into a folder with spaces in its path; checks the
     files, the Settings > Apps entry, the .terrasoar association and the
     Start menu shortcut.
  2. Starts the installed program with its own software OpenGL and PROJ's
     own places emptied (so only the installed proj.db can serve) and
     --measure: the base pack must be found and verified, imagery drawn on
     the globe, PROJ's database usable.
  3. Upgrade: with users' data in place (a data pack folder and saved
     tiles), installs again over it; the data must survive, the program
     start, and Settings > Apps hold one entry.
  4. Uninstalls silently: the folder, the entry, the association and the
     shortcut must be gone, users' data kept.

Usage: test_installer.py TerraSoar-<version>-setup.exe <version>
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import winreg

UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\TerraSoar"
SHORTCUT = os.path.join(os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
                        r"Microsoft\Windows\Start Menu\Programs\Terra Soar.lnk")
USER_DATA = os.path.join(os.environ["LOCALAPPDATA"], "Terra Soar", "terrasoar")


def run(command, **kwargs):
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, **kwargs)


def registry_value(path, name, root=winreg.HKEY_LOCAL_MACHINE):
    try:
        with winreg.OpenKey(root, path, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            return winreg.QueryValueEx(key, name)[0]
    except OSError:
        return None


def install(setup, folder):
    # /D must come last and unquoted, spaces and all: a command line of our
    # own (a list would be quoted where it has spaces, and NSIS would then
    # ignore /D and install into Program Files).
    command = f'"{setup}" /S /D={folder}'
    print("+ " + command, flush=True)
    result = subprocess.run(command, timeout=900)
    return [] if result.returncode == 0 else [
        f"installer exit code {result.returncode}"]


def check_installed(folder, version):
    problems = []
    for relative in ("terrasoar.exe", "uninstall.exe", "opengl32sw.dll",
                     r"basepack\manifest.json", r"share\proj\proj.db",
                     r"licences\bundled-libraries.txt", "THIRD_PARTY_NOTICES",
                     r"platforms\qwindows.dll"):
        if not os.path.exists(os.path.join(folder, relative)):
            problems.append(f"missing {relative}")
    if registry_value(UNINSTALL_KEY, "DisplayVersion") != version:
        problems.append("Settings > Apps: wrong or no version")
    if registry_value(UNINSTALL_KEY, "Publisher") != "Peter Howkins":
        problems.append("Settings > Apps: wrong publisher")
    location = registry_value(UNINSTALL_KEY, "InstallLocation") or ""
    if os.path.normcase(os.path.normpath(location)) != \
            os.path.normcase(os.path.normpath(folder)):
        problems.append(f"Settings > Apps: install location {location}")
    if registry_value(r"Software\Classes\.terrasoar", "") != "TerraSoar.Project":
        problems.append(".terrasoar is not associated")
    if not os.path.exists(SHORTCUT):
        problems.append("no Start menu shortcut")
    return problems


def start(folder, work):
    program = os.path.join(folder, "terrasoar.exe")
    if not os.path.exists(program):
        return [f"no {program} to start"]
    report = os.path.join(work, "measure.json")
    if os.path.exists(report):
        os.remove(report)
    no_proj = os.path.join(work, "no-proj-data")
    os.makedirs(no_proj, exist_ok=True)
    env = dict(os.environ, GALLIUM_DRIVER="llvmpipe", PROJ_DATA=no_proj,
               PROJ_LIB=no_proj)
    env.pop("TS_BASEPACK_DIR", None)
    result = run([program, "--software-gl",
                  "--measure", report, "--idle-seconds", "1"], env=env,
                 timeout=600)
    if result.returncode != 0:
        return [f"program exit code {result.returncode}"]
    with open(report, encoding="utf-8") as stream:
        data = json.load(stream)
    state = data.get("state", data)
    print(json.dumps(state, indent=1))
    problems = []
    if state.get("base_pack") != "ok":
        problems.append(f"base pack: {state.get('base_pack')}")
    if int(state.get("imagery_tiles_drawn", 0)) <= 0:
        problems.append("no imagery drawn")
    if state.get("crs_database") != "ok":
        problems.append(f"PROJ's database: {state.get('crs_database')}")
    return problems


def uninstall(folder):
    uninstaller = os.path.join(folder, "uninstall.exe")
    if not os.path.exists(uninstaller):
        return [f"no {uninstaller}"]
    # The uninstaller copies itself away and returns at once: wait for it.
    result = run([uninstaller, "/S"], timeout=300)
    if result.returncode != 0:
        return [f"uninstaller exit code {result.returncode}"]
    deadline = time.monotonic() + 120
    while os.path.exists(folder) and time.monotonic() < deadline:
        time.sleep(1)
    problems = []
    if os.path.exists(folder):
        problems.append(f"{folder} still there: {os.listdir(folder)}")
    if registry_value(UNINSTALL_KEY, "DisplayName") is not None:
        problems.append("Settings > Apps entry still there")
    if registry_value(r"Software\Classes\.terrasoar", "") is not None:
        problems.append(".terrasoar still associated")
    if os.path.exists(SHORTCUT):
        problems.append("Start menu shortcut still there")
    return problems


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__)
    setup, version = os.path.abspath(argv[0]), argv[1]
    work = tempfile.mkdtemp(prefix="terrasoar-install-test-")
    folder = os.path.join(work, "Program Files Test", "Terra Soar")
    problems = []

    print("== Install", flush=True)
    problems += install(setup, folder)
    problems += check_installed(folder, version)
    problems += start(folder, work)

    print("== Upgrade", flush=True)
    markers = [os.path.join(USER_DATA, "packs", "upgrade-test", "marker.txt"),
               os.path.join(USER_DATA, "cache", "upgrade-test.txt")]
    for marker in markers:
        os.makedirs(os.path.dirname(marker), exist_ok=True)
        with open(marker, "w", encoding="utf-8") as stream:
            stream.write("users' data\n")
    problems += install(setup, folder)
    problems += check_installed(folder, version)
    problems += start(folder, work)
    for marker in markers:
        if not os.path.exists(marker):
            problems.append(f"upgrade lost {marker}")

    print("== Uninstall", flush=True)
    problems += uninstall(folder)
    for marker in markers:
        if not os.path.exists(marker):
            problems.append(f"uninstalling removed users' data {marker}")

    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"Windows install test: {'OK' if not problems else 'FAILED'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
