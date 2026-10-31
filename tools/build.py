#!/usr/bin/env python3
"""Build the ZeroG dependency sysroot and the WebKit port.

    deps        install deps/vcpkg.json into third_party/sysroot/<triplet>
    configure   run CMake on third_party/webkit into build/<port>-<profile>
    build       run Ninja in that build directory
    bundle      engine + static dependencies in one archive, lib/zerog-engine.{lib,a},
                where only port/api/exports.txt stays public (tools/bundle.py)
    linktest    link C programs against that archive and run them, including a
                host with its own ICU and mi_malloc (tests/link)
    all         deps + configure + build + bundle + linktest

The default port is ZeroG (port/overlay, applied by `tools/webkit.py prepare`);
`--port JSCOnly` builds the upstream reference port with the same layout.
On Windows the Visual Studio x64 environment is loaded automatically and the
compiler is clang-cl (upstream dropped MSVC support).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402  (tools/toolchain.py)

ROOT = Path(__file__).resolve().parent.parent
WEBKIT = ROOT / "third_party" / "webkit"
# Overridable so the Linux Docker builder can keep both on a writable volume.
VCPKG = Path(os.environ.get("ZEROG_VCPKG_ROOT", ROOT / "third_party" / "vcpkg"))
SYSROOT = Path(os.environ.get("ZEROG_SYSROOT", ROOT / "third_party" / "sysroot"))
IS_WINDOWS = sys.platform == "win32"
TRIPLET = "x64-windows-zerog" if IS_WINDOWS else "x64-linux-zerog"

# Per-profile CMake options (docs/design/build-system-static-linking.md, "Build profiles"). Only flags that
# exist for the selected port are passed; the rest arrive with WebCore.
# WebKit options set here, rather than only as port defaults in OptionsZeroG.cmake,
# are ones whose default changed: CMake keeps an option's cached value otherwise.
PROFILES = {
    "minimal": {"CMAKE_BUILD_TYPE": "Release", "ENABLE_SAMPLING_PROFILER": "OFF"},
    "standard": {"CMAKE_BUILD_TYPE": "Release", "ENABLE_SAMPLING_PROFILER": "OFF"},
    # Release engine for app development; the Web Inspector is added with WebCore.
    "dev": {"CMAKE_BUILD_TYPE": "Release"},
    # Engine work: assertions (NDEBUG unset) and full symbols.
    "debug": {"CMAKE_BUILD_TYPE": "Debug", "DEVELOPER_MODE": "ON"},
}

# ICU data profile linked by each build profile (deps/icu-data.json).
ICU_DATA = {"minimal": "minimal", "standard": "full", "dev": "full", "debug": "full"}


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
        # The pinned LLVM and native Perl (toolchain.json) go first: vcvars puts Visual
        # Studio's bundled clang-cl on PATH, and Git for Windows' MSYS Perl mangles
        # Windows paths in WebKit's code generators.
        env["PATH"] = f"{toolchain.llvm_bin()};{toolchain.perl_exe().parent};{env['PATH']}"
    return env


def build_dir(args):
    if args.build_dir:
        return Path(args.build_dir)
    return ROOT / "build" / f"{args.port.lower()}-{args.profile}{'-asan' if args.asan else ''}"


def cmd_deps(args):
    env = build_env()
    exe = VCPKG / ("vcpkg.exe" if IS_WINDOWS else "vcpkg")
    if not exe.exists():
        sys.exit("vcpkg missing: clone https://github.com/microsoft/vcpkg into third_party/vcpkg and bootstrap it")
    # vcpkg refuses cache/download locations that do not exist yet (fresh Docker volume).
    for var in ("VCPKG_DEFAULT_BINARY_CACHE", "VCPKG_DOWNLOADS"):
        if os.environ.get(var):
            Path(os.environ[var]).mkdir(parents=True, exist_ok=True)
    run([exe, "install",
         f"--x-manifest-root={ROOT / 'deps'}",
         f"--x-install-root={SYSROOT}",
         # Build logs survive the Docker builder's throwaway container this way.
         f"--x-buildtrees-root={SYSROOT / 'buildtrees'}",
         f"--overlay-triplets={ROOT / 'deps' / 'triplets'}",
         f"--overlay-ports={ROOT / 'deps' / 'ports'}",  # ZeroG changes to vcpkg ports
         f"--triplet={TRIPLET}", f"--host-triplet={TRIPLET}",
         "--disable-metrics"], env=env)

    # Filtered ICU data libraries, one per ICU data profile.
    downloads = Path(os.environ.get("VCPKG_DOWNLOADS", VCPKG / "downloads"))
    tarballs = sorted(downloads.glob("icu4c-*-sources.tgz"))
    if not tarballs:
        sys.exit("ICU source tarball not found in vcpkg downloads")
    for profile in sorted(set(ICU_DATA.values())):
        run([sys.executable, ROOT / "tools" / "icu_data.py", "--profile", profile,
             "--tools", SYSROOT / TRIPLET / "tools" / "icu" / "bin",
             "--icu-sources", tarballs[-1],
             "--work", ROOT / "build" / "icu-data" / profile if IS_WINDOWS else SYSROOT / "icu-data-work" / profile,
             "--out", SYSROOT / TRIPLET / "lib"], env=env)


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
    if args.port == "ZeroG":
        opts["ZEROG_ICU_DATA"] = ICU_DATA[args.profile]
        opts["ZEROG_PROFILE"] = args.profile
        opts["ZEROG_SOURCE_DIR"] = ROOT.as_posix()  # port/overlay/Source/PlatformZeroG.cmake
        # vcpkg's toolchain makes find_package see the sysroot the way vcpkg installed
        # it (its wrappers know names such as zs.lib for static zlib). Manifest mode
        # is off: the sysroot is built by `deps`, and WebKit has its own vcpkg.json.
        opts.update({
            "CMAKE_TOOLCHAIN_FILE": (VCPKG / "scripts" / "buildsystems" / "vcpkg.cmake").as_posix(),
            "VCPKG_TARGET_TRIPLET": TRIPLET,
            "VCPKG_INSTALLED_DIR": SYSROOT.as_posix(),
            "VCPKG_MANIFEST_MODE": "OFF",
            "VCPKG_APPLOCAL_DEPS": "OFF",
        })
    if IS_WINDOWS:
        llvm = toolchain.llvm_bin()
        opts.update({"CMAKE_C_COMPILER": (llvm / "clang-cl.exe").as_posix(),
                     "CMAKE_CXX_COMPILER": (llvm / "clang-cl.exe").as_posix(),
                     "CMAKE_LINKER": (llvm / "lld-link.exe").as_posix(),
                     "CMAKE_AR": (llvm / "llvm-lib.exe").as_posix(),
                     "PERL_EXECUTABLE": toolchain.perl_exe().as_posix(),
                     # Compiles that see all of WebCore's include directories (the
                     # embedding layer, its tests) pass Windows' 32K command-line
                     # limit with a longer build path: arguments in response files.
                     "CMAKE_NINJA_FORCE_RESPONSE_FILE": "ON"})
    else:
        # The Docker builder sets CC/CXX/AR/RANLIB to zig wrappers (glibc baseline,
        # static libc++); outside it, plain clang.
        opts.update({"CMAKE_C_COMPILER": os.environ.get("CC", "clang"),
                     "CMAKE_CXX_COMPILER": os.environ.get("CXX", "clang++")})
        if os.environ.get("AR"):
            opts.update({"CMAKE_AR": shutil.which(os.environ["AR"]),
                         "CMAKE_RANLIB": shutil.which(os.environ.get("RANLIB", "ranlib"))})
        opts["ZEROG_ZIG"] = "ON" if os.environ.get("ZEROG_ZIG_TARGET") else "OFF"
    if args.asan:
        opts["ENABLE_SANITIZERS"] = "address"  # OptionsZeroG.cmake
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
    for target in args.target or []:
        cmd += ["--target", target]
    cmd.append("--")
    if args.jobs:
        cmd.append(f"-j{args.jobs}")
    if args.keep_going:
        cmd += ["-k", "0"]  # report every failing file in one pass (port bring-up)
    run(cmd, env=build_env())
    elapsed = time.monotonic() - start
    # Build time and artifact sizes are tracked from the start (docs/design/build-system-static-linking.md, "Times and cache"; docs/design/dependencies.md, "Order of work" step 4).
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


def bundle_libs_path(out):
    """System libraries an application links with the bundle, one per line."""
    return out / "lib" / "zerog-engine.libs.txt"


# The C API test program links exactly what an application of the full engine
# needs: its link line in build.ninja lists every engine object (JavaScriptCore's
# OBJECT libraries included), every static archive and the system libraries.
API_LINK_TARGET = "ZeroGAPITest"


def link_inputs(out, name):
    """(objects, static archives, system libraries) of an executable's link."""
    exe = f"bin\\{name}.exe" if IS_WINDOWS else f"bin/{name}"
    lines = (out / "build.ninja").read_text().splitlines()
    start = next((i for i, l in enumerate(lines) if l.startswith(f"build {exe}:")), None)
    if start is None:
        sys.exit(f"{exe} not in build.ninja: build the {name} target first")
    link_libraries = ""
    for line in lines[start + 1:]:
        if not line.startswith("  "):
            break
        if line.strip().startswith("LINK_LIBRARIES ="):
            link_libraries = line.split("=", 1)[1]
    objects, archives, system = [], [], []
    for token in link_libraries.replace("$ ", "\0").split():
        token = token.replace("\0", " ")
        path = Path(token) if Path(token).is_absolute() else out / token
        if token.endswith((".obj", ".o")):
            objects.append(path.resolve())
        elif token.endswith((".lib", ".a")) and path.exists():
            if path.resolve() not in archives:
                archives.append(path.resolve())
        elif token not in system:
            system.append(token)
    return objects, archives, system


