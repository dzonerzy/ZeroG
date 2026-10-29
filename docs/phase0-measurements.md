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

## Gaps to the single-binary goal

- Windows: static CRT needs a patch (`CMAKE_MSVC_RUNTIME_LIBRARY` is forced to `MultiThreadedDLL`).
- Linux: static ICU (with data filter) and a static C++ runtime.
- Static library sizes are not yet meaningful: the Windows `.lib` files embed
  `/Zi` debug info (JavaScriptCore.lib 705 MB) and the Linux archives need checking
  (libJavaScriptCore.a is 3.4 MB, so part of JSC lives in other targets or thin archives).
