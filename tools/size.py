#!/usr/bin/env python3
"""Measure what the engine adds to an application (docs/design/measurable-goals.md).

For each profile's build directory, the example application (ZeroGHello, which
links the whole engine statically) is measured as an application would ship
it: stripped of symbols (Windows: the PDB is separate already; Linux: a copy
stripped with llvm-strip or strip). Reported per profile: the file's size, its
sections and, on Windows, from the linker's map written next to the
executable, the bytes of code and data each library brings (zig, the Linux
toolchain, writes no map).

    python tools/size.py [--profile minimal --profile standard ...]
                         [--build-dir DIR] [--top N] [--json FILE]
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = sys.platform == "win32"
EXE = "ZeroGHello.exe" if IS_WINDOWS else "ZeroGHello"
LLVM_BIN = ROOT / "third_party" / "toolchains" / "llvm-21.1.0" / "bin"


def tool(name, fallback):
    """The pinned LLVM's tool, else one on PATH (the Linux builder has binutils'
    `fallback`, whose output and options are the same for what is used here)."""
    local = LLVM_BIN / (name + (".exe" if IS_WINDOWS else ""))
    if local.exists():
        return str(local)
    found = shutil.which(name) or shutil.which(fallback)
    if not found:
        sys.exit(f"neither {name} nor {fallback} found")
    return found


def sections(binary):
    """Section name -> size in bytes (objdump -h)."""
    out = subprocess.run([tool("llvm-objdump", "objdump"), "-h", str(binary)], capture_output=True, text=True, check=True).stdout
    result = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0].isdigit():
            try:
                result[parts[1]] = result.get(parts[1], 0) + int(parts[2], 16)
            except ValueError:
                pass
    return result


def stripped_size(binary):
    if IS_WINDOWS:
        return binary.stat().st_size
    with tempfile.TemporaryDirectory() as work:
        copy = Path(work) / binary.name
        subprocess.run([tool("llvm-strip", "strip"), "--strip-all", "-o", str(copy), str(binary)], check=True)
        return copy.stat().st_size


def windows_map_breakdown(lines):
    """lld-link's /MAP (link.exe's format): symbol lines are
    "SSSS:OOOOOOOO  name  rva+base  library:object", without sizes; a symbol's
    size is the distance to the next symbol of its section. .pdata is left out:
    most of its entries have no symbol, so the distances there would credit long
    unnamed runs to whichever entry precedes them (it is in the section sizes)."""
    section_lengths = {}
    section_names = {}
    symbols = []
    header = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})H\s+(\S+)")
    symbol = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+\S+\s+[0-9a-fA-F]{16}\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$")
    for line in lines:
        if match := header.match(line):
            section, offset, length = int(match.group(1), 16), int(match.group(2), 16), int(match.group(3), 16)
            section_lengths[section] = max(section_lengths.get(section, 0), offset + length)
            section_names.setdefault(section, match.group(4).split("$", 1)[0])
        elif match := symbol.match(line):
            source = match.group(3)
            library = source.split(":", 1)[0] if ":" in source else ("(objects)" if source.endswith(".obj") else source)
            for suffix in (".lib", ".obj"):
                if library.endswith(suffix):
                    library = library[: -len(suffix)]
            symbols.append((int(match.group(1), 16), int(match.group(2), 16), library))
    symbols.sort()
    sizes = defaultdict(int)
    for index, (section, offset, library) in enumerate(symbols):
        if section_names.get(section) == ".pdata":
            continue
        if index + 1 < len(symbols) and symbols[index + 1][0] == section:
            end = symbols[index + 1][1]
        else:
            end = section_lengths.get(section, offset)
        sizes[library] += max(0, end - offset)
    return sizes


def map_breakdown(map_file):
    """Library -> bytes in the output, from the linker's map."""
    return dict(windows_map_breakdown(map_file.read_text(errors="replace").splitlines()))


def measure(build_dir, top):
    binary = build_dir / "bin" / EXE
    if not binary.exists():
        return None
    report = {
        "binary": str(binary),
        "stripped_bytes": stripped_size(binary),
        "sections": sections(binary),
    }
    map_file = binary.with_suffix(".map")
    if IS_WINDOWS and map_file.exists():
        breakdown = map_breakdown(map_file)
        report["libraries"] = dict(sorted(breakdown.items(), key=lambda item: -item[1])[:top])
    return report


def megabytes(value):
    return f"{value / (1024 * 1024):7.2f} MB"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", action="append", help="default: minimal, standard, dev")
    parser.add_argument("--build-dir", help="a build directory, instead of build/zerog-<profile>")
    parser.add_argument("--top", type=int, default=25, help="libraries listed per profile")
    parser.add_argument("--json", help="also write the report as JSON")
    args = parser.parse_args()

    targets = [(Path(args.build_dir).name, Path(args.build_dir))] if args.build_dir else \
        [(p, ROOT / "build" / f"zerog-{p}") for p in (args.profile or ["minimal", "standard", "dev"])]
    reports = {}
    for name, build_dir in targets:
        report = measure(build_dir, args.top)
        if not report:
            print(f"{name}: no {EXE} in {build_dir}")
            continue
        reports[name] = report
        print(f"{name}: {megabytes(report['stripped_bytes'])} stripped ({report['binary']})")
        for section, size in sorted(report["sections"].items(), key=lambda item: -item[1]):
            if size >= 64 * 1024:
                print(f"    {section:<12} {megabytes(size)}")
        if "libraries" in report:
            print("  by library (linker map):")
            for library, size in report["libraries"].items():
                print(f"    {library:<28} {megabytes(size)}")
        elif IS_WINDOWS:
            print("  (no linker map next to the binary: rebuild ZeroGHello for the breakdown)")
    if args.json:
        Path(args.json).write_text(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()