def icu_archives(icu_data):
    """Static ICU libraries for an ICU data profile, or ICU's stock data (None)."""
    lib = SYSROOT / TRIPLET / "lib"
    if IS_WINDOWS:
        data = lib / (f"icudt-{icu_data}.lib" if icu_data else "icudt.lib")
        return [lib / "icuin.lib", lib / "icuuc.lib", data]
    data = lib / (f"libicudt-{icu_data}.a" if icu_data else "libicudata.a")
    return [lib / "libicui18n.a", lib / "libicuuc.a", data]


def zig_cxx_runtime(out):
    """libc++, libc++abi and libunwind archives zig links for the pinned target, found
    by probe-linking a C++ program the way a release build links."""
    probe = out / "cxx-runtime-probe.cpp"
    probe.write_text('#include <string>\nint main() { std::string s("x"); return (int)s.size() - 1; }\n')
    env = {**os.environ, "ZIG_VERBOSE_LINK": "1"}
    result = subprocess.run([os.environ.get("CXX", "zc++"), "-O2", str(probe), "-o", str(out / "cxx-runtime-probe")],
                            text=True, capture_output=True, env=env)
    archives = {}
    for token in (result.stdout + result.stderr).split():
        name = Path(token).name
        if name in ("libc++.a", "libc++abi.a", "libunwind.a"):
            archives[name] = token
    if len(archives) != 3:
        sys.exit(f"could not find zig's C++ runtime archives (found {sorted(archives)})")
    return [archives["libc++.a"], archives["libc++abi.a"], archives["libunwind.a"]]


