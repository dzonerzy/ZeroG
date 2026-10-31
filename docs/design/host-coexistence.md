# Coexisting with the host application

A single-process engine lives inside someone else's process: a game, an app with its own runtime (Go, .NET, Python, Node), a crash handler. WebKit instead is designed to have a process all to itself. This section lists the process-global resources that WebKit uses, and how the engine makes them compatible with the host.

## Memory allocator

- WebKit uses its own allocator (libpas and bmalloc) with Gigacage, which [reserves huge amounts of address space](https://blogs.gnome.org/mcatanzaro/2018/11/02/on-webkit-build-options-also-how-to-accidentally-disable-important-security-features/) and requires overcommit. It is a source of conflicts with hosts that manage memory on their own.
- **Decision: mimalloc on all platforms** (`USE_MIMALLOC=ON`), the copy included in WebKit (`Source/bmalloc/mimalloc`). Same allocator and same behavior on Linux, Windows and macOS. The system malloc is not an option on Windows: `BPlatform.h` rejects it with an `#error`, because aligned memory cannot be freed with `::free`.
- mimalloc does not replace the host's malloc (upstream builds it with `MI_OVERRIDE=OFF`) and its symbols stay private in the bundle, so a host that itself uses mimalloc does not conflict (verified by [`tests/link/host_conflicts.c`](https://github.com/dzonerzy/ZeroG/blob/main/tests/link/host_conflicts.c)).
- **Consequence**: Gigacage, the isolated heaps and the other protections of WebKit's heap (libpas) are lost. Without a sandbox, it is one less defense against memory bugs; external content is even more discouraged.
- **Optional hardening**: `ZEROG_MIMALLOC_SECURE` enables mimalloc's secure mode (guard pages, randomized allocation, detection of double free and free list corruption), at a performance cost. Default: off.
- **Virtual memory reservations**: they do not go away with any allocator. JSC always reserves a 4 GB Structure heap on 64-bit, the JIT a 1 GB executable pool on x86\_64, mimalloc 1 GiB arenas. With `vm.overcommit_memory=2` on Linux they count toward the commit limit. Removing libpas only eliminates Gigacage.
- The API exposes the engine's memory statistics (JSC heap, WebCore caches, graphics surfaces), so the host can include them in its own budgets.

## Signals (Linux)

- JSC uses a signal to suspend threads during concurrent GC: by default SIGUSR1, configurable with the `JSC_SIGNAL_FOR_GC` variable. WebKit [checks that nobody else handles that signal](https://lists.webkit.org/pipermail/webkit-unassigned/2021-March/969495.html), and a host that already uses it (in the cited case, Ruby) crashes.
- Bun, which embeds JSC, [moved the default from SIGUSR1 to SIGPWR](https://github.com/oven-sh/WebKit/pull/235) because many npm packages register handlers on SIGUSR1. In the same work it fixed a GC hang when the host installs an alternate signal stack, as ASan and many crash handlers do.
- JSC also handles CPU trap signals (SIGSEGV, SIGBUS, SIGILL) for some features, including WebAssembly memory. The handlers must be chained with the host's, not replaced.
- **Proposal**: the GC signal is chosen in `zg_runtime_config`, not with an environment variable, with the default on a free real-time signal. The CI tests also run with a crash handler and an alternate stack active.
- macOS uses Mach ports and Windows `SuspendThread`, so the problem concerns Linux.

## Symbols in static linking

- If the host already links zlib, libpng, curl or OpenSSL, static linking produces duplicate symbols or, worse, the silent use of the wrong version. BoringSSL exports the same names as OpenSSL.
- **Proposal**: the engine is distributed as **a single archive** in which all dependencies are linked ahead of time (partial link) and all symbols are made local except the `zg_*` API. BoringSSL is additionally built with its symbol prefix.
- The same applies to the C++ runtime: if the host is C++ with another standard library, the engine's libc++ symbols must stay internal.

## Runtime and main thread

- WTF registers the "main thread" only once per process, and WebKit's global state cannot be torn down reliably.
- `zg_runtime` is therefore a **process singleton**: it is created once, and `zg_runtime_destroy` is valid only at application shutdown. The runtime cannot be recreated.
- API calls from a different thread fail with an explicit error in debug builds.

## JIT and platform policies

| Platform | Constraint | Handling |
| --- | --- | --- |
| macOS | With hardened runtime and notarization the JIT requires the `com.apple.security.cs.allow-jit` entitlement and `MAP_JIT` memory | The framework's `package` adds the entitlement |
| Windows | If the host enables the policy that forbids dynamic code (ACG), the JIT cannot allocate executable memory | Detection at startup and fallback to the interpreter |
| Linux | SELinux policy such as denied `execmem` | Detection at startup and fallback to the interpreter |

`zg_runtime_config` has a `jit` option with three values: automatic (default), always on, disabled.

## Engine threads

- Every thread created by the engine has a recognizable name (network, GC, JIT, decoding, worker), so it shows up clearly in the host's profilers and crash dumps.
- The number of GC, JIT and decoding threads is configurable, for hosts that already have their own pool and do not want to compete for cores.
- **Web Workers** run on engine threads, each with its own run loop, and terminate with the view that created them. They must be tested explicitly, because in single-process they share heap and allocator with the host.
