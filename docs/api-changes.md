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