def cmd_bundle(args):
    """One archive with the engine and its static dependencies; only the symbols in
    port/api/exports.txt stay public (tools/bundle.py). On Linux the C++ runtime
    (zig's libc++, libc++abi, libunwind) is inside too, localized: hosts link the
    bundle with a plain C driver, and their own C++ runtime, if any, stays separate."""
    out = build_dir(args)
    rsp = out / "zerog-engine.rsp"
    if args.port == "ZeroG":
        # The whole engine: WebCore, the embedding layer, the C API and every
        # dependency, as the C API test links them.
        objects, archives, system = link_inputs(out, API_LINK_TARGET)
    else:
        objects, archives, system = engine_objects(out), icu_archives(ICU_DATA[args.profile]), []
    rsp.write_text("".join(f'"{o.as_posix()}"\n' for o in objects))
    runtime = zig_cxx_runtime(out) if not IS_WINDOWS and os.environ.get("ZEROG_ZIG_TARGET") else []
    run([sys.executable, ROOT / "tools" / "bundle.py", "--objects", rsp,
         "--archives", *archives, *runtime,
         "--exports", ROOT / "port" / "api" / "exports.txt",
         "--out", bundle_path(out), "--work", out / "bundle"], env=build_env())
    bundle_libs_path(out).write_text("".join(f"{s}\n" for s in system))


