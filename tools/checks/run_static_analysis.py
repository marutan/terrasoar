#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs the static analysis CI runs: clang-tidy, clazy and cppcheck.

Builds the project with Qt 6 and Clang (CONFIG+=compiledb) in its own build
directory, merges the compile-database fragments, then runs each tool on our
own sources. Any finding in our own files fails the run. Findings located in
Qt or other third-party headers are listed as ignored, not counted.

Usage: run_static_analysis.py [options]

  --build-dir DIR    analysis build directory (default: build-analyze/)
  --changed [BASE]   only files changed since BASE (default origin/main),
                     plus uncommitted changes; a changed header means all
  --tools LIST       comma-separated subset of clang-tidy,clazy,cppcheck
  --report-dir DIR   also write <tool>.txt reports there (CI artefacts)
  --self-test        check that each tool still reports the known-bad code
                     in tests/static_analysis/ (see the README there)
  --no-build         reuse the existing analysis build
  --build-only       only build and write compile_commands.json
  -j N               parallel jobs (default: number of CPUs)

Tools are found on PATH, or from these environment variables:
  QMAKE              qmake for Qt 6 (default: qmake6, then qmake)
  CLANG_TIDY         clang-tidy (default: clang-tidy-18, then clang-tidy)
  CLAZY              clazy-standalone (default: clazy-standalone)
  CLAZY_RESOURCE_DIR Clang resource directory for clazy's own LLVM version,
                     if its built-in headers aren't installed where it expects
  CPPCHECK           cppcheck (default: cppcheck)

