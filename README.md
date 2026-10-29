# ZeroG

An open source, single-process WebKit engine with a small, stable C API,
distributed as a single statically linked executable on Linux, Windows and
macOS. Offscreen rendering with Skia; the application decides where frames are
presented (native window, game texture, overlay).

The full design is in [ZeroG.md](ZeroG.md) (Italian). Verification of the
design against the pinned WebKit revision lives in [docs/verification](docs/verification).

> Status: phase 1 — WebCore renders HTML, CSS, JavaScript and text to a PNG on
> Windows and Linux (`tests/render/first_render.cpp`). No public API yet.

## Layout

| Path | Contents |
| --- | --- |
| `include/zerog.h` | Public C API (`zg_*`), draft until phase 2 |
| `port/overlay/` | New files added to the WebKit tree (`Options<Port>.cmake`, `Platform<Port>.cmake`, …), mirroring its layout |
| `port/platform/` | Platform layer: GPU contexts for Skia, fonts, clipboard, cursor, IME, timers |
| `port/embedding/` | Embedding layer: `Page`, `LocalFrame`, WebCore clients, resource loader |
| `port/gl/` | Linux EGL/GLES loader: entry points generated from the Khronos headers, driver loaded with `dlopen` |
| `tests/` | Link tests against the bundle, render tests |
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
checks its pixels:

```sh
python tools/build.py build -t ZeroGFirstRender
build/zerog-minimal/bin/ZeroGFirstRender first_render.png
```

`--profile minimal|standard|dev` selects the build profile and its ICU data;
`--port JSCOnly` builds the upstream reference port.

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

`tools/webkit.py prepare` gives files whose content did not change their old
modification time back, so re-running it after editing a patch or an overlay
file only rebuilds what that change touches.

## License

The ZeroG port, embedding layer and C API are BSD 2-Clause (proposed, see
ZeroG.md, "Questioni aperte"). WebCore and JavaScriptCore are LGPL; every
other component keeps its own license. Files derived from other sources are
tracked in [docs/PROVENANCE.md](docs/PROVENANCE.md).
