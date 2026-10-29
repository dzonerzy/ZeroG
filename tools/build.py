#!/usr/bin/env python3
"""Build the ZeroG dependency sysroot and the WebKit port.

    deps        install deps/vcpkg.json into third_party/sysroot/<triplet>
    configure   run CMake on third_party/webkit into build/<port>-<profile>
    build       run Ninja in that build directory
    bundle      collect every engine object into one archive: lib/zerog-engine.{lib,a}
    linktest    link tests/link/jsc_c_api.c against that archive alone and run it
    all         deps + configure + build + bundle + linktest

The default port is ZeroG (port/overlay, applied by `tools/webkit.py prepare`);
`--port JSCOnly` builds the upstream reference port with the same layout.
On Windows the Visual Studio x64 environment is loaded automatically and the
compiler is clang-cl (upstream dropped MSVC support).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEBKIT = ROOT / "third_party" / "webkit"
VCPKG = ROOT / "third_party" / "vcpkg"
SYSROOT = ROOT / "third_party" / "sysroot"
IS_WINDOWS = sys.platform == "win32"
TRIPLET = "x64-windows-zerog" if IS_WINDOWS else "x64-linux-zerog"

# Per-profile CMake options (ZeroG.md, "Profili di build"). Only flags that
# exist for the selected port are passed; the rest arrive with WebCore.
PROFILES = {
    "minimal": {"CMAKE_BUILD_TYPE": "Release"},
    "standard": {"CMAKE_BUILD_TYPE": "Release"},
    "dev": {"CMAKE_BUILD_TYPE": "RelWithDebInfo", "DEVELOPER_MODE": "ON"},
}


def run(cmd, env=None, cwd=None):
    cmd = [str(c) for c in cmd]
    # CreateProcess searches the parent's PATH, not `env`'s: resolve the tool explicitly.
    cmd[0] = shutil.which(cmd[0], path=(env or os.environ).get("PATH")) or cmd[0]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, env=env, cwd=cwd)


def vs_environment():
    """Environment of `vcvars64.bat`, so clang-cl finds the MSVC STL, SDK and linker."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        check=True, text=True, capture_output=True).stdout.strip()
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', check=True, text=True,
                         capture_output=True, shell=True).stdout
    # Windows variable names are case-insensitive; normalize so lookups are reliable.
    env = {k.upper(): v for k, v in (line.split("=", 1) for line in out.splitlines() if "=" in line)}
    cmake_dir = Path(install) / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
    if not shutil.which("cmake", path=env["PATH"]) and cmake_dir.exists():
        env["PATH"] = f"{cmake_dir};{env['PATH']}"
    return env


def build_env():
    env = vs_environment() if IS_WINDOWS else dict(os.environ)
    if IS_WINDOWS:
        # Tools installed per-user after this shell started are only in the registry PATH.
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
            user_path = winreg.QueryValueEx(key, "Path")[0]
        env["PATH"] = env["PATH"] + ";" + os.path.expandvars(user_path)
    return env


def build_dir(args):
    if args.build_dir:
        return Path(args.build_dir)
    return ROOT / "build" / f"{args.port.lower()}-{args.profile}"


def cmd_deps(args):
    env = build_env()
    exe = VCPKG / ("vcpkg.exe" if IS_WINDOWS else "vcpkg")
    if not exe.exists():
        sys.exit("vcpkg missing: clone https://github.com/microsoft/vcpkg into third_party/vcpkg and bootstrap it")
    run([exe, "install",
         f"--x-manifest-root={ROOT / 'deps'}",
         f"--x-install-root={SYSROOT}",
         f"--overlay-triplets={ROOT / 'deps' / 'triplets'}",
         f"--triplet={TRIPLET}", f"--host-triplet={TRIPLET}",
         "--disable-metrics"], env=env)


def configure_options(args):
    prefix = SYSROOT / TRIPLET
    opts = {
        "PORT": args.port,
        "CMAKE_PREFIX_PATH": prefix.as_posix(),
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        **PROFILES[args.profile],
    }
    if args.port == "JSCOnly":
        # Reference build of the upstream port. The ZeroG port sets these itself
        # (port/overlay/Source/cmake/OptionsZeroG.cmake). Upstream refuses system
        # malloc on Windows (BPlatform.h), hence mimalloc there.
        opts["ENABLE_STATIC_JSC"] = "ON"
        opts["USE_SYSTEM_MALLOC"] = "OFF" if IS_WINDOWS else "ON"
        opts["USE_MIMALLOC"] = "ON" if IS_WINDOWS else "OFF"
    # Thin archives only work inside the build tree (OptionsCommon.cmake).
    opts["USE_THIN_ARCHIVES"] = "OFF"
    if IS_WINDOWS:
        opts.update({"CMAKE_C_COMPILER": "clang-cl", "CMAKE_CXX_COMPILER": "clang-cl"})
    else:
        opts.update({"CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++"})
    for item in args.define or []:
        key, _, value = item.partition("=")
        opts[key] = value
    return opts


