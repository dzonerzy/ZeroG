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

## ZeroG port, stage 1 (WTF + JSC, `PORT=ZeroG`)

Patch 0002 registers the port and defines `PLATFORM(ZEROG)`; `OptionsZeroG.cmake`
sets static libraries, RunLoopGeneric, the allocator per OS, no thin archives and
(Windows) the static CRT. The link test is now a single executable on both OSes:

| | Windows x64 | Linux x86_64 |
| --- | --- | --- |
| Clean build | ~160 s | 170 s |
| Link test binary | 67.0 MB | 71.1 MB stripped |
| Runtime deps | KERNEL32, ADVAPI32, SHELL32, WINMM (system only) | libc, libm, ld-linux (glibc only) |
| Static | ICU (/MT build), CRT, C++ runtime | ICU, libstdc++, libgcc, libatomic |

ICU data (~32-34 MB) is about half of either binary: the ICU data filter is next.

## ICU data filter (step 1)

`tools/icu_data.py` builds a filtered `icudt` from ICU 79.1's prebuilt data with
ICU's own tools (icupkg, genrb, genccode); `deps` produces one library per
profile in `deps/icu-data.json`, and `OptionsZeroG.cmake` links the profile's
library (`ZEROG_ICU_DATA`). Both platforms build ICU 79.1 through vcpkg at the
same baseline, so the data is identical.

| ICU data | Size | Items |
| --- | --- | --- |
| stock `icudt79l.dat` | 33.6 MB | 4321 |
| `full` (all languages, only WebCore's converters) | 28.5 MB | 4154 |
| `minimal` (17 languages) | 12.6 MB | 1516 |
| English only (measured, not a profile) | 8.5 MB | 830 |

Removed: locale bundles of other languages in every tree (res_index rewritten,
so `Intl` does not advertise them) and converters WebCore never asks ICU for.
Kept: time zones, normalization, break rules and segmentation dictionaries.
`icupkg` dependency checks restore cross-language parents (rbnf/yue for zh_Hant_HK).

Link test (`minimal`) checks Italian and Japanese formatting, that a dropped
locale is not advertised, Thai word segmentation and every WebCore encoding:

| | Windows | Linux |
| --- | --- | --- |
| `jsc` | 69.5 MB -> 49.1 MB | 67.4 MB -> 46.5 MB stripped |
| Link test `minimal` / `standard` | 46.1 / 62.0 MB | 51.7 / 67.6 MB stripped |
| Runtime deps of `jsc` | system DLLs only | glibc only (libc, libm) |

Found on the way: stock ICU 79.1 data has no ISO-8859-16 converter, so WebCore's
`iso-8859-16` codec cannot work with any stock ICU build (recorded in
`deps/icu-data.json`; the build fails if another encoding goes missing).

## glibc 2.28 baseline (step 2)

Linux builds now use zig 0.16.0 (`zig cc/c++ -target x86_64-linux-gnu.2.28`) for
everything: WebKit, vcpkg dependencies and the final link. A newer system
libstdc++ cannot be linked statically for an old baseline, since its archive
references glibc symbols newer than 2.28; zig's libc++ and compiler-rt are built
for the target. Two zig-specific settings:

- `-g0` first in the wrappers: zig emits debug info by default (the engine
  archive was 1031 MB with it, 86.6 MB without); an explicit `-g` still wins.
- `-mevex512` (`OptionsZeroG.cmake`): zig passes `-evex512` with the baseline
  features, which simdutf's `target("avx512...")` kernels cannot re-enable.

| | zig, glibc 2.28 | previous clang + libstdc++ |
| --- | --- | --- |
| Clean build | 166 s | 170 s |
| `libzerog-engine.a` | 86.6 MB | 95.9 MB |
| Link test, stripped | 48.4 MB | 51.7 MB |
| `jsc`, stripped | 47.2 MB | 46.5 MB |
| Highest glibc symbol | GLIBC_2.28 | (built against 2.41) |

The stripped binaries were run unchanged (link test, converter test, `jsc`):

| Distribution | glibc | Result |
| --- | --- | --- |
| Debian 10 (buster) | 2.28 | all checks pass |
| Rocky Linux 8 | 2.28 | all checks pass |
| Fedora (latest) | 2.43 | all checks pass |

## Gaps to the single-binary goal

- Windows: symbols exported from the final executable (`dllexport`).
