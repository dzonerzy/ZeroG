# Concepts

## The objects

| Object | What it is | Created with |
| --- | --- | --- |
| `zg_runtime` | The engine in this process: JavaScript VM, threads, caches. One per process. | `zg_runtime_create` |
| `zg_context` | A session: cookies, storage and caches in a data directory, the schemes it serves, fonts, trust rules. Views of one context share them. | `zg_context_create` |
| `zg_view` | One page: loading, size and scale, input, rendering, JavaScript. | `zg_view_create`, or `zg_window_view` |
| `zg_surface` | Where a view renders: CPU pixels or GPU textures for your renderer. | `zg_view_surface` |
| `zg_window` | A native window owning one view, presented by the engine. | `zg_window_create` |

Destroying an object destroys what it owns: `zg_runtime_destroy` the contexts
left, `zg_context_destroy` its windows and views, `zg_window_destroy` its view.

## Three ways to show a page

- **Window mode.** `zg_window_create` gives you a window and its view. The
  engine renders at the display's rate, takes input from the window and
  presents. You run `zg_runtime_run`. See [Window mode](window-mode.md).
- **Texture mode.** A view on a GPU surface (`zg_surface_config.kind`: D3D11,
  D3D12, D3D9, Vulkan, GL). Your loop calls `zg_view_render` once per frame
  and takes frames with `zg_surface_acquire`, on any thread, to composite them
  yourself; you feed input with `zg_view_mouse_event` and friends. See
  [Texture mode](texture-mode.md).
- **CPU mode.** A view with no surface config: frames are BGRA pixels in
  memory. Same loop as texture mode.

## One thread

Every call happens on the thread that created the runtime, the runtime
thread, and every callback arrives there. Calls from another thread fail with
`ZG_ERROR_WRONG_THREAD` or do nothing. The exceptions are marked "any thread"
in the reference: quitting the loop, taking and returning frames, settling
Promises, answering scheme requests, authentication challenges and file
choosers, releasing JavaScript references.

Work that takes time (a database query, a file read) belongs on your own
threads; answer with a Promise or a scheme response from there. The engine has
its own threads (garbage collection, JIT compilers, network, image decoding,
compositing) and never runs your code on them.

## The event loop

- `zg_runtime_run` runs until `zg_runtime_quit` (from any thread) or until
  the last window closes.
- An application with its own loop calls `zg_runtime_update` instead, every
  iteration, and can wait on `zg_runtime_wakeup_handle` (a Windows event or a
  Linux file descriptor) to sleep until the engine has work.

## Errors

Functions that can fail return a `zg_result`; constructors return NULL.
`zg_last_error()` gives a readable message for the last failure on the calling
thread. The engine's own diagnostics (a GPU that cannot be used, a page's
problem) go to `zg_runtime_config.log`.

## Configuration structs

Every config struct starts with `size`, set to `sizeof` the struct. Zeroed
fields mean "default". New API versions append fields: a program compiled
against an older header keeps working, because the engine reads only as much
as `size` says.
