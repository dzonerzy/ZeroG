# Building

A full build compiles WebKit and every dependency: expect a few hours the first
time on a desktop machine and about 100 GB of disk for one profile. Later builds
only redo what changed.

## Toolchain

The toolchain is pinned in
[`toolchain.json`](https://github.com/dzonerzy/ZeroG/blob/main/toolchain.json);
`tools/build.py` checks it before every step and refuses to run with another
one.

=== "Windows"

    - Visual Studio 2022 with the C++ workload (the Windows SDK, the linker's
      libraries; the compiler is the pinned clang-cl, not MSVC).
    - Python 3.11 or later, Git with `core.longpaths` and
      `core.autocrlf=false` on the checkout, and long paths enabled in Windows.
    - Once: `python tools/toolchain.py fetch` downloads and verifies the pinned
      LLVM and Perl into `third_party/toolchains/`.

=== "Linux"

    Linux builds run in a Docker image built from the manifest, with zig's
    clang targeting an old glibc, so the binaries run on any recent
    distribution:

    ```sh
    python tools/toolchain.py docker-build
    ```

## Sources

```sh
python tools/webkit.py fetch     # shallow fetch of WEBKIT_REVISION (~2 GB)
python tools/webkit.py prepare   # reset, apply patches/series, copy port/overlay
```

## Build

=== "Windows"

    ```sh
    python tools/build.py all --profile minimal
    ```

=== "Linux"

    ```sh
    docker run --rm -v "$PWD:/src:ro" -v zerog-linux-build:/build zerog-linux-builder \
        python3 tools/build.py all --profile minimal --build-dir /build/zerog-minimal
    ```

`all` runs these steps, which can also run one by one:

| Step | What it does |
| --- | --- |
| `deps` | Static dependencies into `third_party/sysroot` (vcpkg, `deps/vcpkg.json`) and the filtered ICU data |
| `configure` | CMake on WebKit with the ZeroG port into `build/zerog-<profile>` |
| `build` | Ninja: the engine, the tests and the examples (`-t TARGET` for one) |
| `bundle` | The engine and its dependencies in one archive, `lib/zerog-engine.lib` or `lib/libzerog-engine.a`, where only the `zg_*` API stays public |
| `linktest` | C programs linked against the bundle the way applications link it, and run |

`--profile minimal|standard|dev|debug` selects the [profile](../guides/profiles-and-size.md).

## Linking your application

An application links two things: `zerog-engine.lib` (or `libzerog-engine.a`)
and the system libraries listed in `lib/zerog-engine.libs.txt` next to it.
Nothing else: ICU, curl, BoringSSL, Skia, SQLite and the rest are inside the
bundle, with their symbols hidden, so they cannot clash with your own copies.

=== "Windows (clang-cl or MSVC)"

    ```sh
    cl /MT app.c app_assets.c /I include /link zerog-engine.lib <libraries from zerog-engine.libs.txt>
    ```

    The engine uses the static C runtime (`/MT`); so must the application.

=== "Linux"

    ```sh
    cc app.c app_assets.c -I include libzerog-engine.a -lpthread -ldl -lm -o app
    ```

    The bundle carries its own C++ runtime: a C application links with `cc`.
    Only glibc stays dynamic.
