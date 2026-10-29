## Summary

### Already resolved

| Finding | Resolution |
| --- | --- |
| `USE_SYSTEM_MALLOC` is a hard `#error` on Windows | Vendored mimalloc on Windows, system malloc on Linux (`OptionsZeroG.cmake`) |
| `/MT` is not reachable with `-D` (OptionsMSVC forces `MultiThreadedDLL`) | `OptionsZeroG.cmake` re-sets it after `include(OptionsMSVC)`; dependencies built `/MT` |
| A new `PORT` needs `ALL_PORTS` plus a platform branch, or Windows becomes `PLATFORM(WIN)` | Patch 0002: `PLATFORM(ZEROG)` |
| Static `libJavaScriptCore.a` lacks the `JavaScriptCoreJIT` objects; thin archives | `tools/build.py bundle`, `USE_THIN_ARCHIVES=OFF` |
| Code outside WebKit must define `STATICALLY_LINKED_WITH_*` or `JS_NO_EXPORT` | Patch 0003 removes export decorations; `JS_NO_EXPORT` everywhere |
| zig's libc++ cannot be mixed with libstdc++ / MSVC STL objects | Linux builds everything with zig; Windows keeps clang-cl (MSVC STL); zig only links |
| **WebCore's CJK codecs build their tables from ICU converters at runtime** (EUC-JP, Big-5, windows-949, gb18030; `EncodingTables.cpp`), with a `RELEASE_ASSERT` on the result; EUC-TW is declared with `DECLARE_ENCODING_NAME_NO_ALIASES` | The ICU data filter had dropped them (first CJK page would crash): `tools/icu_data.py` now keeps every converter WebCore opens by name or declares, matched the way ICU compares names; the link test opens all of them. Minimal ICU data 12.6 -> 13.7 MB |

### Findings that reshape phase 1 (WebCore)

1. **The embedding layer must be written from scratch.** Ultralight's public WebCore fork does not contain its `Page`/`ChromeClient`/`LocalFrameLoaderClient`/`EditorClient`; those are in its closed module. References: WebKitLegacy (Apple-only, but the client implementations) and WebCore's `EmptyClients`. The Empty* clients are mostly `final`, so they are templates to copy, not base classes.
2. **Networking is WebKit2-shaped.** `ResourceHandle` is a stub for curl, `NetworkStorageSession` moved to `Source/WebKit`, `CookieJar` asserts outside Cocoa, cookie header handling lives in `NetworkDataTaskCurl`. ZeroG needs its own LoaderStrategy (or a revived `ResourceHandleCurl` over `CurlRequest`), cookie jar subclass and storage session. `CurlSSLHandle::platformInitialize` exists only for Windows and PlayStation.
3. **WebSocket is not in WebCore any more.** The single-process `WebSocketChannel` (hybi framing) lives in WebKitLegacy with only a CFNetwork backend; `SocketProvider::createWebSocketChannel` is pure. Dev-mode hot reload needs ZeroG's own channel over `CurlStream`, which connects with a blocking `curl_easy_perform` on one shared thread.
4. **GPU: one upstream path, Skia Ganesh over GLES via EGL** (`SK_ASSUME_GL_ES`). Linux: EGL/GLES. Windows: ANGLE EGL. Vulkan and native Metal are new work; macOS v1 is realistically Skia GL over ANGLE's Metal backend. On `PLATFORM(WIN)` the shared display is a hard-coded ANGLE display; `PLATFORM(ZEROG)` avoids that but loses Win-only branches. The WebKit2 Win compositor (WC*) is not reusable; `GraphicsLayerTextureMapper` is the in-process compositor for v2.
5. **The phase 1 dependency set is larger than the doc's step 2**: ICU, libxml2, SQLite, zlib, HarfBuzz *with* ICU (`hb-icu`), FreeType, libpng, libjpeg-turbo, libwebp (+demux, mux, sharpyuv; excluding it breaks `toDataURL`), **BoringSSL already for PAL's digests**, libpsl (or stubs). Skia's CMake hard-requires Fontconfig off Windows: patch needed. Skia's `third_party/` holds no sources; every dependency comes from the sysroot.
6. **Process-global setup** WebKitLegacy performs before the first `Page`: platform strategies, DOM wrapper hooks, storage tracker, log channels; `BlobRegistry` through `PlatformStrategies`. Four `PageConfiguration` members typed optional are dereferenced unconditionally.
7. **Flags**: `ENABLE_SERVICE_WORKER` no longer exists (always compiled; off at runtime by default). `InspectorClient` is now `InspectorBackendClient`.
8. **Threads and env vars**: every Skia GL context spawns 2 unnamed worker threads; MSAA and other knobs read environment variables (`WEBKIT_SKIA_MSAA_SAMPLE_COUNT`), against the doc's "config, not env vars".
9. **GC signal on Linux**: JSC still checks for an existing handler but now only logs and replaces it; if the host re-installs its handler or blocks the signal, `Thread::suspend` waits forever and the GC deadlocks. Bun's sigaltstack fix is real: upstream's retry tests the handler's own stack pointer. A configurable free signal and the alternate-stack fix are both needed.
10. **Virtual memory reservations exist whatever the allocator**: JSC always reserves a 4 GB Structure heap on 64-bit, the JIT a 1 GB executable pool (x86_64), mimalloc 1 GiB arenas; under `vm.overcommit_memory=2` they count against the commit limit. System malloc only removes Gigacage.

### Decisions for the project owner

- **Licensing: WTF is LGPL-encumbered too**, so the relink obligation covers the whole WTF + JSC + WebCore archive, and files derived from WebKitLegacy still link LGPL WTF. The license table in ZeroG.md needs fixing; the relink kit design is unaffected.
- **Allocator**: system malloc on Linux plus mimalloc on Windows (today), or mimalloc everywhere (identical behaviour on all OSes, optional `MI_SECURE` hardening and a smaller arena reservation)?
- **Windows CRT for host applications**: the bundle is `/MT`; MSVC hosts built `/MD` cannot link it (`/FAILIFMISMATCH`). Ship `/MT` only (single-exe goal), or also an `/MD` bundle?
- **Bindings: sync or async by default.** ZeroG.md says sync, but the Zig example uses `await App.listFiles`. Proposal: an explicit async marker (`zg.Async(T)`) that emits `Promise<T>` in `bindings.d.ts`.
- **External content must be a runtime-level switch** fixed at `zg_runtime_create` (it forces the JIT off for the process), not a context or view option. `include/zerog.h` currently has it in `zg_context_config`.
- **Profiles**: split `dev` (release engine + inspector) from `debug` (assertions), and decide how the inspector frontend is shown (second window, or an external browser over the remote inspector socket).
- **rAF clock per mode**: engine vsync in window mode, explicit timestamp from the app in texture/CPU mode (`RunLoopObserver` does nothing on `RunLoopGeneric`).
- **Dev origins**: an `allowed_origins` list in the context config so the dev server can get bindings and navigation in dev builds only.
