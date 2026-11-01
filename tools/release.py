#!/usr/bin/env python3
"""Release packages of the engine: what an application needs to build with
ZeroG, per OS and profile.

    python tools/release.py --profile minimal [--build-dir DIR] [--out dist]
                            [--debug-info] [--no-check]

A package (zerog-<version>-<os>-x64-<profile>.zip on Windows, .tar.xz on Linux)
holds:

    include/zerog.h                 the C API
    lib/zerog-engine.lib            the engine bundle (libzerog-engine.a on Linux)
    lib/zerog-engine.libs.txt       the system libraries to link with it
    tools/assets.py                 packs an application's frontend
    tools/relink_kit.py             the LGPL relink kit
    examples/hello/                 the example application
    LICENSE, THIRD_PARTY_NOTICES.txt
    README.md                       how to build an application with it
    VERSION.json                    version, profile, ZeroG commit, WebKit revision

The bundle is stripped of debug information unless --debug-info (a "-debug"
package: line tables, for crash reports that name the engine's functions and
lines; several times larger). The package is checked by building the example
from it with the README's commands and running it (--no-check skips it), and
its SHA-256 goes into SHA256SUMS next to it.

The bundle must be built first: tools/build.py all --profile <profile>.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = sys.platform == "win32"
OS_NAME = "windows" if IS_WINDOWS else "linux"
BUNDLE = "zerog-engine.lib" if IS_WINDOWS else "libzerog-engine.a"

sys.path.insert(0, str(ROOT / "tools"))
import build  # noqa: E402  (tools/build.py: the toolchain environment, link_runtime)

README = """# ZeroG {version} ({profile} profile, {os_title} x64)

The ZeroG engine as a static library, with its C API. Documentation:
https://dzonerzy.github.io/ZeroG/ (getting started, guides, API reference).

## Build an application

1. Pack your frontend into C source:

       python tools/assets.py path/to/dist -o app_assets.c --c-symbol app_assets

2. Compile and link with the bundle and the system libraries listed in
   `lib/zerog-engine.libs.txt`:

{link}

The example in `examples/hello` builds the same way (its frontend is
`examples/hello/web`, its symbol `hello_assets`).

## Licenses

