#!/usr/bin/env python3
"""Fetch and check the pinned toolchain described in toolchain.json.

    fetch          download and verify the pinned LLVM for Windows into
                   third_party/toolchains/llvm-<version>
    docker-build   build the Linux builder image with the manifest's zig,
                   glibc target and vcpkg commit as build arguments
    check          verify that the toolchain in use matches the manifest
                   (exit status 1 on mismatch)

tools/build.py calls check() before every step, so a build never silently
picks up a different compiler (e.g. the clang-cl bundled with Visual Studio).
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
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAINS = ROOT / "third_party" / "toolchains"
IS_WINDOWS = sys.platform == "win32"


def manifest():
    return json.loads((ROOT / "toolchain.json").read_text())


def llvm_root():
    return TOOLCHAINS / f"llvm-{manifest()['llvm']['version']}"


def llvm_bin():
    """Directory holding the pinned clang-cl, lld-link and llvm-lib (Windows)."""
    return llvm_root() / "bin"


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cmd_fetch(args):
    if not IS_WINDOWS:
        sys.exit("fetch is for Windows; Linux uses the Docker builder (docker-build)")
    spec = manifest()["llvm"]["windows-x86_64"]
    if (llvm_bin() / "clang-cl.exe").exists() and not args.force:
        print(f"{llvm_root().name} already present")
        return
    downloads = TOOLCHAINS / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / spec["url"].rsplit("/", 1)[1]
    if not archive.exists() or sha256(archive) != spec["sha256"]:
        print(f"downloading {spec['url']}")
        tmp = archive.with_suffix(".part")
        urllib.request.urlretrieve(spec["url"], tmp)
        tmp.replace(archive)
    actual = sha256(archive)
    if actual != spec["sha256"]:
        sys.exit(f"{archive.name}: sha256 {actual} does not match the manifest")
    staging = TOOLCHAINS / f"{llvm_root().name}.extract"
    shutil.rmtree(staging, ignore_errors=True)
    print(f"extracting {archive.name}")
    with tarfile.open(archive) as tar:
        tar.extractall(staging, filter="data")
    (top,) = list(staging.iterdir())
    shutil.rmtree(llvm_root(), ignore_errors=True)
    top.rename(llvm_root())
    staging.rmdir()
    print(f"installed {llvm_root()}")


def tool_version(exe, pattern=r"clang version (\S+)"):
    out = subprocess.run([str(exe), "--version"], text=True, capture_output=True).stdout
    match = re.search(pattern, out)
    return match.group(1) if match else None


def problems():
    """Mismatches between the toolchain in use and the manifest."""
    m = manifest()
    found = []
    baseline = json.loads((ROOT / "deps" / "vcpkg.json").read_text()).get("builtin-baseline")
    if baseline != m["vcpkg"]["commit"]:
        found.append(f"deps/vcpkg.json builtin-baseline {baseline} != manifest vcpkg {m['vcpkg']['commit']}")
    vcpkg_root = Path(os.environ.get("ZEROG_VCPKG_ROOT", ROOT / "third_party" / "vcpkg"))
    if (vcpkg_root / ".git").exists():
        head = subprocess.run(["git", "-C", str(vcpkg_root), "rev-parse", "HEAD"],
                              text=True, capture_output=True).stdout.strip()
        if head != m["vcpkg"]["commit"]:
            found.append(f"vcpkg at {vcpkg_root} is {head}, manifest wants {m['vcpkg']['commit']}")
    if IS_WINDOWS:
        clang = llvm_bin() / "clang-cl.exe"
        if not clang.exists():
            found.append(f"pinned LLVM missing: run `python tools/toolchain.py fetch`")
        elif tool_version(clang) != m["llvm"]["version"]:
            found.append(f"{clang} is {tool_version(clang)}, manifest wants {m['llvm']['version']}")
    elif os.environ.get("ZEROG_ZIG_TARGET"):
        zig = shutil.which("zig")
        version = subprocess.run([zig, "version"], text=True, capture_output=True).stdout.strip() if zig else None
        if version != m["zig"]["version"]:
            found.append(f"zig is {version}, manifest wants {m['zig']['version']}")
        clang = tool_version(shutil.which("zcc") or "zcc")
        if clang != m["zig"]["clang_version"]:
            found.append(f"zig's clang is {clang}, manifest wants {m['zig']['clang_version']}")
        if os.environ["ZEROG_ZIG_TARGET"] != m["linux"]["zig_target"]:
            found.append(f"ZEROG_ZIG_TARGET is {os.environ['ZEROG_ZIG_TARGET']}, "
                         f"manifest wants {m['linux']['zig_target']}")
    else:
        found.append("Linux builds run in the Docker builder (python tools/toolchain.py docker-build)")
    return found


def check():
    found = problems()
    if found:
        sys.exit("toolchain does not match toolchain.json:\n  " + "\n  ".join(found))


def cmd_check(args):
    check()
    m = manifest()
    print(f"toolchain ok: LLVM {m['llvm']['version']}, zig {m['zig']['version']}, "
          f"vcpkg {m['vcpkg']['commit'][:12]}")


def cmd_docker_build(args):
    m = manifest()
    spec = m["zig"]["linux-x86_64"]
    build_args = {
        "ZIG_VERSION": m["zig"]["version"],
        "ZIG_SHA256": spec["sha256"],
        "ZEROG_ZIG_TARGET": m["linux"]["zig_target"],
        "VCPKG_COMMIT": m["vcpkg"]["commit"],
    }
    cmd = ["docker", "build", "-t", args.tag, "-f", str(ROOT / "tools/docker/linux-builder.Dockerfile")]
    for key, value in build_args.items():
        cmd += ["--build-arg", f"{key}={value}"]
    cmd.append(str(ROOT / "tools/docker"))
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    fetch = sub.add_parser("fetch")
    fetch.add_argument("--force", action="store_true")
    fetch.set_defaults(func=cmd_fetch)
    docker = sub.add_parser("docker-build")
    docker.add_argument("--tag", default="zerog-linux-builder")
    docker.set_defaults(func=cmd_docker_build)
    sub.add_parser("check").set_defaults(func=cmd_check)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
