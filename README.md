# ZeroG

An open source, single-process WebKit engine with a small, stable C API,
distributed as a single statically linked executable on Linux, Windows and
macOS. Offscreen rendering with Skia; the application decides where frames are
presented (native window, game texture, overlay).

The full design is in [ZeroG.md](ZeroG.md) (Italian). Verification of the
design against the pinned WebKit revision lives in [docs/verification](docs/verification).

> Status: phase 0 — repository and build infrastructure. Nothing usable yet.

## Layout

| Path | Contents |
| --- | --- |
| `include/zerog.h` | Public C API (`zg_*`), draft until phase 2 |
| `port/overlay/` | New files added to the WebKit tree (`Options<Port>.cmake`, `Platform<Port>.cmake`, …), mirroring its layout |
| `port/platform/` | Platform layer: GPU contexts for Skia, fonts, clipboard, cursor, IME, timers |
| `port/embedding/` | Embedding layer: `Page`, `LocalFrame`, WebCore clients, resource loader |
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

## Building (ZeroG port, stage 1: static WTF + JavaScriptCore)

```sh
python tools/build.py deps        # static ICU 79.1 + filtered ICU data (vcpkg, deps/icu-data.json)
python tools/build.py configure   # PORT=ZeroG into build/zerog-minimal
python tools/build.py build
python tools/build.py bundle      # one engine archive: lib/zerog-engine.{lib,a}
python tools/build.py linktest    # C program linked against that archive
```

`--profile minimal|standard|dev` selects the build profile and its ICU data;
`--port JSCOnly` builds the upstream reference port.

Windows requirements:

- Visual Studio 2022 (C++ workload, Windows SDK), LLVM/clang-cl >= 19, Ninja,
  Python 3, Perl (Git for Windows' perl works), Ruby, gperf;
  vcpkg cloned and bootstrapped in `third_party/vcpkg`.
- No Developer Mode or admin rights: upstream symlinks JavaScriptCore's private
  headers, which Windows only allows with that privilege. Patch 0001 writes
  forwarding headers instead (copies and hardlinks break `#pragma once`, since
  clang-cl matches it by resolved path).
- Long paths enabled (`LongPathsEnabled` registry key; `tools/webkit.py` sets
  `core.longpaths` and `core.autocrlf=false` on the checkout).

Linux builds run in the pinned Docker image:

```sh
docker build -t zerog-linux-builder -f tools/docker/linux-builder.Dockerfile tools/docker
docker run --rm -v "$PWD:/src:ro" -v zerog-linux-build:/build zerog-linux-builder \
    python3 tools/build.py all --build-dir /build/zerog-minimal
```

## License

The ZeroG port, embedding layer and C API are BSD 2-Clause (proposed, see
ZeroG.md, "Questioni aperte"). WebCore and JavaScriptCore are LGPL; every
other component keeps its own license. Files derived from other sources are
tracked in [docs/PROVENANCE.md](docs/PROVENANCE.md).
