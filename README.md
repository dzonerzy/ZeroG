# ZeroG

An open source, single-process WebKit engine with a small, stable C API,
distributed as a single statically linked executable on Linux, Windows and
macOS. Offscreen rendering with Skia; the application decides where frames are
presented (native window, game texture, overlay).

The full design is in [ZeroG.md](ZeroG.md) (Italian). Verification of the
design against the pinned WebKit revision lives in [docs/verification](docs/verification).

> Status: phase 2 done — the `zg_*` C API (`include/zerog.h`, version 0.2,
> frozen at 0.1 and since extended: [docs/api-changes.md](docs/api-changes.md))
> runs on Windows and Linux: runtime and event loop, views in CPU, texture and
> window mode, input, callbacks, direct JavaScript bindings (functions, classes
> with native instances, binary data, references to JavaScript values, page
> scripts), and the resource loader: the app's files
> served from memory under `app://`, custom schemes, `data:`, http/https over
> curl with cookies and HTTP authentication, WebSockets (`ws://`, `wss://`),
> Blobs and `blob:` URLs, downloads, and fonts registered by the application.
> The API's tests also run under AddressSanitizer, and its parsers under
> libFuzzer.

## Layout

| Path | Contents |
| --- | --- |
| `include/zerog.h` | Public C API (`zg_*`) |
| `port/overlay/` | New files added to the WebKit tree (`Options<Port>.cmake`, `Platform<Port>.cmake`, …), mirroring its layout |
| `port/platform/` | Platform layer: GPU contexts for Skia, fonts, clipboard, cursor, IME, timers |
| `port/embedding/` | Embedding layer: process setup and `View` at the root; `clients/` (WebCore's clients), `loader/` (sessions, schemes and the app's files, curl, blobs, WebSockets, downloads, HTTP cache, fonts), `storage/` (Web storage, IndexedDB), `graphics/` (surfaces), `window/` (SDL window mode), `inspector/` (Web Inspector) |
| `port/gl/` | Linux EGL/GLES loader: entry points generated from the Khronos headers, driver loaded with `dlopen` |
| `tests/` | Link tests against the bundle, render tests, C API tests, fuzz targets and their corpus |
| `port/api/` | C API implementation |
| `patches/` | Minimal patch series on upstream files, applied in `series` order |
| `tools/` | Build and maintenance scripts |
| `third_party/webkit/` | Upstream WebKit checkout at `WEBKIT_REVISION` (not committed) |
| `docs/` | Verification reports, provenance of derived files |

Upstream changes go in `patches/`; new code goes in the project's own
directories. The size of the patch series is the port's health metric.

## Getting the sources

```sh
python tools/webkit.py fetch     # shallow fetch of WEBKIT_REVISION (~2 GB)
python tools/webkit.py prepare   # reset, apply patches/series, copy port/overlay
python tools/webkit.py status
```

## Building

```sh
python tools/build.py deps        # static dependencies into third_party/sysroot (vcpkg, deps/vcpkg.json)
python tools/build.py configure   # PORT=ZeroG into build/zerog-minimal
python tools/build.py build       # -t TARGET builds one target, -k keeps going after failures
python tools/build.py bundle      # engine + ICU in one archive; only port/api/exports.txt is public
python tools/build.py linktest    # C programs against the bundle, incl. a host with its own ICU
```

The phase 1 render test lays out and paints a page on the CPU, writes a PNG and
checks its pixels; `--gpu` also paints it with Skia's GPU backend (ANGLE over
D3D11 on Windows, the system EGL driver on Linux; the Docker image has Mesa's
llvmpipe) and compares the two images:

```sh
python tools/build.py build -t ZeroGFirstRender
build/zerog-minimal/bin/ZeroGFirstRender --gpu first_render.png
```

