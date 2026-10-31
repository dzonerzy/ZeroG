#!/usr/bin/env python3
"""Separate symbol files for a build (docs/design/testing-ci-upstream.md, "Symbols and crashes").

Collects the debug information of a build's programs into a symbol store that
crash reporters and debuggers look up by build identity, and leaves binaries
that ship without it:

- Windows: each program's PDB in the symstore layout that symbol servers,
  WinDbg and Visual Studio read (<name>.pdb/<GUID><age>/<name>.pdb), and the
  executable itself by its timestamp and image size
  (<name>.exe/<TIMESTAMP><SIZEOFIMAGE>/<name>.exe). PDBs are separate already.
- Linux: the debug information (line tables in release builds) moved out with
  objcopy --only-keep-debug into GDB's and debuginfod's build-id layout
  (.build-id/<xx>/<rest>.debug), and a stripped copy of each program with a
  .gnu_debuglink to it.

    python tools/symbols.py [--profile minimal] [--build-dir DIR] [--out DIR]
                            [--program ZeroGHello ...] [--check]

--check symbolizes zg_runtime_create's address in ZeroGHello back to its
function, file and line from the stored symbols.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = sys.platform == "win32"
LLVM_BIN = ROOT / "third_party" / "toolchains" / "llvm-21.1.0" / "bin"


def tool(name, fallback=None):
    local = LLVM_BIN / (name + (".exe" if IS_WINDOWS else ""))
    if local.exists():
        return str(local)
    found = shutil.which(name) or (fallback and shutil.which(fallback))
    if not found:
        sys.exit(f"{name} not found" + (f" (nor {fallback})" if fallback else ""))
    return found


def output(cmd):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, check=True).stdout


# ---- Windows ----

def pe_identity(exe):
    """The PE's TimeDateStamp and SizeOfImage, as symbol servers key executables."""
    data = exe.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    timestamp = struct.unpack_from("<I", data, pe + 8)[0]
    size_of_image = struct.unpack_from("<I", data, pe + 24 + 56)[0]
    return f"{timestamp:08X}{size_of_image:X}"


def pdb_identity(pdb):
    """GUID and age from the PDB's info stream, as symstore names its directory."""
    summary = output([tool("llvm-pdbutil"), "dump", "--summary", pdb])
    guid = re.search(r"GUID:\s*\{?([0-9A-Fa-f-]+)\}?", summary).group(1).replace("-", "").upper()
    age = int(re.search(r"Age:\s*(\d+)", summary).group(1))
    return f"{guid}{age:X}"


def store_windows(binary, out):
    pdb = binary.with_suffix(".pdb")
    stored = []
    if pdb.exists():
        target = out / pdb.name / pdb_identity(pdb) / pdb.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(pdb, target)
        stored.append(target)
    target = out / binary.name / pe_identity(binary) / binary.name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, target)
    stored.append(target)
    return stored


def check_windows(build_dir, out):
    exe = build_dir / "bin" / "ZeroGHello.exe"
    map_file = exe.with_suffix(".map")
    match = re.search(r"\s(zg_runtime_create)\s+([0-9a-fA-F]{16})\s", map_file.read_text(errors="replace")) if map_file.exists() else None
    if not match:
        sys.exit("no ZeroGHello.map with zg_runtime_create: rebuild ZeroGHello")
    address = int(match.group(2), 16)
    # The stored PDB must be the one the executable names (same GUID and age),
    # and it must resolve the address to a function, file and line.
    directory = output([tool("llvm-readobj"), "--coff-debug-directory", exe])
    guid = re.search(r"PDBGUID:\s*\{?([0-9A-Fa-f-]+)\}?", directory).group(1).replace("-", "").upper()
    age = int(re.search(r"PDBAge:\s*(\d+)", directory).group(1))
    stored = out / "ZeroGHello.pdb" / f"{guid}{age:X}" / "ZeroGHello.pdb"
    if not stored.exists():
        sys.exit(f"FAIL the store has no PDB for ZeroGHello.exe's GUID and age ({stored})")
    # llvm-symbolizer finds a PDB by the path the executable records (the build
    # directory's): the stored one must be that same file.
    if stored.read_bytes() != exe.with_suffix(".pdb").read_bytes():
        sys.exit("FAIL the stored PDB differs from the one ZeroGHello.exe names")
    return output([tool("llvm-symbolizer"), f"--obj={exe}", f"0x{address:x}"])