def link_c(args, out, name, sources, includes, libraries, linker="lld", cxx_runtime=True, defines=()):
    """Compile and link a C test program the way an application would. On Windows
    `linker` is "lld" (our toolchain, and zig's) or "link" (MSVC's link.exe, what
    MSVC-built hosts use)."""
    exe = out / "bin" / (f"{name}.exe" if IS_WINDOWS else name)
    if IS_WINDOWS:
        crt = "/MT" if args.port == "ZeroG" else "/MD"
        fuse = ["-fuse-ld=lld"] if linker == "lld" else []
        objects = link_objects(out, name, sources)[0].parent
        objects.mkdir(parents=True, exist_ok=True)
        run(["clang-cl", "/nologo", crt, "/O2", "/DJS_NO_EXPORT", *[f"/D{d}" for d in defines], *fuse, *[f"/I{i}" for i in includes],
             *sources, f"/Fe{exe}", f"/Fo{objects}\\", "/link", *libraries, *link_runtime()],
            env=build_env())
    else:
        cc, cxx = os.environ.get("CC", "clang"), os.environ.get("CXX", "clang++")
        objs = link_objects(out, name, sources)
        for src, obj in zip(sources, objs):
            run([cc, "-O2", "-DJS_NO_EXPORT", *[f"-D{d}" for d in defines], *[f"-I{i}" for i in includes], "-c", src, "-o", obj])
        # cxx_runtime=False links with the C driver: the bundle must bring its own C++ runtime.
        run([cxx if cxx_runtime else cc, *objs, *libraries, "-o", exe, *link_runtime()])
    return exe


def link_objects(out, name, sources):
    """The object files link_c compiles `sources` of program `name` into."""
    if IS_WINDOWS:
        return [out / "linktest" / name / f"{Path(src).stem}.obj" for src in sources]
    return [out / f"{name}-{Path(src).stem}.o" for src in sources]


def link_runtime():
    """What link_c links a program with besides its libraries."""
    if IS_WINDOWS:
        # ws2_32: JavaScriptCore's remote inspector sockets (dev and debug profiles).
        return ["advapi32.lib", "shell32.lib", "winmm.lib", "user32.lib", "ole32.lib", "ws2_32.lib"]
    if os.environ.get("ZEROG_ZIG_TARGET"):
        return ["-lpthread", "-ldl", "-lm"]  # zig links libc++ and compiler-rt statically
    # Everything static except glibc (docs/design/dependencies.md, "What stays dynamic at runtime").
    return ["-static-libstdc++", "-static-libgcc", "-Wl,-Bstatic", "-latomic", "-Wl,-Bdynamic", "-lpthread", "-ldl", "-lm"]


