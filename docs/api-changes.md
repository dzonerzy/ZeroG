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
- Cache API (standard, dev and debug profiles): `caches` in pages and workers,
  kept in `data_dir/CacheStorage/` (in memory for ephemeral contexts), 512 MB
  per origin. No new function. Before, `caches` was absent (the minimal profile
  still has none).
- Service workers (standard, dev and debug profiles) for http and https pages:
  registration, install and activate, fetch events for navigations and
  subresources (answered or left to the network), messages, the Cache API
  inside them. Registrations are kept in `data_dir/ServiceWorkers/` (in memory
  for ephemeral contexts). No push, notifications, background fetch or
  navigation preload. No new function. Before, `navigator.serviceWorker` was
  absent (the minimal profile still has none).
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
  unchanged. In texture mode these animations are paced by the application:
  one composited frame per `zg_view_render`, at its `frame_time_ms` (they stop
  without calls), and `zg_view_set_frame_rate` caps them (a game at 144 fps
  may keep its UI at 30); a window follows its display.
- Accessibility: window mode exposes the page to screen readers, through UI
  Automation on Windows and AT-SPI on Linux (libdbus is loaded at run time;
  nothing without a session bus, or with `NO_AT_BRIDGE=1`). Controls have
  their names, roles, states and bounds; buttons and links can be pressed, text
  fields read (by character, word and line too: UI Automation's Text pattern,
  AT-SPI's Text) and set without moving the focus, check boxes toggled, ranges
  read and set, and focus,
  value, name, state and structure changes are announced. No new function;
  texture-mode views have no bridge yet.
- Media (standard, dev and debug profiles): `<audio>` and `<video>` play WebM
  (VP8, VP9, AV1 with Opus or Vorbis), Ogg (Opus, Vorbis), MP3, FLAC and WAV,
  from any scheme the page loads from; sound goes to the system's default
  device. No new function. Before, media elements reported that they could not
  play (the minimal profile still does).
- MP4 on Windows (standard, dev and debug profiles): H.264 and AAC through the
  system's Media Foundation, where Windows has it (not in "N" editions);
  `canPlayType` says so. MP4 files with VP9, AV1 or Opus play everywhere.
- AVIF images (standard, dev and debug profiles), through libavif and dav1d.
  No new function. Before, AVIF images did not decode (the minimal profile
  still does not).
- `<video controls>` shows WebKit's native controls with their icons (before,
  the buttons had none).
- GPU views: 2D canvases draw on the GPU, and video frames reach the GPU as YUV
  planes converted there. On Windows H.264 decodes on the GPU (DXVA) where it
  can. `getVideoPlaybackQuality()` reports shown and dropped frames.
- Media Source Extensions (standard, dev and debug profiles): `MediaSource` and
  `SourceBuffer` for WebM (VP8, VP9, AV1, Opus, Vorbis) and fragmented MP4 (VP9,
  AV1, Opus; on Windows H.264 and AAC through Media Foundation), several buffers
  per source, seeking within what is buffered. In composited views a video is a
  layer of its own. Before, `MediaSource` was absent (the
  minimal profile still has none).
- Web Audio (standard, dev and debug profiles): `AudioContext` plays on the
  system's default device, `OfflineAudioContext` renders, `decodeAudioData`
  takes the formats `<audio>` plays, `AnalyserNode`, `ConvolverNode` and
  `PannerNode` (HRTF too) work, and `createMediaElementSource` takes an
  `<audio>` or `<video>`'s sound into the graph. No new function. Before, Web Audio was absent
  (the minimal profile still has none).
- dma-buf frames (Linux): `ZG_SURFACE_VULKAN` views render on the engine's own
  GPU into dma-bufs, for Vulkan (VK_EXT_external_memory_dma_buf) or any API
  that imports them. `zg_surface_dmabuf` gives a frame's format, modifier and
  planes (descriptors, offsets, strides), its sync_file and a buffer ID that
  stays with the buffer; `zg_surface_release_with_fence` hands back the
  application's own sync_file. The buffers come from the driver (EGL export),
  GBM, or a DRM device's dumb buffers, whichever the system has first;
  `ZG_ERROR_UNSUPPORTED` with none. Before, the kind was declared and refused.
- Window mode with several windows: each window gets its own events. Before,
  whichever window polled first took every window's events.
- WebGL (standard, dev and debug profiles): WebGL and WebGL 2 in GPU views,
  their canvases composited as layers; CPU views have none. On Linux ANGLE runs
  over the system's EGL driver, on the display the engine opened, the window's,
  or the application's in GL texture mode, which then says how it opened it
  (`zg_surface_config.egl_platform` and `egl_native_display`; without them
  pages have no WebGL there). Before, these profiles did not link on Windows
  nor configure on Linux.

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
