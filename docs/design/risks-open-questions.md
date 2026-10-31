# Risks and open questions

The risk that can make the project fail is the **maintenance cost of the embedding layer**: WebCore's internal APIs change at every revision, and no upstream port shares that work anymore. It is the price of single-process and of its own port, and it is the same one that Ultralight pays.

## Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| WebCore internal APIs that change at every rebase | High | Small embedding layer, regular rebases, layer tests at every rebase |
| Web content without sandbox in the app's process | High | External content disabled by default, minimal profile, JIT can be disabled |
| Static applications with vulnerable WebKit | High | Update policy, version check in the framework |
| Accelerated compositing to maintain | Medium | Upstream Skia compositor (coordinated graphics) with a small compositor thread of its own; CPU views without compositing |
| Media engine to maintain | Medium | Separate and stable libraries, decoders shared between files and MSE, tests with fixtures for every format and a real site (`ZeroGSite`) |
| Skia GPU backend on Windows | Medium | Windows prototype early, as a roadmap gate |
| No crash isolation | Medium | Fuzzing of the project's code, ASan in CI |
| Binary size | Medium | ICU data filter, minimal profile; to be measured at the first milestone |
| Adoption barrier due to the LGPL | Medium | Relink kit and dynamic profile |
| Competition from Servo | Low | Differentiate on compatibility and C API |
| Conflicts with the host: signals, allocator, duplicate symbols | High | Configurable GC signal, single archive with local symbols, exposed memory statistics, tests with host runtimes and crash handlers |
| JIT blocked by platform policies | Medium | Detection at startup and fallback to the interpreter, entitlement added by the package on macOS |

## Open questions

- [ ] Name: decided, ZeroG (trademark search in EUIPO to be done). License of the engine code still to be decided.
- [x] Upstream branch: decided, `main` with pinned commit and regular updates.
- [x] Windows: decided, Skia on ANGLE (GL on D3D11).
- [x] Compositing: decided and done, the upstream Skia compositor (coordinated graphics) with its own compositor thread; no TextureMapper.
- [x] Media backend: decided and done, separate royalty-free libraries instead of FFmpeg, plus the system's decoders for H.264 and AAC (Media Foundation on Windows).
- [x] Static JSC: WTF, JavaScriptCore and PAL build as static archives (`*_LIBRARY_TYPE STATIC` in the port), without patches.
- [x] Fonts: for now FreeType everywhere, with Skia's custom font manager; the native font manager remains an option not done.
- [ ] Accessibility: AT-SPI and UI Automation done (window mode); NSAccessibility and texture mode remain.

* [x] Allocator: decided, mimalloc on all platforms; secure mode available (`ZEROG_MIMALLOC_SECURE`), off by default.
* [x] CRT on Windows: decided, `/MT` only.
* [x] Bindings: decided, synchronous by default, asynchronous supported if declared (`async_methods`).
* [x] Licenses: WTF must be treated as LGPL; the relink obligation covers the whole archive, the relink kit design stays unchanged.
* [ ] JSC GC signal on Linux: configurable (`zg_runtime_config.gc_signal`, default a free real-time signal); tests with the most common host runtimes and with a crash handler with alternate stack are missing.
* [ ] Integration with the host's crash reporter: which ones to support officially?

- [x] WebSocket backend in single-process: decided and done, implementation in the embedding layer on top of curl.
- [x] Cache API: decided and done in the standard, dev and debug profiles, with its own storage in the embedding layer; off in minimal.
- [x] Service workers: done in the standard, dev and debug profiles, all in the engine's process; off in minimal. With script updates and navigation preload. Push, notifications and background fetch remain out.
- [x] AVIF: done in the standard, dev and debug profiles (libavif on top of dav1d); off in minimal.
- [x] JPEG XL: decided, excluded from all profiles (not widespread, heavy dependencies).
- [x] Context menu: callback to the host with WebCore's items, executed by the engine; the native menu belongs to the Zig framework (decided: no menus drawn by the engine).
- [x] Click-through on the transparent parts of a window: decided and done (Windows with layered window, X11 with the input shape); not on Wayland.