docs/coding-standards.md explains the policy and how to suppress a finding.
"""

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCE_DIRS = ("core", "app", "tools", "tests")
HEADER_EXTENSIONS = (".h", ".hpp")
TOOLS = ("clang-tidy", "clazy", "cppcheck")

# clazy has no configuration file, so its checks are set here. Everything up
# to level 2, except (each with its reason, also in docs/coding-standards.md):
#   qstring-ref  suggests midRef() and friends, which Qt 6 removed; the code
#                must build with Qt 5.15 and Qt 6
CLAZY_CHECKS = "level0,level1,level2,no-qstring-ref"
# Extra exceptions for test code:
#   ctor-missing-parent-argument  QTEST_MAIN creates the test object; it never
#                                 has a parent
CLAZY_TEST_CHECKS = ",no-ctor-missing-parent-argument"

# cppcheck settings (from the Phase 0.5 plan). cppcheck gets our own include
# directories only, not Qt's or the build tree's: it knows Qt through
# --library=qt, and Qt's real headers and moc's generated files #error
# without the compiler's predefined macros and Qt's internals.
# Suppressions in our code are inline only (--inline-suppr).
CPPCHECK_ARGS = [
    "--enable=warning,performance,portability",
    "--inline-suppr",
    "--std=c++17",
    "--library=qt",
    "--suppress=missingIncludeSystem",
    "--template={file}:{line}:{column}: {severity}: {message} [{id}]",
    "--quiet",
]

# What each tool must report for tests/static_analysis/known_bad.cpp.
SELF_TEST_FIXTURE = os.path.join("tests", "static_analysis", "known_bad.cpp")
SELF_TEST_EXPECTED = {
    "clang-tidy": {
        "clang-diagnostic-unused-variable",   # unused variable (-Wall)
        "clang-diagnostic-shadow",            # shadowed name (-Wshadow)
        "cppcoreguidelines-pro-type-member-init",  # uninitialised member
    },
    "clazy": {
        "clazy-detaching-temporary",          # Qt container detach
    },
    "cppcheck": {
        "uninitMemberVar",                    # uninitialised member
    },
}

# Findings that mean a tool could not analyse a file. They count wherever
# they are, even in a third-party header, so a file that failed to parse can't
# pass as "0 findings".
ALWAYS_COUNT = {"clang-diagnostic-error", "compile-error",
                "preprocessorErrorDirective", "syntaxError", "internalError",
                "internalAstError", "cppcheckError", "unknownMacro"}
# A compiler error without a [check] tag, as clazy prints them.
BARE_ERROR = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<column>\d+): "
    r"(?:fatal )?error: (?P<message>.*)$")

DIAGNOSTIC = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<level>warning|error|style|performance|portability): "
    r"(?P<message>.*?) \[(?P<check>[^\]]+)\]$")


def find_tool(variable, candidates):
    configured = os.environ.get(variable)
    if configured:
        return configured
    for name in candidates:
        if shutil.which(name):
            return name
    return None


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def tool_version(command):
    try:
        out = run([command, "--version"]).stdout.strip()
    except OSError:
        return "not found"
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    return lines[0] if lines else "unknown version"


def is_ours(path, build_dir):
    """True for a file in our source directories, not in a build tree."""
    path = os.path.realpath(path)
    if path.startswith(os.path.realpath(build_dir) + os.sep):
        return False
    relative = os.path.relpath(path, ROOT)
    if relative.startswith(".."):
        return False
    parts = relative.replace("\\", "/").split("/")
    # Build trees inside the repo (build/, build-*/) are not sources.
    directories = parts[:-1]
    return (parts[0] in SOURCE_DIRS and
            not any(d == "build" or d.startswith("build-") for d in directories))


# --- Build and compile database ---------------------------------------------

def configure_and_build(build_dir, jobs):
    qmake = find_tool("QMAKE", ["qmake6", "qmake"])
    if not qmake:
        sys.exit("qmake not found; set QMAKE to Qt 6's qmake.")
    os.makedirs(build_dir, exist_ok=True)
    # qmake uses the nearest .qmake.stash above the build directory; under
    # another build (`make analyze`) that one holds the other compiler's
    # settings. An empty stash here keeps this build self-contained.
    stash = os.path.join(build_dir, ".qmake.stash")
    if not os.path.exists(stash):
        open(stash, "w", encoding="utf-8").close()
    print(f"Configuring {build_dir} (Qt 6, Clang, CONFIG+=compiledb)", flush=True)
    configure = run([qmake, "-spec", "linux-clang", os.path.join(ROOT, "terrasoar.pro"),
                     "CONFIG+=compiledb"], cwd=build_dir)
    if configure.returncode != 0:
        sys.exit(configure.stdout + configure.stderr + "\nqmake failed.")
    print("Building (the tools need moc's generated files)", flush=True)
    build = run(["make", f"-j{jobs}"], cwd=build_dir)
    if build.returncode != 0:
        sys.exit(build.stdout[-4000:] + build.stderr[-4000:] + "\nBuild failed.")


def compile_database(build_dir):
    """Merges the -MJ fragments into compile_commands.json; returns entries
    for our own sources."""
    entries = {}
    fragments = glob.glob(os.path.join(build_dir, "**", "*.o.json"), recursive=True)
    for fragment in fragments:
        with open(fragment, encoding="utf-8") as handle:
            text = handle.read().strip().rstrip(",")
        if not text:
            continue
        entry = json.loads(text)
        # With the build inside the source tree, qmake passes sources
        # relative to the build directory ("../../core/src/x.cpp"). Make them
        # absolute, in the arguments too, so the entry stands on its own.
        source = entry["file"]
        if not os.path.isabs(source):
            absolute = os.path.normpath(os.path.join(entry["directory"], source))
            entry["arguments"] = [absolute if argument == source else argument
                                  for argument in entry["arguments"]]
            entry["file"] = absolute
        if is_ours(entry["file"], build_dir):
            entries[os.path.realpath(entry["file"])] = entry
    if not fragments:
        sys.exit(f"No compile-database fragments in {build_dir}; "
                 "was it built with CONFIG+=compiledb?")
    if not entries:
        sys.exit(f"{len(fragments)} compile-database fragment(s) in {build_dir}, "
                 "but none for our own sources.")
    ordered = [entries[key] for key in sorted(entries)]
    with open(os.path.join(build_dir, "compile_commands.json"), "w",
              encoding="utf-8") as handle:
        json.dump(ordered, handle, indent=1)
    return ordered


def changed_files(base):
    """Files changed since base...HEAD, plus uncommitted changes."""
    names = set()
    for command in (["git", "diff", "--name-only", "--diff-filter=ACMR", f"{base}...HEAD"],
                    ["git", "diff", "--name-only", "--diff-filter=ACMR", "HEAD"]):
        result = run(command, cwd=ROOT)
        if result.returncode != 0:
            sys.exit(f"git diff against {base} failed:\n{result.stderr}")
        names.update(result.stdout.split())
    return {os.path.realpath(os.path.join(ROOT, name)) for name in names}


# --- Tools --------------------------------------------------------------------

def diagnostics(output):
    """Yields (match, check) for each diagnostic line in a tool's output."""
    for line in output.splitlines():
        line = line.strip()
        match = DIAGNOSTIC.match(line)
        if match:
            checks = match.group("check").replace(",-warnings-as-errors", "")
            # clazy prints its checks as compiler flags: -Wclazy-name.
            checks = ",".join(c[2:] if c.startswith("-W") else c
                              for c in checks.split(","))
            yield match, checks
            continue
        match = BARE_ERROR.match(line)
        if match:
            yield match, "compile-error"