| Test | Checks |
| --- | --- |
| `ZeroGFirstRender [--gpu]` | HTML, CSS, script and text painted on the CPU (and GPU), pixels checked |
| `ZeroGFrameLoop` | `requestAnimationFrame` on the application's frame clock; each frame repaints only the damage; buffers match a full repaint |
| `ZeroGInput` | mouse (clicks, click count, `:hover`), focus, typing and editing commands (Backspace, arrows, Ctrl+A/Z/Shift+Z), key event properties, smooth wheel scrolling |
| `ZeroGWindow` | window mode (SDL3 window, GPU presentation at vsync): rAF at the display's rate, presented image matches a CPU render, cursor and typing from window events, resizing reaches the page. On Linux: `xvfb-run -a ZeroGWindow` |
| `ZeroGTexture` | texture mode: frames shared with the application's own device (D3D11 on Windows, an EGL display on Linux) and read on its render thread while the engine renders; no torn frames; the result matches a CPU render |
| `ZeroGLoader` | the resource loader: an `app://` page from memory (stylesheet, module scripts, fetch, range requests, compressed archive entries, percent-encoded paths, directory redirects, synchronous XHR, iframes), write-once files, `data:`, http over curl against a server in the test (CORS, redirects and `Response.redirected`, cookies, synchronous XHR, a main-frame navigation), WebSockets against the same server (subprotocol, handshake cookies, text, binary, a 100 KB message, the close handshake, refused and unreachable servers), registered fonts (TrueType and WOFF2, used by a page of their session only, also when registered while it is open), the HTTP disk cache (a new session served from disk, no-store, revalidation with ETag and 304, Vary, `cache: 'reload'` and `'only-if-cached'`, eviction of the least recently used under the capacity), https and `wss://` against a TLS server in the test whose CA is generated at run time (refused with "certificate not trusted" before the CA is added, loaded after). `ZEROG_ONLINE_TESTS=1` adds https to example.com and `wss://` to a public echo server |
| `ZeroGAccessibility` | accessibility in window mode, from a client on its own thread as a screen reader: on Windows UI Automation, on Linux AT-SPI over D-Bus (through the registry). The page's button is found by name and pressed, a text field read (on Windows also through the Text pattern, by character, and selected) and set with an input event, a check box toggled, a slider's value read, and focusing the field raises its focus event. On Linux: `dbus-run-session -- xvfb-run -a ZeroGAccessibility` (at-spi2-core) |
| `ZeroGCompositing` | GPU compositing on the compositor thread: frames for the page's rendering updates, CSS transform and opacity animations producing frames with no rendering update at all, one per application frame, none without, and at most as many as a frame rate cap allows, `will-change` making a layer, a still page producing none, a still composited frame matching the CPU render; with WebGL (standard, dev and debug builds) a WebGL canvas composited, in the minimal build no context |
| `ZeroGInspector` | the Web Inspector (dev and debug builds; elsewhere it checks there is none): the frontend opens in the embedder's view and connects to the page, the page's `console.log` reaches it, it is brought to the front, closes itself and is closed by the embedder or with the inspected view; the remote inspector's server lists the page to a socket client |
| `ZeroGAPITest [--no-window]` | the C API from a C99 program: runtime, bindings (synchronous, JSON, BigInt, exceptions, a Promise settled from another thread), evaluate, emit, callbacks, input and IME, frames, files and archives (one packed by `tools/assets.py`), a scheme handler answering from another thread, trust by scheme, a window whose loop a binding ends. `linktest` also links it against the bundle |
| `ZeroGValuesTest` | value conversions for every JavaScript type, both ways: each `zg_call_arg_*` reader (embedded NULs, UTF-8 outside the BMP, BigInt, int64 saturation), each `zg_value_*`, Promises resolved with JSON or rejected, `zg_view_evaluate` results |
| `ZeroGBindingsTest` | API 0.2: page scripts (before the page's own, again in each new document, errors to `js_error`); bytes (ArrayBuffer, typed arrays with an offset and DataView read and written in place, Uint8Arrays returned copied or in place, Promises resolved with bytes); references (a function called later with every kind of argument, an object handed back as itself, neither leaving its page); classes (constructor, methods, accessors, `instanceof`, `extends`, instances made by the host, wrong `this`, every native pointer finalized once) |
| `ZeroGStorageTest` | localStorage and IndexedDB persist in `data_dir` across contexts (on disk, structured values, quota), separate per directory and per origin; storage events between a context's views; sessionStorage per view; an ephemeral context keeps both in memory and leaves nothing |
| `ZeroGHostTest` | what a page shares with its host: the clipboard (Ctrl+C/X/V with text and HTML through the host's callbacks, a copy event's own data, `navigator.clipboard` writeText and readText, the in-process default), `<input type=file>` (the file_chooser callback, choosing and cancelling), drops from the host (a file read by the page, text, leaving), selecting text by dragging the mouse, a draggable element dropped on another (dragstart to dragend, with its data) |
| `ZeroGLifecycleTest [--no-window] [cycles]` | contexts, views and windows created and destroyed over and over, each with a page that has an iframe, a canvas, a `blob:` URL, timers and 3 MB of objects, destroyed while a scheme request, a fetch and a binding's Promise are pending (answered after the view is gone); afterwards the JavaScript heap, asset and surface memory are back where they were |
| `ZeroGFuzz{Assets,Requests,Values} <files>` | the fuzz targets ([tests/fuzz](tests/fuzz)) replaying inputs: asset archives, paths and `Range` headers against `app://`, values across bindings. The seed corpus runs with the tests |
| `ZeroGHello [--smoke]` | the example of ZeroG.md (`examples/hello`): a window, the frontend packed into the binary, `App.greet` called synchronously. `linktest` builds it against the bundle and runs it |
| `ZeroGBindingBench [calls]` | binding latency: empty synchronous and asynchronous calls, in µs ([docs/phase2-measurements.md](docs/phase2-measurements.md)) |

An application ships its frontend inside its binary: `tools/assets.py` packs a
directory (the frontend's build output) into an archive, as C source to compile
in or as a `.zga` file; `zg_context_mount_assets(ctx, "app", data, len)` serves
it at `app://localhost/`. Files are compressed with zlib where it pays off and
inflated by the engine on first use.

```sh
python tools/assets.py dist -o build/app_assets.c --c-symbol app_assets
```

`--profile minimal|standard|dev` selects the build profile and its ICU data;
`--port JSCOnly` builds the upstream reference port.

`--asan` builds with AddressSanitizer into `build/<port>-<profile>-asan` (Windows;
zig ships no sanitizer runtimes). mimalloc stays the allocator and reports its
blocks to the sanitizer. There the fuzz targets are libFuzzer programs, with
coverage of ZeroG's own code:

```sh
python tools/build.py build --asan -t ZeroGLifecycleTest -t ZeroGFuzzValues
build/zerog-minimal-asan/bin/ZeroGLifecycleTest
# New inputs go to the first directory; the seeds in the repository stay as they are.
mkdir -p build/fuzz/values
build/zerog-minimal-asan/bin/ZeroGFuzzValues build/fuzz/values tests/fuzz/corpus/values -max_total_time=600
```

### Toolchain

[toolchain.json](toolchain.json) pins one LLVM release everywhere (21.1.0, the
clang inside zig 0.16.0), zig, the glibc baseline and the vcpkg commit.
`tools/build.py` runs `tools/toolchain.py check` before every step and refuses
to build with anything else.

| Platform | Compiler | C++ runtime | Linked by |
| --- | --- | --- | --- |
| Windows x64 | clang-cl 21.1.0 (pinned, in `third_party/toolchains`) | MSVC STL, static CRT | lld-link, or zig for apps |
| Linux x86_64 | zig 0.16.0 (clang 21.1.0), target glibc 2.28 | libc++ (static, from zig) | zig |

Windows requirements:

- `python tools/toolchain.py fetch` once: downloads and verifies the pinned
  LLVM into `third_party/toolchains/`. System and Visual Studio clang are not used.
- Visual Studio 2022 Build Tools (Windows SDK, MSVC STL and CRT), Ninja,
  Python 3, Perl (Git for Windows' perl works), Ruby, gperf;
  vcpkg cloned at the pinned commit and bootstrapped in `third_party/vcpkg`.
- No Developer Mode or admin rights: upstream symlinks JavaScriptCore's private
  headers, which Windows only allows with that privilege. Patch 0001 writes
  forwarding headers instead (copies and hardlinks break `#pragma once`, since
  clang-cl matches it by resolved path).
- Long paths enabled (`LongPathsEnabled` registry key; `tools/webkit.py` sets
  `core.longpaths` and `core.autocrlf=false` on the checkout).

Linux builds run in the Docker builder, built from the manifest:

```sh
python tools/toolchain.py docker-build
docker run --rm -v "$PWD:/src:ro" -v zerog-linux-build:/build zerog-linux-builder \
    python3 tools/build.py all --build-dir /build/zerog-minimal
```

The tests run in the same image, on Mesa's llvmpipe, covering every EGL display
path (surfaceless, EGL device as on NVIDIA's headless driver, default) and both
window systems (Xvfb for X11, Weston's headless backend for Wayland):

```sh
docker run --rm -v "$PWD:/src:ro" -v zerog-linux-build:/build zerog-linux-builder \
    sh /src/tests/render/run_linux.sh
```

`ZEROG_EGL_PLATFORM=surfaceless|device|default` forces the headless EGL display
the engine picks. On a Windows host with Docker Desktop (WSL 2) the tests can use
the real GPU through Mesa's `d3d12` driver:

```sh
docker run --rm --gpus all -v /usr/lib/wsl:/usr/lib/wsl:ro \
    -e LD_LIBRARY_PATH=/usr/lib/wsl/lib -e GALLIUM_DRIVER=d3d12 \
    -e MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
    -v zerog-linux-build:/build zerog-linux-builder /build/zerog-minimal/bin/ZeroGTexture
```

`tools/webkit.py prepare` gives files whose content did not change their old
modification time back, so re-running it after editing a patch or an overlay
file only rebuilds what that change touches. Do not build (on either OS: the
Docker build shares the tree) while `prepare` runs: anything compiled from the
half-prepared tree, precompiled headers included, looks newer than its restored
sources and is not rebuilt.

## License

The ZeroG port, embedding layer and C API are BSD 2-Clause (proposed, see
ZeroG.md, "Questioni aperte"). WebCore and JavaScriptCore are LGPL; every
other component keeps its own license. Files derived from other sources are
tracked in [docs/PROVENANCE.md](docs/PROVENANCE.md).
