# Testing

Every change is tested on Windows and Linux before it lands. The tests are
programs in the build (`build/zerog-<profile>/bin/`); each prints one line per
check and ends with `PASS` or `FAILED`.

## Running them

=== "Windows"

    ```sh
    python tools/build.py all --profile minimal
    build/zerog-minimal/bin/ZeroGLoader
    ```

    `linktest` (part of `all`) links C programs against the bundle the way
    applications do and runs them.

=== "Linux"

    The Docker image runs the tests on Mesa's llvmpipe, covering every EGL
    display path (surfaceless, EGL device, default) and both window systems
    (Xvfb for X11, Weston's headless backend for Wayland):

    ```sh
    docker run --rm --device /dev/dri -v "$PWD:/src:ro" -v zerog-linux-build:/build zerog-linux-builder \
        sh /src/tests/render/run_linux.sh /build/zerog-minimal
    ```

    `--device /dev/dri` gives the dma-buf test a DRM device (vgem under WSL 2);
    without it that test is skipped. `ZEROG_EGL_PLATFORM=surfaceless|device|default`
    forces the headless EGL display the engine picks.

Tests whose subject a profile leaves out (media in minimal, the inspector
outside dev and debug) check that it is absent instead.

## The tests

| Test | Checks |
| --- | --- |
| `ZeroGFirstRender [--gpu]` | HTML, CSS, script and text painted on the CPU (and GPU), pixels checked |
| `ZeroGFrameLoop` | `requestAnimationFrame` on the application's frame clock; each frame repaints only the damage; buffers match a full repaint |
| `ZeroGInput` | mouse (clicks, click count, `:hover`), focus, typing and editing commands (Backspace, arrows, Ctrl+A/Z/Shift+Z), key event properties, smooth wheel scrolling |
| `ZeroGWindow` | window mode (SDL3 window, GPU presentation at vsync): rAF at the display's rate, presented image matches a CPU render, cursor and typing from window events, resizing reaches the page. A frameless transparent window: its empty part is transparent in what it presents, a translucent part premultiplied; a header marked `--zg-app-region: drag` moves it, a `no-drag` button inside does not, the edges resize it (on Windows, `WM_NCHITTEST` answers caption, client and edge, and the desktop shows through on screen when the desktop lets the test see it). Click-through: the same page's transparent part lets clicks through (Windows: `WindowFromPoint` finds the window behind there, this one on the header and the translucent card; X11: the input shape the X server has is the page's alpha). On Linux: `xvfb-run -a ZeroGWindow` |
| `ZeroGTexture` | texture mode: frames shared with the application's own device (D3D11 on Windows, an EGL display on Linux) and read on its render thread while the engine renders; no torn frames; the result matches a CPU render; on Linux, WebGL on the application's display |
| `ZeroGTextureD3D12` | Windows, texture mode for D3D12 games: frames opened on the reader's `ID3D12CommandQueue`, waited for and released on it, copied into a readback buffer while the engine renders; no torn frames, the last one matches a CPU render; a queue on another adapter (WARP) is refused, saying why |
| `ZeroGTextureD3D9` | Windows, texture mode for Direct3D 9 games: a 9Ex device opens the engine's textures, and the same device with the copy path (what plain devices and 9Ex devices on another adapter get) receives the changed pixels; each frame drawn as a textured quad and read back, none torn, the last one matching a CPU render, then `zg_surface_device_lost` and a Reset and frames again. A plain `IDirect3DDevice9` too, where the desktop allows one (skipped in remote sessions) |
| `ZeroGTextureVulkan` | Windows, texture mode for Vulkan games: the frames' NT handles imported (a D3D11 texture into a dedicated allocation, two D3D12 fences into timeline semaphores), each texture once by buffer ID, copied while the engine renders after waiting for the ready value, the release value signaled; none torn, the last one matching a CPU render. Skipped without a Vulkan driver that imports these handles |
| `ZeroGTextureDMABuf` | Linux, texture mode with dma-buf frames (`ZG_SURFACE_VULKAN`): the render thread imports each frame's planes (EGL_EXT_image_dma_buf_import) after its fence and reads it while the engine renders; no torn frames, a buffer keeps its descriptors, the last frame matches a CPU render; one frame imported into Vulkan memory (lavapipe's VK_EXT_external_memory_dma_buf) has the same bytes. In Docker it needs `--device /dev/dri` (vgem's dumb buffers: llvmpipe exports no dma-buf) |
| `ZeroGLoader` | the resource loader: an `app://` page from memory (stylesheet, module scripts, fetch, range requests, compressed archive entries, percent-encoded paths, directory redirects, synchronous XHR, iframes), write-once files, `data:`, http over curl against a server in the test (CORS, redirects and `Response.redirected`, cookies, synchronous XHR, a main-frame navigation, `<img>` loads cross-origin and from the cache, a `crossorigin` image refused without CORS headers, `window.visualViewport`), WebSockets against the same server (subprotocol, handshake cookies, text, binary, a 100 KB message, the close handshake, refused and unreachable servers), registered fonts (TrueType and WOFF2, used by a page of their session only, also when registered while it is open), the HTTP disk cache (a new session served from disk, no-store, revalidation with ETag and 304, Vary, `cache: 'reload'` and `'only-if-cached'`, eviction of the least recently used under the capacity), https and `wss://` against a TLS server in the test whose CA is generated at run time (refused with "certificate not trusted" before the CA is added, loaded after). Service workers (full profiles): one registers, installs and activates, controls the page, answers fetches (a made-up response, one from the cache it filled when installed, a blob body), leaves the others to the network, exchanges messages, answers a navigation; a new session on the same directory has it from the first navigation; another session has none; navigation preload (the navigation reaches the server once, with the registration's header value, and the worker gives it to the page); a changed script is fetched on the next controlled navigation and its new version takes over; minimal has no `navigator.serviceWorker`. `ZEROG_ONLINE_TESTS=1` adds https to example.com and `wss://` to a public echo server |
| `ZeroGAccessibility` | accessibility in window mode, from a client on its own thread as a screen reader: on Windows UI Automation, on Linux AT-SPI over D-Bus (through the registry). The page's button is found by name and pressed, a text field read (on Windows also through the Text pattern, by character, and selected) and set with an input event, a check box toggled, a slider's value read, and focusing the field raises its focus event. On Linux: `dbus-run-session -- xvfb-run -a ZeroGAccessibility` (at-spi2-core) |
| `ZeroGMedia` | `<audio>` and `<video>` (standard, dev and debug builds) from app://, against tests/media: canPlayType for each format (no patented codec); Opus and Vorbis in Ogg and WebM, MP3, FLAC and WAV load their duration and play to `ended`; a VP9 and Opus video has its size and duration, its frame painted in the page, a seek showing the frame there, and plays to its end; VP8 and AV1 frames painted; `getVideoPlaybackQuality` counting the frames. The native controls: the play button's icon drawn over the frame, clicking it plays, the controls bar shown while playing. VP9 and Opus in MP4 files everywhere (a regular file with its moov after the mdat, and a fragmented one): frames, a seek, the end. On Windows, H.264 and AAC in MP4 (Media Foundation): an M4A plays to its end, a 64x36 video is cropped from its coded size, painted, seeked and played to its end. Media Source Extensions: isTypeSupported, a video through a source buffer (appended in two pieces cut mid-element) with its buffered range, frames, a seek and its end for VP9 and Opus in WebM and in fragmented MP4 and, on Windows, H.264 with B-frames and AAC in fragmented MP4, and two source buffers (VP8 video, Vorbis audio) played to the end; with a GPU that decodes H.264, its frames come from the GPU (DXVA). In the minimal build nothing can play and there is no MediaSource. An AVIF image (libavif over dav1d) loads at its size with both its colours, and `image-set()` takes `image/avif`; minimal does not decode it. On Linux `SDL_AUDIODRIVER=dummy` gives it a sound device |
| `ZeroGWebAudio` | Web Audio (standard, dev and debug builds), checked on the rendered samples: an OfflineAudioContext's 440 Hz oscillator through a 0.5 gain has that amplitude and frequency; an AnalyserNode (the FFT) finds a 1 kHz tone; a PannerNode in HRTF mode renders, louder on its side; decodeAudioData decodes every format `<audio>` plays; a live AudioContext's clock runs on the sound device; an `<audio>`'s tone reaches an AnalyserNode through a MediaElementAudioSourceNode. The minimal build has no Web Audio. On Linux `SDL_AUDIODRIVER=dummy` |
| `ZeroGCompositing` | GPU compositing on the compositor thread: frames for the page's rendering updates, CSS transform and opacity animations producing frames with no rendering update at all, one per application frame, none without, and at most as many as a frame rate cap allows, `will-change` making a layer, a still page producing none, a still composited frame matching the CPU render; with WebGL (standard, dev and debug builds) a WebGL canvas composited, in the minimal build no context; a 2D canvas drawn on the GPU (a composited layer, its drawing in the frame, `getImageData` reading it back); a video (standard, dev and debug builds) as a layer of its own whose frames reach the compositor with no rendering update of the page, as YUV planes the GPU converts |
| `ZeroGInspector` | the Web Inspector (dev and debug builds; elsewhere it checks there is none): the frontend opens in the embedder's view and connects to the page, the page's `console.log` reaches it, it is brought to the front, closes itself and is closed by the embedder or with the inspected view; the remote inspector's server lists the page to a socket client |
| `ZeroGAPITest [--no-window]` | the C API from a C99 program: runtime, bindings (synchronous, JSON, BigInt, exceptions, a Promise settled from another thread), evaluate, emit, callbacks, input and IME, frames, files and archives (one packed by `tools/assets.py`), a scheme handler answering from another thread, trust by scheme, a window whose loop a binding ends, the game texture kinds' arguments and Win32 handles. `linktest` also links it against the bundle |
| `ZeroGValuesTest` | value conversions for every JavaScript type, both ways: each `zg_call_arg_*` reader (embedded NULs, UTF-8 outside the BMP, BigInt, int64 saturation), each `zg_value_*`, Promises resolved with JSON or rejected, `zg_view_evaluate` results |
| `ZeroGBindingsTest` | API 0.2: page scripts (before the page's own, again in each new document, errors to `js_error`); bytes (ArrayBuffer, typed arrays with an offset and DataView read and written in place, Uint8Arrays returned copied or in place, Promises resolved with bytes); references (a function called later with every kind of argument, an object handed back as itself, neither leaving its page); classes (constructor, methods, accessors, `instanceof`, `extends`, instances made by the host, wrong `this`, every native pointer finalized once) |
| `ZeroGObjectsTest` | API 0.4: a page's objects from C. The global object; `document.getElementById` called and the element's `textContent` and `style.color` set; strings, numbers, booleans, array elements and deeper paths read; strict types (a string read as a number fails with both types named); numbers, booleans, JSON, bytes and references set; methods with `this` their object and their results as references; a missing step, a non-object step, a non-function, a missing method, a throwing method and getter, empty steps and missing arguments as errors; references into a document that was replaced report their page gone |
| `ZeroGStorageTest` | localStorage and IndexedDB persist in `data_dir` across contexts (on disk, structured values, quota), separate per directory and per origin; storage events between a context's views; sessionStorage per view; an ephemeral context keeps both in memory and leaves nothing. Full profiles: the Cache API (put, match with headers and body, keys, delete, from a worker too) persists the same way; minimal has no `caches` |
| `ZeroGHostTest` | what a page shares with its host: the clipboard (Ctrl+C/X/V with text and HTML through the host's callbacks, a copy event's own data, `navigator.clipboard` writeText and readText, the in-process default), `<input type=file>` (the file_chooser callback, choosing and cancelling), drops from the host (a file read by the page, text, leaving), selecting text by dragging the mouse, a draggable element dropped on another (dragstart to dragend, with its data). Context menus: a right click calls the host with the position, the selection, a link's URL and text or an editable field, and WebCore's items (no new windows); choosing Copy, Copy Link or Paste does it, cancelling does nothing, Shift+F10 opens the focused element's menu, a page that cancels `contextmenu` gets no host menu |
| `ZeroGLifecycleTest [--no-window] [cycles]` | contexts, views and windows created and destroyed over and over, each with a page that has an iframe, a canvas, a `blob:` URL, timers and 3 MB of objects, destroyed while a scheme request, a fetch and a binding's Promise are pending (answered after the view is gone); afterwards the JavaScript heap, asset and surface memory are back where they were |
| `ZeroGFuzz{Assets,Requests,Values} <files>` | the fuzz targets ([tests/fuzz](https://github.com/dzonerzy/ZeroG/tree/main/tests/fuzz)) replaying inputs: asset archives, paths and `Range` headers against `app://`, values across bindings. The seed corpus runs with the tests |
| `ZeroGHello [--smoke]` | the example app (`examples/hello`): a window, the frontend packed into the binary, `App.greet` called synchronously. `linktest` builds it against the bundle and runs it, and relinks it from its relink kit and runs that too |
| `zerog-host-runtime` (linktest) | a host with its own handlers installed first (POSIX: SIGUSR1, SIGUSR2, SIGSEGV on an alternate signal stack; Windows: an unhandled exception filter and a vectored handler): they stay in place (on Linux SIGSEGV becomes JavaScriptCore's fault handler, which calls the host's for faults not its own), pages allocate with the JIT and concurrent GC on a signal of the engine's own while the host keeps receiving its signals, engine threads have names (Windows: thread descriptions, as crash dumps and profilers read them), and a crash in host code, in a child process, reaches the host's crash handler |
| `zerog-host-conflicts` (linktest) | a host with its own ICU (the same version, the same symbol names) and its own `mi_malloc`, linked together with the bundle: no duplicate symbols, each side keeps its own; on Windows also linked with MSVC's link.exe |
| `ZeroGSite <url> [seconds] [out.png] [--run <script>] [--gpu]` | not in the suite (it needs the network): a real site in a view, its console and its `<video>` elements' state every second, a render saved at the end. YouTube's watch page plays through MSE (VP9 and Opus), buffers ahead and seeks; hls.js's demo plays its H.264 and AAC through MSE with no dropped frame, 1080p in a GPU view (`--gpu`) |
| `ZeroGMemoryBench <scenario>` | memory at rest of one scenario (`baseline`, `cpu`, `texture`, `window`, `page`): process and engine counters (`tools/memory.py` runs them all) |
| `ZeroGBindingBench [calls]` | binding latency: empty synchronous and asynchronous calls, in µs ([measurements](measurements.md#binding-latency)) |

`linktest` also checks that `include/zerog.h` declares exactly the functions
the C API library defines, that the bundle keeps them all public, and that
every ICU converter WebCore routes to ICU opens with the filtered data.

## AddressSanitizer and fuzzing

`--asan` builds with AddressSanitizer into `build/zerog-<profile>-asan`
(Windows; zig ships no sanitizer runtimes). mimalloc stays the allocator and
reports its blocks to the sanitizer. There the fuzz targets are libFuzzer
programs, with coverage of ZeroG's own code:

```sh
python tools/build.py build --asan -t ZeroGLifecycleTest -t ZeroGFuzzValues
build/zerog-minimal-asan/bin/ZeroGLifecycleTest
mkdir -p build/fuzz/values
build/zerog-minimal-asan/bin/ZeroGFuzzValues build/fuzz/values tests/fuzz/corpus/values -max_total_time=600
```

## Continuous integration

GitHub Actions run quick checks on every push: `include/zerog.h` compiled as
C99 and C++17 with gcc, clang, MSVC and clang-cl; the tools' unit tests
(`python -m unittest discover -s tests/tools`); the API reference checked
against the header; the documentation site built; and the patch series applied
to the pinned WebKit revision when patches change. A full build (hours, about
100 GB) does not fit hosted runners: full builds and the tests above run with
`tools/build.py`, on Windows and in the Linux builder.
