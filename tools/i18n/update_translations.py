#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keeps translations/terrasoar_en.ts in step with the sources.

  update_translations.py           run lupdate, then fill in English
  update_translations.py --check   fail if the .ts file is out of date

English is the source language, so after lupdate every new entry gets its
source text as its translation and is marked finished. Plural (%n) entries
need their English singular and plural forms typed in by hand (Qt Linguist
or a text editor); until then they stay unfinished and --check fails.

--check runs lupdate on a temporary copy and fails if:
  * a translatable string in the code is missing from the committed file
    (someone forgot `make lupdate`),
  * the committed file has entries the code no longer uses, or
  * any entry is still unfinished.

The lupdate binary is taken from $LUPDATE, else from the Qt that `qmake`
($QMAKE) reports, else from PATH.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TS_FILE = os.path.join(ROOT, "translations", "terrasoar_en.ts")
SOURCE_DIRS = ["core", "app"]
HEADER = '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n'


def find_lupdate():
    if os.environ.get("LUPDATE"):
        return os.environ["LUPDATE"]
    qmake = os.environ.get("QMAKE", "qmake")
    try:
        bins = subprocess.run([qmake, "-query", "QT_INSTALL_BINS"], check=True,
                              capture_output=True, text=True).stdout.strip()
        for name in ("lupdate", "lupdate.exe"):
            candidate = os.path.join(bins, name)
            if os.path.exists(candidate):
                return candidate
    except (OSError, subprocess.CalledProcessError):
        pass
    found = shutil.which("lupdate")
    if not found:
        sys.exit("lupdate not found; set LUPDATE or QMAKE.")
    return found


def run_lupdate(ts_path):
    args = [find_lupdate(), "-silent", "-no-obsolete", "-locations", "none",
            "-source-language", "en", "-target-language", "en",
            "-extensions", "h,cpp", *SOURCE_DIRS, "-ts", ts_path]
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"lupdate failed:\n{result.stdout}{result.stderr}")
    # lupdate reports strings it could not parse on stderr.
    problems = [line for line in result.stderr.splitlines()
                if "warning" in line.lower() or "error" in line.lower()]
    return problems


def message_key(context, message):
    source = message.findtext("source") or ""
    comment = message.findtext("comment") or ""
    return (context.findtext("name") or "", source, comment)


def entries(path):
    tree = ET.parse(path)
    result = {}
    for context in tree.getroot().iter("context"):
        for message in context.iter("message"):
            translation = message.find("translation")
            unfinished = translation is None or \
                translation.get("type") in ("unfinished", "vanished", "obsolete")
            result[message_key(context, message)] = unfinished
    return result


def fill_english(path):
    tree = ET.parse(path)
    root = tree.getroot()
    plural_todo = []
    for context in root.iter("context"):
        for message in context.iter("message"):
            translation = message.find("translation")
            if translation is None or translation.get("type") != "unfinished":
                continue
            if message.get("numerus") == "yes":
                forms = translation.findall("numerusform")
                if forms and all((f.text or "").strip() for f in forms):
                    del translation.attrib["type"]
                else:
                    plural_todo.append(message_key(context, message))
                continue
            translation.text = message.findtext("source")
            del translation.attrib["type"]
    ET.indent(tree, space="    ")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER)
        handle.write(ET.tostring(root, encoding="unicode"))
        handle.write("\n")
    return plural_todo


def check():
    failures = []
    committed = entries(TS_FILE)
    with tempfile.TemporaryDirectory() as tmp:
        generated_path = os.path.join(tmp, "terrasoar_en.ts")
        shutil.copyfile(TS_FILE, generated_path)
        problems = run_lupdate(generated_path)
        generated = entries(generated_path)
    for problem in problems:
        failures.append(f"lupdate: {problem}")
    for key in sorted(set(generated) - set(committed)):
        failures.append(f"new string not in terrasoar_en.ts: {key[0]}: {key[1]!r}")
    for key in sorted(set(committed) - set(generated)):
        failures.append(f"terrasoar_en.ts has an unused string: {key[0]}: {key[1]!r}")
    for key, unfinished in sorted(committed.items()):
        if unfinished:
            failures.append(f"unfinished entry: {key[0]}: {key[1]!r}")
    for failure in failures:
        print(failure)
    if failures:
        print("Translation check: FAILED. Run `make lupdate` (or "
              "tools/i18n/update_translations.py) and commit "
              "translations/terrasoar_en.ts.", file=sys.stderr)
        return 1
    print(f"Translation check: OK ({len(committed)} strings)")
    return 0


def main(argv):
    if "--check" in argv:
        return check()
    problems = run_lupdate(TS_FILE)
    for problem in problems:
        print(problem, file=sys.stderr)
    plural_todo = fill_english(TS_FILE)
    for key in plural_todo:
        print(f"fill in the English plural forms: {key[0]}: {key[1]!r}")
    print(f"Updated {os.path.relpath(TS_FILE, ROOT)} ({len(entries(TS_FILE))} strings).")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
