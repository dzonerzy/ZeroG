# Changes to the C API

`include/zerog.h` is versioned with `ZG_VERSION_MAJOR.MINOR.PATCH` (also
`zg_version()`). Within 0.x a minor version may add functions, enum values and
fields at the end of config structs (clients set `size`, so older ones keep
working); renaming, removing or changing what something means takes a new minor
version and an entry here.

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
