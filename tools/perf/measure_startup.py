#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measures start-up time, memory and idle CPU of the built application and
compares them with a stored baseline.

  measure_startup.py <terrasoar> [options]

Runs `terrasoar --measure - --idle-seconds N` several times (offscreen
unless --platform is given), takes the median of each value and checks:

  * resident memory stays under the common-use budget (256 MB),
  * idle CPU stays under --max-idle-cpu percent (render on demand),
  * start-up time and memory are no more than --margin worse than the
    baseline (default 10%; time also gets --time-slack-ms of absolute slack,
    because tens of milliseconds are noisy on shared CI runners).

Options:
  --runs N              runs to take the median of (default 5)
  --idle-seconds N      idle period per run (default 10, as in the roadmap)
  --platform NAME       QT_QPA_PLATFORM to use (default offscreen)
  --baseline FILE       baseline JSON (default baselines/empty-app.json)
  --margin F            allowed regression as a fraction (default 0.10)
  --time-slack-ms MS    absolute start-up slack (default 50)
  --max-idle-cpu PCT    idle CPU limit in percent (default 1.0)
  --output FILE         write the median results here as JSON
  --update-baseline     write the results to the baseline file and exit

Exit code 0 when every check passes, 1 otherwise.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MiB = 1024 * 1024
MEMORY_BUDGET_BYTES = 256 * MiB

NUMERIC_KEYS = ("startup_ms", "startup_cpu_s", "resident_bytes",
                "peak_resident_bytes", "idle_resident_bytes",
                "idle_cpu_percent")


def run_once(app, idle_seconds, platform):
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = platform
    result = subprocess.run(
        [app, "--measure", "-", "--idle-seconds", str(idle_seconds)],
        env=env, capture_output=True, text=True, timeout=idle_seconds + 120)
    if result.returncode != 0:
        raise RuntimeError(
            f"{app} exited with {result.returncode}:\n{result.stderr}")
    return json.loads(result.stdout)


def median_results(runs):
    merged = dict(runs[0])
    for key in NUMERIC_KEYS:
        merged[key] = statistics.median(r[key] for r in runs)
    merged["runs"] = len(runs)
    return merged


def check(results, baseline, args):
    failures = []
    peak = results["peak_resident_bytes"]
    if peak > MEMORY_BUDGET_BYTES:
        failures.append(f"peak memory {peak / MiB:.1f} MB is over the "
                        f"{MEMORY_BUDGET_BYTES / MiB:.0f} MB budget")
    if results["idle_cpu_percent"] > args.max_idle_cpu:
        failures.append(f"idle CPU {results['idle_cpu_percent']:.2f}% is over "
                        f"{args.max_idle_cpu:.2f}%")
    if baseline:
        limit_ms = (baseline["startup_ms"] * (1 + args.margin)
                    + args.time_slack_ms)
        if results["startup_ms"] > limit_ms:
            failures.append(f"start-up {results['startup_ms']:.1f} ms is over "
                            f"{limit_ms:.1f} ms (baseline "
                            f"{baseline['startup_ms']:.1f} ms)")
        limit_bytes = baseline["peak_resident_bytes"] * (1 + args.margin)
        if peak > limit_bytes:
            failures.append(f"peak memory {peak / MiB:.1f} MB is over "
                            f"{limit_bytes / MiB:.1f} MB (baseline "
                            f"{baseline['peak_resident_bytes'] / MiB:.1f} MB)")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("app")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--idle-seconds", type=int, default=10)
    parser.add_argument("--platform", default="offscreen")
    parser.add_argument("--baseline",
                        default=os.path.join(HERE, "baselines",
                                             "empty-app.json"))
    parser.add_argument("--margin", type=float, default=0.10)
    parser.add_argument("--time-slack-ms", type=float, default=50.0)
    parser.add_argument("--max-idle-cpu", type=float, default=1.0)
    parser.add_argument("--output")
    parser.add_argument("--update-baseline", action="store_true")
    args = parser.parse_args()

    runs = [run_once(args.app, args.idle_seconds, args.platform)
            for _ in range(max(1, args.runs))]
    results = median_results(runs)

    print(f"start-up      {results['startup_ms']:8.1f} ms")
    print(f"peak memory   {results['peak_resident_bytes'] / MiB:8.1f} MB "
          f"(budget {MEMORY_BUDGET_BYTES / MiB:.0f} MB, "
          f"{(MEMORY_BUDGET_BYTES - results['peak_resident_bytes']) / MiB:.1f}"
          f" MB left)")
    print(f"idle CPU      {results['idle_cpu_percent']:8.3f} % "
          f"over {args.idle_seconds} s")

    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            json.dump(results, handle, indent=2, sort_keys=True)
            handle.write("\n")

    if args.update_baseline:
        with open(args.baseline, "w", encoding="utf-8") as handle:
            json.dump(results, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print(f"Baseline written to {args.baseline}.")
        return 0

    baseline = None
    if os.path.exists(args.baseline):
        with open(args.baseline, encoding="utf-8") as handle:
            baseline = json.load(handle)
    else:
        print(f"No baseline at {args.baseline}; checking budgets only.")

    failures = check(results, baseline, args)
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    print("Start-up check: " + ("FAILED" if failures else "OK"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
