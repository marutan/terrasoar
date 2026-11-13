#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install test of a Linux package: installs it (Flatpak) or makes it
runnable (AppImage), starts it offline-capable with --measure, checks the
state it reports (the built-in base pack found and verified, imagery
drawn on the globe), then uninstalls it and checks that it is gone.

Run inside an X server (xvfb-run). HOME is a fresh directory, so the run
starts as on a new machine.

Usage:
  test_package.py appimage TerraSoar-<version>-x86_64.AppImage
  test_package.py flatpak TerraSoar-<version>.flatpak
"""

import json
import os
import subprocess
import sys
import tempfile

APP_ID = "io.github.marutan.TerraSoar"


def run(command, **kwargs):
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, **kwargs)


def check_state(report_path):
    with open(report_path, encoding="utf-8") as stream:
        report = json.load(stream)
    state = report.get("state", report)
    print(json.dumps(state, indent=1))
    problems = []
    if state.get("base_pack") != "ok":
        problems.append(f"base pack: {state.get('base_pack')}")
    if int(state.get("imagery_tiles_drawn", 0)) <= 0:
        problems.append("no imagery drawn")
    if state.get("view") != "globe":
        problems.append(f"view: {state.get('view')}")
    return problems


def test_appimage(path, home, env):
    os.chmod(path, 0o755)
    report = os.path.join(home, "measure.json")
    result = run([os.path.abspath(path), "--measure", report,
                  "--idle-seconds", "1"],
                 env=dict(env, APPIMAGE_EXTRACT_AND_RUN="1"), timeout=600)
    if result.returncode != 0:
        return [f"exit code {result.returncode}"]
    problems = check_state(report)
    # An AppImage is removed by deleting it; it installs nothing else.
    for name in os.listdir(home):
        if name not in (".cache", ".local", ".config", "measure.json"):
            problems.append(f"unexpected file in HOME: {name}")
    return problems


def test_flatpak(path, home, env):
    problems = []
    if run(["flatpak", "install", "--user", "--noninteractive", "-y",
            os.path.abspath(path)], env=env).returncode != 0:
        return ["flatpak install failed"]
    report = os.path.join(home, "measure.json")
    result = run(["flatpak", "run", APP_ID, "--measure", report,
                  "--idle-seconds", "1"], env=env, timeout=600)
    if result.returncode != 0:
        problems.append(f"exit code {result.returncode}")
    else:
        problems += check_state(report)
    if run(["flatpak", "uninstall", "--user", "--noninteractive", "-y",
            APP_ID], env=env).returncode != 0:
        problems.append("flatpak uninstall failed")
    if run(["flatpak", "info", "--user", APP_ID], env=env,
           capture_output=True).returncode == 0:
        problems.append("still installed after uninstalling")
    return problems


def main(argv):
    if len(argv) != 2 or argv[0] not in ("appimage", "flatpak"):
        sys.exit(__doc__)
    kind, path = argv
    with tempfile.TemporaryDirectory(prefix="terrasoar-home-") as home:
        env = dict(os.environ, HOME=home, LIBGL_ALWAYS_SOFTWARE="1",
                   QT_QPA_PLATFORM="xcb")
        env.pop("TS_BASEPACK_DIR", None)
        if kind == "flatpak":
            # The user installation in the fresh HOME needs Flathub for the
            # KDE runtime.
            env.setdefault("XDG_DATA_HOME", os.path.join(home, ".local",
                                                         "share"))
            run(["flatpak", "remote-add", "--user", "--if-not-exists",
                 "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"],
                env=env, check=True)
            problems = test_flatpak(path, home, env)
        else:
            problems = test_appimage(path, home, env)
    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"{kind} install test: {'OK' if not problems else 'FAILED'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
