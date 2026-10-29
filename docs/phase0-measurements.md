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
| `full` (all languages, only WebCore's converters) | 29.6 MB | 4161 |
| `minimal` (17 languages) | 13.7 MB | 1523 |
| English only (measured, not a profile) | 8.5 MB | 830 |

Removed: locale bundles of other languages in every tree (res_index rewritten,
so `Intl` does not advertise them) and converters WebCore never asks ICU for.
Kept converters (29): TextCodecICU's encodings, and the ones `EncodingTables.cpp`
runs at runtime to build the CJK decoders' tables (EUC-JP, Big-5, windows-949,
gb18030), followed by a `RELEASE_ASSERT`. The first version of the filter missed
those and EUC-TW; the design verification caught it (docs/verification).
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

## One pinned LLVM (toolchain.json)

The Windows build had silently used Visual Studio's bundled clang-cl 19.1.5
(vcvars puts it first on PATH), while Linux used zig's clang 21.1.0. Now
`toolchain.json` pins LLVM 21.1.0 for both: Windows uses the official release
in `third_party/toolchains/` (fetched and checksum-verified by
`tools/toolchain.py`), also for vcpkg dependencies through a chainloaded
toolchain; the Linux image takes zig, the glibc target and the vcpkg commit from
the manifest. `tools/build.py` refuses to run on a mismatch.

| Windows | clang-cl 19.1.5 (VS) | clang-cl 21.1.0 (pinned) |
| --- | --- | --- |
| Clean build | ~160 s | 131 s |
| `jsc.exe` | 49.1 MB | 49.6 MB |
| Link test (`minimal`) | 46.1 MB | 46.6 MB |
| ICU compiled by | MSVC cl.exe | clang-cl 21.1.0 |

zig can also link the clang-cl-built engine archive for Windows
(`zig cc -target x86_64-windows-msvc`); the result passes the link test and
imports only system DLLs. zig cannot compile C++ for the MSVC ABI (its libc++
does not build against MSVC headers), so clang-cl stays the Windows compiler.

## Symbol isolation (step 3)

Patch 0003 makes every export decoration empty for `PLATFORM(ZEROG)` (WTF's
`ExportMacros.h`, bmalloc's `BExport.h`) and `OptionsZeroG` defines
`JS_NO_EXPORT`, so nothing is `dllexport`ed. `tools/bundle.py` then builds the
engine bundle from the engine objects plus static ICU and the filtered ICU data;
only names matching `port/api/exports.txt` stay public (`zg_*`, plus the JSC C
API until `zg_*` exists).

| | Linux (ELF) | Windows (COFF) |
| --- | --- | --- |
| Method | `ld -r --force-group-allocation`, then `objcopy --keep-global-symbols` | every defined symbol (C and mangled C++) renamed `zg_<name>` with `llvm-objcopy --redefine-syms`, linker directives rewritten |
| Bundle | `libzerog-engine.a`, 1 object, 89.0 MB | `zerog-engine.lib`, 994 objects, 794 MB (CodeView inside) |
| Public symbols | 161 | 160 (81,281 renamed) |
| Exports of the linked exe | n/a | 0 (was 164) |

`tests/link/host_conflicts.c` is a host that links its own stock ICU 79.1 (same
version, same symbol names) and defines its own `mi_malloc`: it links with no
duplicate symbols and each side keeps its own copy (host sees all 906 locales,
the engine its minimal data). It passes on Linux, and on Windows with both lld
and MSVC's link.exe; zig also links the Windows bundle (`zig cc -target
x86_64-windows-msvc`).

What it took, beyond the obvious:

- COFF linker directives are text: clang emits `/INCLUDE:<name>` for
  `__attribute__((used))` symbols (JSC's JIT operations), genccode emits
  `/EXPORT:icudt79_dat`. `.drectve` is rewritten (renamed names, no `/EXPORT`
  except the API) and its section flags restored (`LNK_INFO`), which
  llvm-objcopy cannot set.
- `/alternatename` pairs are shared with code outside the bundle (the MSVC STL
  emits `_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue` everywhere, its own static
  library included): those names are kept, or lld reports a conflict.
- Renaming only C symbols is not enough: ICU's C++ API (`icu_79::`) collides with
  a host's identical ICU, so mangled names are renamed too.
- ELF COMDAT groups are deduplicated by signature name even when the symbol is
  local, so a host with the same inline code shared the engine's inline data and
  crashed; `--force-group-allocation` removes the groups after the bundle's own
  duplicates are merged.

### C++ runtime inside the Linux bundle

The Linux bundle also contains zig's libc++, libc++abi and libunwind (found by a
probe link, so they always match the pinned zig and target), localized like the
rest: no `std::__1`, `__cxa_*` or `_Unwind_*` symbol is global. The link test is
now linked with the C driver (`zcc`), as a C, Zig, Go or Rust host would link it:
the bundle needs nothing but glibc and compiler-rt builtins. Bundle 89.0 ->
105.5 MB; stripped link test 49.48 -> 49.70 MB (unused libc++ sections are
dropped by the final link).

## mimalloc on every platform

Decision: one allocator everywhere, the mimalloc vendored in WebKit
(`USE_MIMALLOC`, `OptionsZeroG.cmake`). It never overrides the host's malloc and
its symbols are private to the bundle; the host-conflicts test now exercises a
host with its own `mi_malloc` on Linux too. `ZEROG_MIMALLOC_SECURE=ON` builds
mimalloc's hardened mode (`MI_SECURE`) through
`port/overlay/Source/bmalloc/mimalloc/PlatformZeroG.cmake`; off by default.
zig needs `-Wno-date-time` for mimalloc (its verbose banner uses `__DATE__`).

| Linux | system malloc | mimalloc |
| --- | --- | --- |
| Clean build | 166 s | 162 s |
| Link test, stripped | 49.70 MB | 50.92 MB |

## Gaps to the single-binary goal

