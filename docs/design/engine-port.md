# Engine port

The engine is **its own cross-platform WebKit port**, built directly on WTF, JavaScriptCore and WebCore, without the multi-process WebKit2 layer and without starting from WPE or the Win port. It is the same approach as Ultralight.

Upstream has already removed the single-process path for non-Apple ports: [WinCairo's WebKit1 was eliminated](https://lists.webkit.org/pipermail/webkit-unassigned/2023-March/1093883.html), and with it the reason to maintain the old curl implementation of `ResourceHandle`. The layer that connects WebCore to the application must therefore be written by the project.

## Layers

| Layer | Origin | Content |
| --- | --- | --- |
| WTF | Upstream | Data structures, threading, `RunLoopGeneric` (already used by the JSCOnly and PlayStation ports), hence no GLib |
| JavaScriptCore | Upstream | JS engine, JIT, C API (`JSContextRef`, `JSObjectRef`) |
| WebCore | Upstream with minimal patches | DOM, CSS, layout, rendering with Skia, networking with the curl classes of `platform/network/curl` |
| Platform layer | Project | GPU contexts for Skia, font manager, clipboard, cursor, IME, timers |
| Embedding layer | Project | `Page`, `LocalFrame` and the WebCore clients (chrome, editor, frame loader, context menu), resource loader |
| C API | Project | Public `zg_*` interface |

The names of WebCore's client classes change often between revisions (for example `FrameLoaderClient` became `LocalFrameLoaderClient` with the site isolation work). They must be checked on the chosen revision.

## References to study

- **[Ultralight's WebCore](https://github.com/ultralight-ux/WebCore)**: Ultralight's public LGPL fork, with a working single-process embedding layer. It is the reference closest to the project.
- **Apple's WebKitLegacy** (`Source/WebKitLegacy`): the last single-process embedding layer still in the repository, only for Apple platforms. It shows how WebCore's clients are implemented.
- **PlayStation and JSCOnly ports**: CMake build without GLib, `RunLoopGeneric`, curl networking, Skia planned.

## CMake port

The port is added as a new value of `PORT` in WebKit's CMake, with the files `Options<Port>.cmake` and `Platform<Port>.cmake` for WTF, JavaScriptCore and WebCore, on the PlayStation model. The same configuration applies to Linux, Windows and macOS; the per-OS differences live only in the platform layer.

## Graphics: Skia

Since WPE and WebKitGTK 2.46 [Skia has replaced Cairo as the 2D renderer](https://wpewebkit.org/release/wpewebkit-2.46.0.html). Skia is already included in the WebKit repository and [statically linked in the CMake build](https://lists.webkit.org/pipermail/webkit-dev/2024-February/032615.html), with the BSD 3-Clause license. The port reuses WebCore's Skia graphics code and only adds the creation of GPU contexts for each OS.

## JavaScriptCore

- JIT enabled on the platforms that allow it; interpreter as fallback.
- The CMake build has an `ENABLE_STATIC_JSC` option to compile JSC as non-shared, [introduced for the PlayStation port](https://lists.webkit.org/pipermail/webkit-unassigned/2021-September/995296.html).
- JSC's C API is the basis of the native bindings to the host.

## Upstream revision to follow

- The port follows **`main`**: fixes, including security fixes, land there first.
- `main` is not followed day by day: a commit that passes the whole CI is pinned and updated at regular intervals (proposal: every 2–4 weeks), or immediately in case of a critical security advisory.
- A nightly job builds the port against the latest `main`, so breakages of WebCore's internal APIs are seen the day they arrive, instead of piling up.
- Changes to upstream files live in [`patches/`](https://github.com/dzonerzy/ZeroG/blob/main/patches); the port's new code lives in its own directories, so conflicts stay limited to the patches.
