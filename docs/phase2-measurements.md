# Phase 2 measurements

The binding latency of ZeroG.md ("Obiettivi misurabili": an empty call, synchronous
and asynchronous, in microseconds), whose target is set after phase 2. Measured
with `tests/bench/bindings.c`, a C program on the public API only: a CPU view
whose page times its own loops with `performance.now()`, each case warmed up first
so the JIT has compiled the loop.

Machine: AMD Ryzen 9 9950X3D (16 cores, 32 threads), 64 GB RAM. Windows 11 natively;
Linux in the Docker builder on WSL 2. Profile `minimal`, Release, JIT on.

| Case | What is timed | Windows x64 | Linux x86_64 |
| --- | --- | --- | --- |
| `js` | an empty JavaScript function, for comparison | 0.005 µs | 0.005 µs |
| `sync` | `App.noop()`: an empty synchronous binding | 0.03 µs | 0.025 µs |
| `async` | `await App.later()`, its Promise resolved during the call | 0.55 µs | 0.50 µs |
| `thread` | `await App.thread()`, its Promise resolved by a worker thread | 10.5 µs | 54.8 µs |
| `os` | the same two thread switches, without the engine | 6.2 µs | 47.8 µs |

The engine's share of a Promise resolved from another thread is the difference
between `thread` and `os`: about 4 µs on Windows and 7 µs on Linux; the rest is the
operating system waking two threads (slower in the WSL 2 virtual machine).

Run it with `build/zerog-minimal/bin/ZeroGBindingBench [calls]`; the Linux test
runner (`tests/render/run_linux.sh`) runs it too.

## What the first measurement found

The first run gave 42 µs for `sync`, 390 µs for `async` and 190 µs for `thread` on
Windows. Three causes, all fixed:

- Bindings were JSC C API callback objects, and JSC releases the JavaScript lock
  around every C API callback (`JSLock::DropAllLocks`): microtasks drain, heap
  access goes back to the collector, and the lock is taken again. Bindings are now
  JSC native functions (`JSNativeStdFunction`), as WebCore's DOM functions are, and
  the application's function runs with the lock held. 42 µs to 0.03 µs.
- A Promise resolved on the runtime thread waited for a turn of the run loop. It is
  now settled at once; the page's continuation runs as a microtask, as for any
  resolved Promise. Hundreds of µs to 0.55 µs.
- On Windows, every entry into the JavaScript VM moved the stack limit and touched
  every page of the stack below it (`preCommitStackMemory`, which keeps the guard
  page in step with JIT code that grows the stack). Pages stay committed once
  touched; patch 0019 touches only those below what the thread already committed.
  An empty take-and-release of the lock went from 28 µs to 0.2 µs. This was a cost
  of every call from the host into JavaScript, not only of bindings.