def parse(output, build_dir, findings, ignored):
    for match, check in diagnostics(output):
        path = match.group("file")
        if not os.path.isabs(path):
            path = os.path.join(ROOT, path)
        ours = is_ours(path, build_dir) or bool(
            ALWAYS_COUNT.intersection(check.split(",")))
        path = os.path.realpath(path)
        shown = os.path.relpath(path, ROOT) if ours else path
        text = (f"{shown}:{match.group('line')}:{match.group('column')}: "
                f"{match.group('message')} [{check}]")
        (findings if ours else ignored).add(text)


def run_parallel(function, files, jobs):
    outputs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for output in pool.map(function, files):
            outputs.append(output)
    return outputs


def run_clang_tidy(build_dir, files, jobs):
    tidy = find_tool("CLANG_TIDY", ["clang-tidy-18", "clang-tidy"])
    if not tidy:
        return None, "clang-tidy not found (set CLANG_TIDY)"

    def one(path):
        result = run([tidy, "-p", build_dir, "--quiet", path])
        return result.stdout + result.stderr

    return run_parallel(one, files, jobs), tidy


def run_clazy(build_dir, files, jobs):
    clazy = find_tool("CLAZY", ["clazy-standalone"])
    if not clazy:
        return None, "clazy-standalone not found (set CLAZY)"
    extra = []
    resource_dir = os.environ.get("CLAZY_RESOURCE_DIR")
    if resource_dir:
        extra.append(f"-extra-arg=-resource-dir={resource_dir}")

    def one(path):
        checks = CLAZY_CHECKS
        if os.path.relpath(path, ROOT).startswith("tests" + os.sep):
            checks += CLAZY_TEST_CHECKS
        result = run([clazy, "-p", build_dir, f"-checks={checks}",
                      "-header-filter=/(" + "|".join(SOURCE_DIRS) + ")/",
                      "-ignore-dirs=^/usr/", *extra, path])
        return result.stdout + result.stderr

    return run_parallel(one, files, jobs), clazy


def run_cppcheck(build_dir, files, jobs):
    cppcheck = find_tool("CPPCHECK", ["cppcheck"])
    if not cppcheck:
        return None, "cppcheck not found (set CPPCHECK)"
    cache = os.path.join(build_dir, "cppcheck-cache")
    os.makedirs(cache, exist_ok=True)
    database = os.path.join(build_dir, "cppcheck_compile_commands.json")
    with open(os.path.join(build_dir, "compile_commands.json"),
              encoding="utf-8") as handle:
        entries = json.load(handle)
    for entry in entries:
        entry["arguments"] = own_includes_only(entry["arguments"],
                                               entry["directory"], build_dir)
    with open(database, "w", encoding="utf-8") as handle:
        json.dump(entries, handle, indent=1)
    command = [cppcheck, f"--project={database}",
               f"--cppcheck-build-dir={cache}", f"-j{jobs}", *CPPCHECK_ARGS]
    command += [f"--file-filter={path}" for path in files]
    result = run(command, cwd=ROOT)
    return [result.stdout + result.stderr], cppcheck


def own_includes_only(arguments, directory, build_dir):
    """Keeps only our own include directories (in the repo, not the build
    tree) in a compile command, for cppcheck. Qt and the other libraries can
    be anywhere (/usr, or Qt's own install directory in CI), so anything
    else is dropped, including every -isystem. The build tree is dropped
    too: moc's generated files there need Qt's own headers (Qt 6.8's #error
    without qtmochelpers.h), so their #include is left unresolved, which
    cppcheck handles."""
    root = os.path.realpath(ROOT) + os.sep
    build = os.path.realpath(build_dir) + os.sep

    def own(path):
        path = os.path.realpath(os.path.join(directory, path)) + os.sep
        return path.startswith(root) and not path.startswith(build)

    kept = []
    skip = False
    for index, argument in enumerate(arguments):
        if skip:
            skip = False
            continue
        if argument in ("-isystem", "-I"):
            skip = True
            if argument == "-I" and index + 1 < len(arguments) and \
                    own(arguments[index + 1]):
                kept += [argument, arguments[index + 1]]
            continue
        if argument.startswith("-isystem"):
            continue
        if argument.startswith("-I"):
            if own(argument[2:]):
                kept.append(argument)
            continue
        kept.append(argument)
    return kept


RUNNERS = {"clang-tidy": run_clang_tidy, "clazy": run_clazy, "cppcheck": run_cppcheck}


