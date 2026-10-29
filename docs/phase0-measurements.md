# Phase 0 measurements

Upstream JSCOnly port at `WEBKIT_REVISION` (31a83a39), static WTF + JavaScriptCore,
profile `minimal` (Release). Machine: 32 threads, 64 GB RAM. Builds use `-j30`.

| | Windows x64 | Linux x86_64 |
| --- | --- | --- |
| Toolchain | clang-cl 19.1.7, VS 2022 SDK, Ninja | clang 19.1.7 (Debian trixie Docker image) |
| Allocator | mimalloc (vendored; system malloc is rejected upstream) | system malloc |
| ICU | 79.1, static, vcpkg sysroot | 76, system shared (to become static) |
| Clean build | 158 s | 163 s |
| `jsc` binary | 69.5 MB (with debug info) | 37.3 MB (not stripped) |
| Runtime deps of `jsc` | system DLLs + MSVC runtime (`/MD`, forced by `OptionsMSVC.cmake`) | glibc, ICU, libstdc++, libgcc_s, libatomic |

Smoke test (both platforms): JIT loop, `Intl.NumberFormat("it-IT")`,
`Intl.DateTimeFormat` with time zones, `WebAssembly` present.

## Engine archive and link test

`tools/build.py bundle` collects every engine object (WTF, bmalloc/mimalloc,
JavaScriptCore including the JavaScriptCoreJIT subtarget, LLInt) into one
archive; `tools/build.py linktest` links `tests/link/jsc_c_api.c` against it
alone (plus ICU and system libraries) and runs JS with a native callback.

| | Windows x64 | Linux x86_64 |
| --- | --- | --- |
| Archive | `zerog-engine.lib`, 537 objects, 752 MB (CodeView debug info inside objects) | `libzerog-engine.a`, 532 objects, 95.9 MB (no debug info) |
| Link test | passes | passes |
| Link test binary | static ICU included | 34.1 MB stripped, ICU shared |

### Where the size goes (`jsc`)

| | Windows `jsc.exe` 69.5 MB | Linux `jsc` 31.3 MB stripped |
| --- | --- | --- |
| Code (`.text`) | 30.2 MB | ~30.8 MB |
| Read-only data | 38.7 MB, of which ICU data ~33.5 MB (`icudt.lib`) | ICU not included (shared) |
| Debug info | separate `jsc.pdb`, 343 MB | none |

ICU data is about half of the Windows binary: the ICU data filter (ZeroG.md,
"ICU e dimensioni del binario") is the first size lever. JavaScriptCore with all
JIT tiers is ~30 MB of code; the minimal profile turns WebAssembly off.

## Findings

- Upstream static libraries are incomplete outside the WebKit tree:
  `libJavaScriptCore.a` lacks the `JavaScriptCoreJIT` objects, which reach
  consumers only through a CMake `INTERFACE` link (`WebKitMacros.cmake:437`).
  On Linux they are also thin archives (`USE_THIN_ARCHIVES` defaults on). The
  ZeroG bundle step is therefore required, not optional.
- On Windows JSC symbols are compiled `dllexport`, so a statically linked exe
  exports them (the link test produces an `.exp`). Symbol localization
  (ZeroG.md, "Simboli nel link statico") must handle this.

## Gaps to the single-binary goal

- Windows: static CRT needs a patch (`CMAKE_MSVC_RUNTIME_LIBRARY` is forced to `MultiThreadedDLL`).
- Linux: static ICU (with data filter) and a static C++ runtime.
- Windows: symbols exported from the final executable (`dllexport`).
