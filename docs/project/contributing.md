# Contributing

## Repository layout

| Path | Contents |
| --- | --- |
| `include/zerog.h` | The public C API (`zg_*`) |
| `port/api/` | The C API's implementation |
| `port/embedding/` | The embedding layer: process setup and `View` at the root; `clients/` (WebCore's clients), `loader/`, `storage/`, `graphics/` (surfaces, compositor), `window/`, `inspector/`, `media/`, `serviceworkers/` |
| `port/platform/` | Platform layer: GPU contexts, fonts, clipboard, cursor, IME, timers, accessibility bridges |
| `port/gl/` | Linux EGL/GLES loader |
| `port/overlay/` | New files added to the WebKit tree (`OptionsZeroG.cmake`, `PlatformZeroG.cmake`, ...), mirroring its layout |
| `patches/` | The patch series on upstream files, applied in `series` order |
| `deps/` | The vcpkg manifest, triplets, toolchains and overlay ports of the dependencies |
| `tools/` | Build and maintenance scripts |
| `tests/` | Render tests, C API tests, link tests, fuzz targets, benchmarks, tools tests |
| `examples/` | Example applications |
| `docs/` | This documentation |
| `third_party/webkit/` | The WebKit checkout at `WEBKIT_REVISION` (not committed) |

## Rules

- **Upstream changes go in `patches/`; new code goes in ZeroG's own
  directories.** The size of the patch series is the port's health metric:
  every patch is something to carry through each WebKit update. A new file in
  the WebKit tree goes in `port/overlay/`; `tools/webkit.py prepare` refuses an
  overlay file that would replace an upstream one.
- **Derived code keeps its origin.** Code copied or adapted from WebKit or
  elsewhere keeps its license header and gets a line in
  [PROVENANCE](../PROVENANCE.md).
- **The C API grows by appending.** New functions, enum values at the end,
  fields at the end of config structs; a note in
  [Changes to the C API](../api-changes.md). The reference pages are generated
  from the header (`python tools/apidoc.py`); CI fails if they are stale.
- **Every change is tested on Windows and Linux** before it lands: the full
  [test suite](testing.md) on the regular build, in the profiles it touches.

## Working on the engine

```sh
python tools/webkit.py prepare      # after editing a patch or an overlay file
python tools/build.py build -t ZeroGLoader --profile dev
build/zerog-dev/bin/ZeroGLoader
```

`prepare` gives unchanged files their old modification time back, so only what
a change touches rebuilds. Do not build while it runs: it resets the WebKit
tree first. To turn an edit of a WebKit file into a patch, make the edit in
`third_party/webkit`, save `git diff` of that file as the next numbered patch
in `patches/`, and add it to `patches/series`.

## Documentation

The site is built with MkDocs Material:

```sh
pip install mkdocs-material
python tools/apidoc.py      # regenerate the API reference from the header
mkdocs serve
```
