#!/usr/bin/env python3
"""Memory at rest per profile (docs/design/measurable-goals.md).

Runs ZeroGMemoryBench once per scenario (each in its own process, so nothing
carries over) for each profile's build directory and prints a table: the
process's private memory and working set (Linux: RSS and PSS too), and the
engine's own counters.

    python tools/memory.py [--profile minimal --profile standard ...]
                           [--build-dir DIR] [--scenario cpu ...] [--runs N] [--json FILE]

Each scenario runs --runs times (default 3); the table shows the median.
"""

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = sys.platform == "win32"
EXE = "ZeroGMemoryBench.exe" if IS_WINDOWS else "ZeroGMemoryBench"
SCENARIOS = ["baseline", "cpu", "texture", "window", "page"]


def run_scenario(binary, scenario):
    result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=300)
    for line in result.stdout.splitlines():
        if line.startswith("{"):
            return json.loads(line)
        if line.startswith("skipped"):
            print(f"  {line}")
            return None
    print(f"  {scenario}: no measurement\n{result.stdout}{result.stderr}", file=sys.stderr)
    return None


def megabytes(value):
    return f"{value / (1024 * 1024):7.1f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", action="append", help="default: minimal, standard, dev")
    parser.add_argument("--build-dir", help="a build directory, instead of build/zerog-<profile>")
    parser.add_argument("--scenario", action="append", choices=SCENARIOS, help="default: all")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--json", help="also write the results as JSON")
    args = parser.parse_args()

    targets = [(Path(args.build_dir).name, Path(args.build_dir))] if args.build_dir else \
        [(p, ROOT / "build" / f"zerog-{p}") for p in (args.profile or ["minimal", "standard", "dev"])]
    results = {}
    for name, build_dir in targets:
        binary = build_dir / "bin" / EXE
        if not binary.exists():
            print(f"{name}: no {EXE} in {build_dir}")
            continue
        print(f"{name}:")
        header = "  scenario    private  working set" if IS_WINDOWS else "  scenario    private      RSS      PSS"
        print(header + "  JS heap  surfaces   (MB, median of %d)" % args.runs)
        results[name] = {}
        for scenario in args.scenario or SCENARIOS:
            runs = [r for r in (run_scenario(binary, scenario) for _ in range(args.runs)) if r]
            if not runs:
                continue
            median = {key: statistics.median(r[key] for r in runs) for key in runs[0] if key != "scenario"}
            results[name][scenario] = median
            columns = [megabytes(median["private"]), megabytes(median["resident"])]
            if not IS_WINDOWS:
                columns.append(megabytes(median["pss"]))
            print(f"  {scenario:<9} " + "  ".join(columns) + f"  {megabytes(median['js_heap'])}  {megabytes(median['surfaces'])}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
