# Testing, CI and upstream

The most important test is a single one: **the same page rendered the same way on all platforms**. With a single port it is finally measurable, and it must be measured at every commit.

## Test levels

| Level | What it verifies | How |
| --- | --- | --- |
| C API | Creation and destruction, errors, callbacks | Unit tests in CPU mode |
| Rendering | Visual output of reference pages | CPU and texture mode, comparison with reference images with tolerance, the same for all OSes |
| Bindings | Value conversion between JSC and host for every supported type | Tests generated from the framework's type table |
| Embedding layer | Navigation, forms, events, storage | Dedicated test pages, driven by scripts |
| Memory | Leaks, use-after-free, view creation and destruction cycles | Build with ASan and LSan in the nightly CI |
| Fuzzing | Value conversion, custom schemes, loader | libFuzzer on the project's code |
| Web compatibility | Regressions at every WebKit rebase | Subset of Web Platform Tests |

## CI

- Initial targets: Linux x86\_64 and Windows x64, then macOS arm64, then Linux aarch64.
- WebKit artifact cache per revision and configuration.
- Full nightly build for every profile (minimal, standard, dev).
- Every release produces: example binaries, corresponding source, license file, example relink kit.

## Symbols and crashes

- Symbols separate from the binary (debug files on Linux, PDB on Windows, dSYM on macOS), indexed by build ID and published for every release.
- The engine does not include a crash reporter: it integrates with the host's (Crashpad, Breakpad, Sentry Native), and its threads have recognizable names in the dumps.
- On Linux crash handlers install an alternate signal stack, which interferes with JSC's GC (see [Coexisting with the host application](host-coexistence.md)). The CI includes a test with an active crash handler.
- A symbolication script turns a minidump into readable stack traces with the release's symbols.

## Upstream strategy

- The single-process embedding layer is unlikely to be accepted upstream, given that WebKit1 was removed for non-Apple ports. It stays in the project's repository.
- The generic patches should instead be proposed upstream: portability of `RunLoopGeneric`, static build, fixes to WebCore's Skia and curl code.
- Update of the `main` commit every 2–4 weeks, with the nightly job flagging breakages in advance.
- The health metric is the size of the patch series on upstream files, distinct from the project's new code.
