# Changes to the C API

`include/zerog.h` is versioned with `ZG_VERSION_MAJOR.MINOR.PATCH` (also
`zg_version()`). Within 0.x a minor version may add functions, enum values and
fields at the end of config structs (clients set `size`, so older ones keep
working); renaming, removing or changing what something means takes a new minor
version and an entry here.

## 0.3.0 (in progress)

Engine features of roadmap phases 3 and 4:

- Storage: localStorage, sessionStorage and IndexedDB work, kept in `data_dir`
  (`LocalStorage/`, `IndexedDB/`), in memory for ephemeral contexts. No new
  function: before, `window.localStorage` was null and IndexedDB never answered.
- HTTP disk cache: `zg_context_config.http_cache_limit` (0 = 64 MB, negative =
  none), in `data_dir/HTTPCache/`.
- Clipboard: `zg_runtime_set_clipboard` connects the application's clipboard
  (`zg_clipboard`: write, has, read through `zg_clipboard_reply_data`); by
  default the system's once a window exists, else one in process.
  `navigator.clipboard` works.
- File chooser: `zg_view_callbacks.file_chooser` and `zg_file_chooser_*`; window
  mode shows the system's dialog when there is no callback.
- Drag and drop: `zg_view_drag` for drags from the host (files, text, URLs);
  window mode takes drops on the window; drags in the page (draggable elements,
  links, images, selections) and selecting text by dragging the mouse work.
- Certificates: `zg_runtime_add_ca_certificates` trusts PEM root certificates
  on top of the system's, on every OS (Windows too, where curl reads only the
  system store). An untrusted certificate fails the load with "certificate not
  trusted: <reason>" in `zg_last_error()`, for pages, fetches and wss.
- Web Inspector (dev and debug profiles): `zg_view_show_inspector` opens it in a
  window of its own (F12 and Ctrl+Shift+I in window mode);
  `zg_runtime_config.inspector_port` starts the remote inspector's server on
  127.0.0.1. `ZG_ERROR_UNSUPPORTED` in the other profiles.
- GPU compositing: GPU views (D3D11 and GL surfaces, window mode) composite
  their layers on the runtime's compositor thread (one for every view), with
  upstream's coordinated graphics over Skia. CSS animations of transform,
  opacity and filters run there without rendering updates. In texture mode a
  GPU view's frame now reaches `zg_surface_acquire` shortly after
  `zg_view_render` returns, rather than before; while the previous frame is
  still being composited the call does nothing. Window mode presents on the
  compositor thread: the engine thread no longer waits for vsync. CPU views are
  unchanged.
- Window mode with several windows: each window gets its own events. Before,
  whichever window polled first took every window's events.
- WebGL (standard, dev and debug profiles, Windows): WebGL and WebGL 2 in GPU
  views, their canvases composited as layers; CPU views have none. On Linux
  WebGL is not built yet (it needs ANGLE there). Before, these profiles did not
  link on Windows nor configure on Linux.

## 0.2.0

Additions for frameworks built on the API (one in Zig, for instance):

- Page scripts: `zg_view_add_script` runs a script in every page that gets the
  bindings, after them and before the page's own scripts.
- Binary values: `zg_call_arg_bytes` reads (and may write) an ArrayBuffer, typed
  array or DataView in place; `zg_value_bytes` and `zg_value_bytes_external`
  return a Uint8Array, copied or over the application's memory;
  `zg_promise_resolve_bytes`. Such arguments now have the type `ZG_TYPE_BYTES`
  (`ZG_TYPE_OBJECT` in 0.1).
- References: `zg_ref` keeps a JavaScript value past the call (`zg_call_arg_ref`),
  returns it as itself (`zg_value_ref`) and calls functions later
  (`zg_ref_call`). Value constructors accept a NULL call for values the caller
  owns, as `zg_ref_call` arguments.
- Classes: `zg_view_define_class`, `zg_class_add_method`, `zg_class_add_property`,
  `zg_call_set_native`, `zg_call_native`, `zg_value_new_instance`; instances carry
  a native pointer finalized once, after collection or when the view goes.
- `zg_view_load_html` gives each document a new window object; before, a second
  document of the same origin kept the first one's globals.

## 0.1.0

The API of ZeroG.md ("API pubblica C") at the end of roadmap phase 2, frozen.
Changes from the drafts before it:

- `zg_runtime_config.decode_threads` became `sync_image_decoding` (a flag: images
  decode on the runtime thread).
- `zg_runtime_quit` works from any thread and wakes a sleeping `zg_runtime_run`;
  a quit asked for before `zg_runtime_run` makes it return at once.
- `zg_call_arg_int64` saturates Numbers outside the int64 range (NaN gives 0).
- Strings passed to JavaScript keep embedded NULs (`zg_value_string_len`,
  `zg_value_json`, `zg_promise_resolve`).
- Documented: `zg_view_load_html` replaces the document at once and reports no
  `load_finished`; `zg_memory_stats.js_heap_bytes` is what the last collection
  found live; work the application answers stays valid after its view or context
  is gone; a Promise keeps its page's objects alive until it is settled.