ZeroG's own code is under the BSD 2-Clause license (`LICENSE`). The engine
contains LGPL components (WebCore, JavaScriptCore): an application that links
them statically must let its users relink it with a modified engine.
`tools/relink_kit.py` makes the kit that allows it; the source of this version
is ZeroG at commit {commit} (https://github.com/dzonerzy/ZeroG). Ship
`THIRD_PARTY_NOTICES.txt` with your application.
"""

LINK_WINDOWS = """       cl /nologo /MT /O2 /I include app.c app_assets.c /Fe:app.exe ^
          /link lib\\zerog-engine.lib <every library in lib\\zerog-engine.libs.txt> ^
          advapi32.lib shell32.lib winmm.lib user32.lib ole32.lib ws2_32.lib

   (clang-cl takes the same options; the engine uses the static C runtime, /MT.
   Link a desktop application with /SUBSYSTEM:WINDOWS so no console opens.)"""

LINK_LINUX = """       cc -O2 -I include app.c app_assets.c lib/libzerog-engine.a \\
          $(cat lib/zerog-engine.libs.txt) -lpthread -ldl -lm -o app

   (The bundle carries its own C++ runtime: a C application links with cc.
   Only glibc stays dynamic; the release builds target glibc 2.28.)"""


def header_version():
    text = (ROOT / "include" / "zerog.h").read_text()
    parts = [re.search(rf"#define ZG_VERSION_{p}\s+(\d+)", text).group(1) for p in ("MAJOR", "MINOR", "PATCH")]
    return ".".join(parts)


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()


def strip_bundle(source, target):
    """The bundle without debug information (line tables included), and
    without the address-significance tables: they name symbols by index, which
    stripping shifts, and LLD rejects such a table when it folds identical code
    (lld-link /OPT:ICF, as Zig's release builds link). Without it LLD folds
    less, never wrongly."""
    objcopy = shutil.which("llvm-objcopy", path=build.build_env()["PATH"]) or shutil.which("objcopy")
    if not objcopy:
        sys.exit("no llvm-objcopy or objcopy to strip the bundle")
    subprocess.run([objcopy, "--strip-debug", "--remove-section=.llvm_addrsig", str(source), str(target)], check=True)


def stage(args, build_dir, staging):
    lib = build_dir / "lib"
    if not (lib / BUNDLE).exists():
        sys.exit(f"no {lib / BUNDLE}: run tools/build.py all --profile {args.profile} first")
    (staging / "include").mkdir(parents=True)
    (staging / "lib").mkdir()
    (staging / "tools").mkdir()
    shutil.copy2(ROOT / "include" / "zerog.h", staging / "include")
    if args.debug_info:
        shutil.copy2(lib / BUNDLE, staging / "lib")
    else:
        strip_bundle(lib / BUNDLE, staging / "lib" / BUNDLE)
    shutil.copy2(lib / "zerog-engine.libs.txt", staging / "lib")
    for tool in ("assets.py", "relink_kit.py"):
        shutil.copy2(ROOT / "tools" / tool, staging / "tools")
    shutil.copytree(ROOT / "examples" / "hello", staging / "examples" / "hello",
                    ignore=shutil.ignore_patterns("*.o", "*.obj", "__pycache__"))
    shutil.copy2(ROOT / "LICENSE", staging)
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.txt", staging)

    commit = git("rev-parse", "HEAD")
    info = {
        "version": args.version,
        "profile": args.profile,
        "os": OS_NAME,
        "arch": "x64",
        "debug_info": bool(args.debug_info),
        "zerog_commit": commit,
        "webkit_revision": (ROOT / "WEBKIT_REVISION").read_text().strip(),
    }
    (staging / "VERSION.json").write_text(json.dumps(info, indent=2) + "\n", newline="\n")
    (staging / "README.md").write_text(README.format(
        version=args.version, profile=args.profile, os_title="Windows" if IS_WINDOWS else "Linux",
        link=LINK_WINDOWS if IS_WINDOWS else LINK_LINUX, commit=commit[:12]), newline="\n")


def check(staging):
    """Builds examples/hello from the package as its README says, and runs it."""
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        assets = work / "hello_assets.c"
        subprocess.run([sys.executable, str(staging / "tools" / "assets.py"), str(staging / "examples" / "hello" / "web"),
                        "-o", str(assets), "--c-symbol", "hello_assets"], check=True)
        system = [l for l in (staging / "lib" / "zerog-engine.libs.txt").read_text().splitlines() if l]
        exe = work / ("hello.exe" if IS_WINDOWS else "hello")
        if IS_WINDOWS:
            cmd = ["cl", "/nologo", "/MT", "/O2", f"/I{staging / 'include'}", str(staging / "examples" / "hello" / "main.c"),
                   str(assets), f"/Fe{exe}", f"/Fo{work}\\", "/link", str(staging / "lib" / BUNDLE), *system, *build.link_runtime()]
            env = build.build_env()
        else:
            cmd = [*os.environ.get("CC", "cc").split(), "-O2", f"-I{staging / 'include'}", str(staging / "examples" / "hello" / "main.c"),
                   str(assets), str(staging / "lib" / BUNDLE), *system, *build.link_runtime(), "-o", str(exe)]
            env = dict(os.environ)
        # CreateProcess searches the parent's PATH, not env's: resolve the compiler.
        cmd[0] = shutil.which(cmd[0], path=env.get("PATH")) or cmd[0]
        print("+", " ".join(cmd), flush=True)
        subprocess.run(cmd, check=True, env=env)
        if IS_WINDOWS:
            # LLD folding identical code, as Zig's release builds link: it reads
            # sections MSVC's linker ignores (the address-significance tables).
            lld = shutil.which("lld-link", path=env.get("PATH")) or "lld-link"
            lld_cmd = [lld, "/nologo", *map(str, sorted(work.glob("*.obj"))), str(staging / "lib" / BUNDLE), *system,
                       *build.link_runtime(), "/OPT:REF", "/OPT:ICF", f"/OUT:{work / 'hello-lld.exe'}"]
            print("+", " ".join(lld_cmd), flush=True)
            subprocess.run(lld_cmd, check=True, env=env)
            print("ok    it links with LLD folding identical code (Zig's release builds)")
        if IS_WINDOWS or os.environ.get("DISPLAY"):
            subprocess.run([str(exe), "--smoke"], check=True)
            print("ok    the example builds from the package and runs")
        else:
            print("ok    the example builds from the package (no display to run it)")


def archive(staging, out, name):
    out.mkdir(parents=True, exist_ok=True)
    if IS_WINDOWS:
        path = out / f"{name}.zip"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for file in sorted(staging.rglob("*")):
                if file.is_file():
                    zf.write(file, Path(name) / file.relative_to(staging))
    else:
        path = out / f"{name}.tar.xz"
        tar_path = out / f"{name}.tar"
        with tarfile.open(tar_path, "w") as tf:
            tf.add(staging, arcname=name)
        # xz with threads where available: Python's lzma is single-threaded.
        if shutil.which("xz"):
            subprocess.run(["xz", "-T0", "-6", "-f", str(tar_path)], check=True)
        else:
            import lzma
            path.write_bytes(lzma.compress(tar_path.read_bytes(), preset=6))
            tar_path.unlink()
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    sums = out / "SHA256SUMS"
    lines = [l for l in (sums.read_text().splitlines() if sums.exists() else []) if not l.endswith(f"  {path.name}")]
    sums.write_text("".join(f"{l}\n" for l in lines + [f"{digest}  {path.name}"]), newline="\n")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", default="minimal", choices=["minimal", "standard", "dev"])
    parser.add_argument("--build-dir", help="default: build/zerog-<profile>")
    parser.add_argument("--out", default=str(ROOT / "dist"))
    parser.add_argument("--version", default=header_version(), help="default: include/zerog.h's")
    parser.add_argument("--debug-info", action="store_true", help="keep the bundle's line tables (a -debug package)")
    parser.add_argument("--no-check", action="store_true")
    args = parser.parse_args()

    build_dir = Path(args.build_dir) if args.build_dir else ROOT / "build" / f"zerog-{args.profile}"
    name = f"zerog-{args.version}-{OS_NAME}-x64-{args.profile}" + ("-debug" if args.debug_info else "")
    staging_root = Path(tempfile.mkdtemp(prefix="zerog-release-"))
    try:
        staging = staging_root / name
        stage(args, build_dir, staging)
        if not args.no_check:
            check(staging)
        path = archive(staging, Path(args.out), name)
    finally:
        shutil.rmtree(staging_root, ignore_errors=True)
    print(f"{path} ({path.stat().st_size / (1024 * 1024):.0f} MB)")


if __name__ == "__main__":
    main()