def cmd_configure(args):
    out = build_dir(args)
    out.mkdir(parents=True, exist_ok=True)
    defs = [f"-D{k}={v}" for k, v in configure_options(args).items()]
    run(["cmake", "-G", "Ninja", "-S", WEBKIT, "-B", out, *defs], env=build_env())


def cmd_build(args):
    out = build_dir(args)
    start = time.monotonic()
    cmd = ["cmake", "--build", out]
    if args.jobs:
        cmd += ["--", f"-j{args.jobs}"]
    run(cmd, env=build_env())
    elapsed = time.monotonic() - start
    # Build time and artifact sizes are tracked from the start (ZeroG.md, "Tempi e cache", "Ordine di lavoro" step 4).
    libs = sorted(p for p in (out / "lib").glob("*") if p.suffix in (".lib", ".a"))
    report = {"port": args.port, "profile": args.profile, "seconds": round(elapsed),
              "artifacts": {p.name: p.stat().st_size for p in libs}}
    (out / "zerog-build-report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


# Targets whose objects make up the engine. Upstream's static libraries are not
# complete on their own: subtargets such as JavaScriptCoreJIT are OBJECT libraries
# that reach consumers only through a CMake INTERFACE link (WebKitMacros.cmake).
ENGINE_TARGETS = {"WTF", "bmalloc", "mimalloc-obj", "JavaScriptCore", "JavaScriptCoreJIT",
                  "LowLevelInterpreterLib"}


def engine_objects(out):
    objects = []
    for entry in json.loads((out / "compile_commands.json").read_text()):
        parts = Path(entry["output"].replace("\\", "/")).parts
        if "CMakeFiles" not in parts:
            continue
        target = parts[parts.index("CMakeFiles") + 1].removesuffix(".dir")
        if target in ENGINE_TARGETS and Path(entry["output"]).suffix in (".o", ".obj"):
            objects.append((out / entry["output"]).resolve())
    return sorted(set(objects))


def bundle_path(out):
    return out / "lib" / ("zerog-engine.lib" if IS_WINDOWS else "libzerog-engine.a")


def cmd_bundle(args):
    out = build_dir(args)
    objects = engine_objects(out)
    archive = bundle_path(out)
    archive.unlink(missing_ok=True)
    rsp = out / "zerog-engine.rsp"
    rsp.write_text("".join(f'"{o.as_posix()}"\n' for o in objects))
    if IS_WINDOWS:
        run(["llvm-lib", "/nologo", f"/out:{archive}", f"@{rsp}"], env=build_env())
    else:
        ar = shutil.which("llvm-ar") or shutil.which("llvm-ar-19") or "ar"
        run([ar, "rcs", archive, f"@{rsp}"])  # a real archive: thin ones only work in-tree
    print(f"{archive.name}: {len(objects)} objects, {archive.stat().st_size / 1e6:.1f} MB")


def cmd_linktest(args):
    out = build_dir(args)
    src = ROOT / "tests" / "link" / "jsc_c_api.c"
    exe = out / "bin" / ("zerog-linktest.exe" if IS_WINDOWS else "zerog-linktest")
    headers = out / "JavaScriptCore" / "Headers"
    if IS_WINDOWS:
        icu = SYSROOT / TRIPLET / "lib"
        system = ["advapi32.lib", "shell32.lib", "winmm.lib", "user32.lib", "ole32.lib"]
        crt = "/MT" if args.port == "ZeroG" else "/MD"
        run(["clang-cl", "/nologo", crt, "/O2", "/DJS_NO_EXPORT", f"/I{headers}", src,
             f"/Fe{exe}", f"/Fo{out / 'zerog-linktest.obj'}", "/link", bundle_path(out),
             icu / "icuin.lib", icu / "icuuc.lib", icu / "icudt.lib", *system], env=build_env())
    else:
        obj = out / "zerog-linktest.o"
        run(["clang", "-O2", "-DJS_NO_EXPORT", f"-I{headers}", "-c", src, "-o", obj])
        # Everything static except glibc (ZeroG.md, "Cosa resta dinamico a runtime").
        run(["clang++", obj, bundle_path(out), "-o", exe,
             "-Wl,-Bstatic", "-licui18n", "-licuuc", "-licudata", "-latomic", "-Wl,-Bdynamic",
             "-static-libstdc++", "-static-libgcc", "-lpthread", "-ldl"])
    run([exe])


def cmd_all(args):
    cmd_deps(args)
    cmd_configure(args)
    cmd_build(args)
    cmd_bundle(args)
    cmd_linktest(args)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["deps", "configure", "build", "bundle", "linktest", "all"])
    parser.add_argument("--port", default="ZeroG")
    parser.add_argument("--profile", default="minimal", choices=sorted(PROFILES))
    parser.add_argument("-D", "--define", action="append", help="extra CMake definition KEY=VALUE")
    parser.add_argument("-j", "--jobs", type=int)
    parser.add_argument("--build-dir", help="default: build/<port>-<profile>")
    args = parser.parse_args()
    {"deps": cmd_deps, "configure": cmd_configure, "build": cmd_build,
     "bundle": cmd_bundle, "linktest": cmd_linktest, "all": cmd_all}[args.command](args)


if __name__ == "__main__":
    main()
