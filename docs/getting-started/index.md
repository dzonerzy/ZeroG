# Getting started

ZeroG is built from source: the engine is WebKit at a pinned revision with a
small patch series, its dependencies come from vcpkg, and the result is one
static archive your application links.

1. [Build the engine](building.md) on Windows, or on Linux in the Docker
   builder.
2. [Write your first app](first-app.md): a window, a frontend packed into the
   binary, a native function the page calls.

What you need to know before you start:

- **One runtime per process.** `zg_runtime_create` once, on the thread that
  will run the engine; every call happens on that thread, except the few
  marked "any thread" in the [conventions](../reference/index.md#conventions).
- **Your frontend is part of your binary.** `tools/assets.py` packs a
  directory (your frontend's build output) into C source you compile in;
  `zg_context_mount_assets` serves it at `app://localhost/`.
- **Pick a profile.** `minimal` for the smallest binary (no media, WebGL,
  WebAssembly or service workers), `standard` for the full web platform, `dev`
  for development with the Web Inspector. See
  [Profiles and binary size](../guides/profiles-and-size.md).