def analyse(build_dir, files, tools, jobs):
    """Returns {tool: (findings, ignored, problem)}."""
    results = {}
    for tool in tools:
        print(f"--- {tool} ({len(files)} file(s))", flush=True)
        outputs, command = RUNNERS[tool](build_dir, files, jobs)
        if outputs is None:
            results[tool] = (set(), set(), command)
            continue
        print(f"    {tool_version(command)}", flush=True)
        findings, ignored = set(), set()
        for output in outputs:
            parse(output, build_dir, findings, ignored)
        results[tool] = (findings, ignored, None)
    return results


# --- Self-test ------------------------------------------------------------------

def self_test(build_dir, entries, tools, jobs):
    """Runs each tool on the known-bad fixture and checks it reports what
    SELF_TEST_EXPECTED says, so a broken config can't pass silently."""
    fixture = os.path.join(ROOT, SELF_TEST_FIXTURE)
    template = next((e for e in entries
                     if os.path.relpath(e["file"], ROOT).startswith("core" + os.sep)),
                    entries[0])
    with tempfile.TemporaryDirectory(prefix="ts-analysis-selftest-",
                                     dir=build_dir) as work:
        arguments = []
        skip = False
        for argument in template["arguments"]:
            if skip:
                skip = False
                continue
            if argument in ("-o", "-MJ"):
                skip = True
                continue
            if argument == template["file"]:
                argument = fixture
            arguments.append(argument)
        arguments += ["-o", os.path.join(work, "known_bad.o")]
        with open(os.path.join(work, "compile_commands.json"), "w",
                  encoding="utf-8") as handle:
            json.dump([{"directory": template["directory"], "file": fixture,
                        "arguments": arguments}], handle)
        failed = 0
        for tool in tools:
            outputs, command = RUNNERS[tool](work, [fixture], jobs)
            if outputs is None:
                print(f"{tool}: {command}")
                failed += 1
                continue
            reported = set()
            for output in outputs:
                for _, checks in diagnostics(output):
                    reported.update(checks.split(","))
            missing = SELF_TEST_EXPECTED[tool] - reported
            if missing:
                failed += 1
                print(f"{tool}: FAILED, no longer reports {', '.join(sorted(missing))}")
                print("\n".join(f"    {line}" for line in "".join(outputs).splitlines()[:40]))
            else:
                print(f"{tool}: OK ({', '.join(sorted(SELF_TEST_EXPECTED[tool]))})")
    print(f"{len(tools) - failed}/{len(tools)} tools passed the self-test.")
    return 1 if failed else 0


# --- Main ---------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build-analyze"))
    parser.add_argument("--changed", nargs="?", const="origin/main", metavar="BASE")
    parser.add_argument("--tools", default=",".join(TOOLS))
    parser.add_argument("--report-dir")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args(argv[1:])

    tools = [tool.strip() for tool in args.tools.split(",") if tool.strip()]
    unknown = set(tools) - set(TOOLS)
    if unknown:
        parser.error(f"unknown tool(s): {', '.join(sorted(unknown))}")
    build_dir = os.path.abspath(args.build_dir)

    if not args.no_build:
        configure_and_build(build_dir, args.jobs)
    entries = compile_database(build_dir)
    if args.build_only:
        print(f"Compile database: {len(entries)} file(s) in {build_dir}")
        return 0

    if args.self_test:
        return self_test(build_dir, entries, tools, args.jobs)

    files = [os.path.realpath(entry["file"]) for entry in entries]
    if args.changed:
        changed = changed_files(args.changed)
        if any(path.endswith(HEADER_EXTENSIONS) and is_ours(path, build_dir)
               for path in changed):
            print("A header changed: analysing every file.")
        else:
            files = [path for path in files if path in changed]
        if not files:
            print(f"No analysed source files changed since {args.changed}.")
            return 0

    results = analyse(build_dir, files, tools, args.jobs)

    total = 0
    problems = 0
    for tool, (findings, ignored, problem) in results.items():
        lines = []
        if problem:
            problems += 1
            lines.append(f"{tool}: {problem}")
        lines += sorted(findings)
        if ignored:
            lines.append(f"{tool}: {len(ignored)} finding(s) in third-party "
                         "headers ignored:")
            lines += [f"    {line}" for line in sorted(ignored)]
        lines.append(f"{tool}: {len(findings)} finding(s).")
        total += len(findings)
        print("\n".join(lines), flush=True)
        if args.report_dir:
            os.makedirs(args.report_dir, exist_ok=True)
            with open(os.path.join(args.report_dir, f"{tool}.txt"), "w",
                      encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")

    if problems:
        print(f"Static analysis: {problems} tool(s) could not run.", file=sys.stderr)
        return 1
    if total:
        print(f"Static analysis: {total} finding(s). Fix them, or suppress one "
              "inline with a reason (docs/coding-standards.md).", file=sys.stderr)
        return 1
    print("Static analysis: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