def check_no_exports(exe):
    """The bundle exports nothing: Windows executables get no export table."""
    if not IS_WINDOWS:
        return
    out = subprocess.run(["llvm-readobj", "--coff-exports", str(exe)], text=True,
                         capture_output=True, env=build_env()).stdout
    exports = [l for l in out.splitlines() if l.strip().startswith("Name:")]
    print(f"{'ok  ' if not exports else 'FAIL'}  {exe.name} exports {len(exports)} symbols")
    if exports:
        sys.exit(1)


def check_api_symbols(out):
    """include/zerog.h and the engine agree: the C API library defines exactly the
    functions the header declares, and the bundle keeps every one of them."""
    declared = set(re.findall(r"\bZG_API\b[^;(]*?\b(zg_\w+)\s*\(", (ROOT / "include" / "zerog.h").read_text()))
    env = build_env()
    nm = shutil.which("llvm-nm", path=env["PATH"]) or shutil.which("nm", path=env["PATH"]) or "nm"

    def defined(archive):
        listing = subprocess.run([nm, "--defined-only", "--extern-only", "--format=just-symbols", str(archive)],
                                 text=True, capture_output=True, env=env).stdout
        return {s for s in listing.split() if re.fullmatch(r"zg_\w+", s)}

    api = defined(out / "lib" / ("ZeroGAPI.lib" if IS_WINDOWS else "libZeroGAPI.a"))
    in_bundle = defined(bundle_path(out))
    problems = [f"declared, not defined: {s}" for s in sorted(declared - api)]
    problems += [f"defined, not declared: {s}" for s in sorted(api - declared)]
    problems += [f"not public in the bundle: {s}" for s in sorted(declared - in_bundle)]
    print(f"{'ok  ' if not problems else 'FAIL'}  include/zerog.h declares the {len(declared)} functions the engine defines")
    for problem in problems:
        print(f"      {problem}")
    if problems:
        sys.exit(1)


