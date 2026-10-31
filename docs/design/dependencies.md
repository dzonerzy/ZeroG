# Dependencies: what to remove, keep, replace

The minimal profile depends on **16 libraries (17 on Windows, with ANGLE), all built statically**, and at runtime on nothing other than libc, GPU drivers and system components. Everything that the upstream Linux ports bring with them (GLib, libsoup, GStreamer, Fontconfig, system libraries for sandbox and accessibility) is removed or replaced.

## Criteria

A dependency stays only if it meets all three conditions:

1. it serves a WebCore feature that the profile keeps on;
2. it builds statically with CMake or zig, without invasive patches;
3. it does not load plugins, modules or data files at runtime from outside the binary.

## Decisions

| Dependency | Decision | Use | How |
| --- | --- | --- | --- |
| ICU | Keep | Unicode, segmentation, normalization, JSC's `Intl`: used everywhere in WTF and WebCore | Static with data filter and few languages, keeping the time zone data for Date and Intl. WebCore has its own codecs for many encodings (`TextCodecCJK`, `TextCodecSingleByte`), so the ICU converter data can be reduced a lot (to be verified) |
| HarfBuzz | Keep | Text shaping | Without GLib, ICU and Graphite; internal Unicode data |
| FreeType | Keep | Glyph rasterization | Only zlib and libpng as dependencies; no bzip2 and brotli |
| Skia | Keep | 2D graphics | Already in the repository; only the necessary GPU backends, no PDF, SVG and Skottie |
| curl | Keep | HTTP/1.1 and HTTP/2 | Only HTTP and HTTPS: no FTP, SMTP, LDAP and other protocols |
| BoringSSL | Keep | TLS and WebCrypto | A single crypto library for networking and WebCrypto |
| nghttp2 | Keep | HTTP/2 | — |
| libpsl | Keep | Public suffix list for cookie domains | List compiled into the binary |
| zlib | Keep | Decompression for HTTP, PNG and FreeType | zlib-ng in compatibility mode for performance |
| brotli, woff2 | Keep | WOFF2 fonts and `Content-Encoding: br` | WOFF2 is the most common web font format in frontends |
| libpng, libjpeg-turbo | Keep | PNG and JPEG images, `toDataURL` | — |
| libwebp | Keep | WebP images | Can be excluded if the app does not use WebP |
| libxml2 | Keep | XML and XHTML documents, SVG as a document, `responseXML` | Minimal build: no HTTP, FTP, iconv, LZMA, Python |
| SQLite | Keep | Cookie jar, IndexedDB | Amalgamation in a single C file |
| GLib, GObject, GIO | Replace | Main loop and types of the upstream Linux ports | WTF's `RunLoopGeneric` |
| libsoup, glib-networking | Replace | Networking and TLS of the GLib ports; TLS is a module loaded at runtime | curl and BoringSSL |
| libgcrypt, libtasn1 | Replace | WebCrypto of the GLib ports | [WebCore's OpenSSL backend](https://webkit.googlesource.com/WebKit/+/7f131dee9ca99449765f131be8dfd81c800dce88/Source/WebCore/crypto/openssl), written by Sony for its ports, built against BoringSSL |
| Fontconfig, expat | Replace | Font lookup and fallback on Linux | Skia's font manager for embedded fonts or fonts read from directories, plus its own fallback table |
| GTK, Wayland, X11 | Replace | Window and input | Windowless core; window mode with static SDL3, which loads X11 and Wayland at runtime with `dlopen` (and in the profiles with media the audio: ALSA, PulseAudio, PipeWire, also at runtime) |
| libepoxy | Replace | Loading of OpenGL functions | Skia loads the GL and Vulkan functions with `eglGetProcAddress` and `vkGetInstanceProcAddr` |
| GStreamer | Replace | Audio and video via plugins | Own media engine on separate libraries (see [Media](build-system-static-linking.md#media)): libwebm, libvpx, dav1d, libopus, libogg, opusfile, libvorbis, dr_libs, libyuv, pffft for Web Audio; H.264 and AAC from the system's decoders on Windows |
| libdrm, GBM | Remove | DMA-BUF between processes | Useless for transport between processes; for the Vulkan surface on Linux GBM and the DRM ioctls are loaded at runtime, without a build dependency |
| libxslt | Remove | XSLT | `ENABLE_XSLT=OFF` |
| lcms2 | Remove | ICC color profiles | Skia handles color spaces |
| libavif | Keep (standard, dev, debug) | AVIF | Decoder only, on top of dav1d (already in the engine for AV1 videos) and libyuv; no encoder. The vcpkg port is tweaked so that clang-cl merges the static archives like MSVC ([`deps/ports/libavif`](https://github.com/dzonerzy/ZeroG/blob/main/deps/ports/libavif)). Off in minimal |
| libjxl | Remove | JPEG XL | Excluded: little used on the web (Chrome and Firefox do not display it by default), and libjxl brings with it highway and lcms2, about 2 MB |
| libhyphen | Remove | `hyphens: auto` | — |
| enchant | Remove | Spell checking | — |
| libmanette | Remove | Gamepad API | — |
| libsecret | Remove | Credential store | — |
| ATK, AT-SPI | Replace | Accessibility on Linux | Own AT-SPI bridge on libdbus loaded at runtime, without ATK or GLib |
| libsystemd, libseccomp, bubblewrap, xdg-dbus-proxy | Remove | Logging and sandbox of the multi-process ports | — |
| libidn2 | Remove | Internationalized domains in curl | WebCore already converts hosts to punycode with ICU |
| c-ares | Remove | Asynchronous DNS | curl's threaded resolver is enough |
| ANGLE | Keep | WebGL; OpenGL on Windows | Required on Windows as Skia's GPU backend (GL on D3D11); on Linux and macOS only in the standard profile, for WebGL |

## Features to turn off

| Feature | CMake flag | minimal | standard |
| --- | --- | --- | --- |
| Video and audio | `ENABLE_VIDEO` | On, without backend (no playback) | With media backend |
| Web Audio | `ENABLE_WEB_AUDIO` | No | With media backend |
| Media Source Extensions | `ENABLE_MEDIA_SOURCE` | No | Yes (WebM, fragmented MP4) |
| WebGL | `ENABLE_WEBGL` | No | Yes |
| WebAssembly | `ENABLE_WEBASSEMBLY` | No | Yes |
| FTL JIT (fourth tier of JSC) | `ENABLE_FTL_JIT` | No: its B3 backend builds only with WebAssembly; interpreter, Baseline and DFG remain | Yes |
| XSLT | `ENABLE_XSLT` | No | No |
| WebRTC and MediaStream | `ENABLE_WEB_RTC`, `ENABLE_MEDIA_STREAM` | No | No |
| Encrypted Media | `ENABLE_ENCRYPTED_MEDIA` | No | No |
| Gamepad | `ENABLE_GAMEPAD` | No | No |
| Speech synthesis | `ENABLE_SPEECH_SYNTHESIS` | No | No |
| Cache API | `CacheAPIEnabled` (setting) | No | Yes |
| Service worker | `ServiceWorkersEnabled` (setting) | No | Yes |
| Remote Web Inspector | `ENABLE_REMOTE_INSPECTOR` | No | Only in the dev and debug profiles |
| AVIF | `USE_AVIF` | No | Yes |
| JPEG XL | `USE_JPEGXL` | No | No |
| WOFF2 | `USE_WOFF2` | Yes | Yes |

The flag names must be verified on the chosen revision: they change between versions, and some are `ENABLE_` while others are `USE_`.

## What stays dynamic at runtime

| Platform | Loaded from the system | How |
| --- | --- | --- |
| Linux | glibc, GPU drivers (`libEGL`, `libGL`, `libvulkan`) | Build against an old baseline glibc (for example 2.28) with zig, so the binary also starts on dated distros. Static musl is excluded: it cannot load GPU drivers with `dlopen` |
| Windows | Only system DLLs (kernel32, user32, d3d11, dxgi, crypt32, ws2\_32) | Static CRT (`/MT`), the only variant distributed: an MSVC host must also build with `/MT`, because the linker refuses to mix different runtimes (`/FAILIFMISMATCH`) |
| macOS | Only system frameworks (Metal, AppKit, CoreFoundation, Security) | — |

Perl, Python, Ruby, gperf and unifdef are needed to generate code during the WebKit build, but do not end up in the binary.

## Order of work

1. Start from the configuration of the JSCOnly port, which [depends only on ICU](https://bugs.webkit.org/show_bug.cgi?id=155706): it immediately verifies the static build of WTF and JSC.
2. Add WebCore with all the features in the table turned off and only Skia, HarfBuzz, FreeType, libxml2 and SQLite.
3. Add the other libraries one at a time, when the feature that requires them arrives: networking (curl, BoringSSL, nghttp2, libpsl), images (libpng, libjpeg-turbo, libwebp), web fonts (brotli, woff2).
4. Measure the binary size at every addition, in CI, so every dependency has a visible cost.
