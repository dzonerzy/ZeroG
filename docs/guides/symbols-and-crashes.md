# Symbols and crashes

ZeroG has no crash reporter of its own: it works with yours (Crashpad,
Breakpad, Sentry Native, Windows Error Reporting). What it provides is what a
crash report needs to be readable, and the guarantee that it does not get in
your handler's way.

## Your crash handler stays yours

A crash in your code reaches your crash handler, not the engine's:

- **Windows**: your unhandled exception filter and vectored handlers stay in
  place. JavaScriptCore's own fault handling is a vectored handler that only
  takes faults in its generated code.
- **Linux**: your signal handlers and alternate signal stack stay in place,
  except for `SIGSEGV` and `SIGBUS`: JavaScriptCore installs a fault handler
  for them that keeps yours and calls it for every fault that is not its own.
  Install your crash handler **before** `zg_runtime_create`, so it is chained.
  A handler installed later replaces JavaScriptCore's: then call the previous
  handler (`sigaction`'s old action) for faults that are not yours, as crash
  reporters such as Crashpad and Breakpad do.

JavaScriptCore's garbage collector suspends threads with a signal of its own on
Linux: by default a free real-time signal, never `SIGUSR1` or `SIGUSR2`, or the
one you choose with `zg_runtime_config.gc_signal`. The link test
`host_runtime.c` checks all of this on every build.

## Thread names

Engine threads have names (garbage collector, JIT compilers, network, image
decoders, compositor), as thread descriptions on Windows and thread names on
Linux, so crash dumps and profilers show which thread is which.

## Symbol files

Release builds keep line tables: function names, files and lines, without the
size of full debug information. `tools/symbols.py` moves them out of the
binaries into a symbol store laid out the way debuggers and symbol servers
expect:

=== "Windows"

    PDBs in the symstore layout (`name.pdb/<GUID><age>/name.pdb`) and the
    executables by timestamp and image size, which WinDbg, Visual Studio and
    symbol servers read. The PDBs are separate from the executables already.

=== "Linux"

    Debug files by build ID in GDB's and debuginfod's layout
    (`.build-id/<xx>/<rest>.debug`), and stripped copies of the programs with a
    `.gnu_debuglink` to them.

```sh
python tools/symbols.py --profile minimal --out symbols/ --check
```

`--check` symbolizes an address of the example app from the store. Keep each
release's store: a crash report from that release is symbolized with it. For
your own application, link with the same kind of debug information (line
tables) and add its symbols to the same store.