def cmd_linktest(args):
    out = build_dir(args)
    if args.port == "ZeroG":
        check_api_symbols(out)
    tests = ROOT / "tests" / "link"
    headers = out / "JavaScriptCore" / "Headers"
    include = SYSROOT / TRIPLET / "include"
    icu_data = ICU_DATA[args.profile]

    # 1. An application linking only the bundle.
    # Linked as a C program: on Linux the bundle carries its own C++ runtime.
    exe = link_c(args, out, "zerog-linktest", [tests / "jsc_c_api.c"], [headers], [bundle_path(out)],
                 cxx_runtime=bool(IS_WINDOWS or not os.environ.get("ZEROG_ZIG_TARGET")))
    check_no_exports(exe)
    run([exe, icu_data])

    # 2. A host with its own copies of the engine's dependencies (same ICU version,
    #    its own mi_malloc): no duplicate symbols, each side keeps its own.
    host = link_c(args, out, "zerog-host-conflicts", [tests / "host_conflicts.c"], [headers, include],
                  [bundle_path(out), *icu_archives(None)])
    run([host, icu_data])
    if IS_WINDOWS:
        host = link_c(args, out, "zerog-host-conflicts-msvc", [tests / "host_conflicts.c"],
                      [headers, include], [bundle_path(out), *icu_archives(None)], linker="link")
        run([host, icu_data])

    # 3. The C API test, a C program that sees only include/zerog.h, linked with the
    #    bundle and the system libraries it lists: the full engine, nothing else.
    if args.port == "ZeroG":
        system = [l for l in bundle_libs_path(out).read_text().splitlines() if l]
        assets = out / "zerog-api-test-assets.c"
        run([sys.executable, ROOT / "tools" / "assets.py", ROOT / "tests" / "api" / "assets", "-o", assets, "--c-symbol", "test_assets"])
        font = (WEBKIT / "Source" / "ThirdParty" / "skia" / "resources" / "fonts" / "ahem.ttf").as_posix()
        api = link_c(args, out, "zerog-api-bundle", [ROOT / "tests" / "api" / "api_test.c", assets], [ROOT / "include"],
                     [bundle_path(out), *system], cxx_runtime=bool(IS_WINDOWS or not os.environ.get("ZEROG_ZIG_TARGET")),
                     defines=[f'ZEROG_TEST_FONT="{font}"'])
        check_no_exports(api)
        # The window part needs a display (a desktop, or Xvfb on Linux).
        run([api, *([] if IS_WINDOWS or os.environ.get("DISPLAY") else ["--no-window"])])

        # The example of docs/design/c-api.md, built as an application builds it, run until its
        # page has called the binding.
        hello_assets = out / "zerog-hello-assets.c"
        run([sys.executable, ROOT / "tools" / "assets.py", ROOT / "examples" / "hello" / "web", "-o", hello_assets, "--c-symbol", "hello_assets"])
        hello = link_c(args, out, "zerog-hello", [ROOT / "examples" / "hello" / "main.c", hello_assets], [ROOT / "include"],
                       [bundle_path(out), *system], cxx_runtime=bool(IS_WINDOWS or not os.environ.get("ZEROG_ZIG_TARGET")))
        check_no_exports(hello)
        if IS_WINDOWS or os.environ.get("DISPLAY"):
            run([hello, "--smoke"])

        # Its relink kit (LGPL): its objects relinked with the bundle from the kit
        # alone, the result run the same way.
        kit = out / "relink-kit" / "zerog-hello"
        hello_sources = [ROOT / "examples" / "hello" / "main.c", hello_assets]
        run([sys.executable, ROOT / "tools" / "relink_kit.py", "create", "--name", "zerog-hello",
             *[a for obj in link_objects(out, "zerog-hello", hello_sources) for a in ("--object", obj)],
             "--system-libs", bundle_libs_path(out), *[f"--link-arg={a}" for a in link_runtime()],
             "--profile", args.profile, "--out", kit])
        relinked = out / "bin" / ("zerog-hello-relinked.exe" if IS_WINDOWS else "zerog-hello-relinked")
        env = build_env()
        if not IS_WINDOWS and not os.environ.get("ZEROG_ZIG_TARGET"):
            env["CC"] = os.environ.get("CXX", "clang++")  # as link_c links it: with the C++ runtime
        run([sys.executable, kit / "relink.py", "relink", kit, "--bundle", bundle_path(out), "--out", relinked], env=env)
        if IS_WINDOWS or os.environ.get("DISPLAY"):
            run([relinked, "--smoke"])

        # A host with its own signal handlers and crash handler: they stay, the
        # GC works next to them, a crash in host code reaches the host's handler.
        host_runtime = link_c(args, out, "zerog-host-runtime", [tests / "host_runtime.c"], [ROOT / "include"],
                              [bundle_path(out), *system], cxx_runtime=bool(IS_WINDOWS or not os.environ.get("ZEROG_ZIG_TARGET")))
        run([host_runtime])

    # 4. The filtered ICU data opens every converter WebCore routes to ICU.
    conv = link_c(args, out, "zerog-icu-converters", [tests / "icu_converters.c"], [include],
                  icu_archives(icu_data))
    sys.path.insert(0, str(ROOT / "tools"))
    import icu_data as icu_data_tool
    missing = json.loads((ROOT / "deps" / "icu-data.json").read_text())["converters_missing_upstream"]
    run([conv, *sorted(icu_data_tool.webcore_encodings() - set(missing))])


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
    parser.add_argument("-k", "--keep-going", action="store_true", help="keep building after failures")
    parser.add_argument("-t", "--target", action="append", help="build only these CMake targets")
    parser.add_argument("--asan", action="store_true",
                        help="AddressSanitizer build, in build/<port>-<profile>-asan")
    parser.add_argument("--build-dir", help="default: build/<port>-<profile>")
    args = parser.parse_args()
    toolchain.check()
    {"deps": cmd_deps, "configure": cmd_configure, "build": cmd_build,
     "bundle": cmd_bundle, "linktest": cmd_linktest, "all": cmd_all}[args.command](args)


if __name__ == "__main__":
    main()