# ---- Linux ----

def build_id(binary):
    notes = output([tool("llvm-readelf", "readelf"), "-n", binary])
    match = re.search(r"Build ID:\s*([0-9a-f]+)", notes)
    return match.group(1) if match else None


def store_linux(binary, out):
    identity = build_id(binary)
    if not identity:
        print(f"  {binary.name}: no build ID (built before --build-id), skipped")
        return []
    debug = out / ".build-id" / identity[:2] / f"{identity[2:]}.debug"
    debug.parent.mkdir(parents=True, exist_ok=True)
    objcopy = tool("llvm-objcopy", "objcopy")
    subprocess.run([objcopy, "--only-keep-debug", str(binary), str(debug)], check=True)
    stripped = out / "stripped" / binary.name
    stripped.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([objcopy, "--strip-debug", "--strip-unneeded", f"--add-gnu-debuglink={debug}", str(binary), str(stripped)], check=True)
    return [debug, stripped]


def check_linux(build_dir, out):
    binary = build_dir / "bin" / "ZeroGHello"
    symbols = output([tool("llvm-nm", "nm"), binary])
    # Local (t) in programs built with hidden visibility, global (T) otherwise.
    match = re.search(r"^([0-9a-f]+) [Tt] zg_runtime_create$", symbols, re.M)
    if not match:
        sys.exit("FAIL no zg_runtime_create in ZeroGHello's symbol table")
    identity = build_id(binary)
    debug = out / ".build-id" / identity[:2] / f"{identity[2:]}.debug"
    if not debug.exists():
        sys.exit(f"FAIL the store has no debug file for ZeroGHello's build ID ({debug})")
    symbolizer = shutil.which("llvm-symbolizer") or (str(LLVM_BIN / "llvm-symbolizer") if (LLVM_BIN / "llvm-symbolizer").exists() else None)
    if symbolizer:
        return output([symbolizer, f"--obj={debug}", f"0x{match.group(1)}"])
    return output([tool("addr2line"), "-f", "-e", debug, match.group(1)])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", default="minimal")
    parser.add_argument("--build-dir", help="default: build/zerog-<profile>")
    parser.add_argument("--out", help="default: <build dir>/symbols")
    parser.add_argument("--program", action="append", help="default: every program in bin/")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    build_dir = Path(args.build_dir) if args.build_dir else ROOT / "build" / f"zerog-{args.profile}"
    out = Path(args.out) if args.out else build_dir / "symbols"
    bin_dir = build_dir / "bin"
    suffix = ".exe" if IS_WINDOWS else ""
    if args.program:
        programs = [bin_dir / (p + suffix) for p in args.program]
    elif IS_WINDOWS:
        programs = sorted(bin_dir.glob("*.exe"))
    else:
        programs = sorted(p for p in bin_dir.iterdir() if p.is_file() and p.read_bytes()[:4] == b"\x7fELF")
    out.mkdir(parents=True, exist_ok=True)
    for program in programs:
        stored = store_windows(program, out) if IS_WINDOWS else store_linux(program, out)
        for path in stored:
            print(f"  {path.relative_to(out)}")

    if args.check:
        lines = check_windows(build_dir, out) if IS_WINDOWS else check_linux(build_dir, out)
        print(f"zg_runtime_create symbolized from the store:\n{lines.strip()}")
        if "zg_runtime_create" not in lines or "API.cpp" not in lines:
            sys.exit("FAIL the stored symbols do not resolve zg_runtime_create to API.cpp")
        print("PASS")


if __name__ == "__main__":
    main()
