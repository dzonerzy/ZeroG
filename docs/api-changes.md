# Changes to the C API

`include/zerog.h` is versioned with `ZG_VERSION_MAJOR.MINOR.PATCH` (also
`zg_version()`). Within 0.x a minor version may add functions, enum values and
fields at the end of config structs (clients set `size`, so older ones keep
working); renaming, removing or changing what something means takes a new minor
version and an entry here.

## 0.5.0

What desktop applications need of their windows:

- **Window control:** `zg_window_set_size`/`get_size` (logical pixels),
  `set_min_size`/`set_max_size`, `set_resizable`, `set_position`/`get_position`
  and `center` (desktop coordinates; `ZG_ERROR_UNSUPPORTED` on Wayland),
  `minimize`, `maximize`, `restore`, `show`, `hide`, `focus`,
  `set_always_on_top`, and `zg_window_get_state` with `zg_window_state`.
  `zg_window_config.hidden` creates a window hidden until `zg_window_show`.
- **Window events:** `zg_window_callbacks` with `zg_window_set_callbacks`:
  `close_requested` (return 0 to keep the window open), `resized`, `moved`,
  `focus_changed`, `state_changed`, `menu_item`, `screen_changed`, and
  `files_dropped` (the paths of files dropped on the window; the application
  may keep the drop from the page).
- **Icons:** `zg_window_set_icon` (RGBA pixels). On Windows windows have the
  executable's icon by default. On Linux the context's `app_id` is its windows'
  Wayland app_id and X11 class, so desktops match them to the application's
  `.desktop` file (and its icon).
- **Light and dark:** `zg_view_set_color_scheme` (`zg_color_scheme`: system,
  light, dark) for `prefers-color-scheme`, `color-scheme` and the look of form
  controls and scrollbars; by default views follow the desktop's setting live
  once a window exists, and a window's frame follows its view (a dark title bar
  on Windows). `zg_system_color_scheme` gives the desktop's setting.
- **The clipboard from native code:** `zg_clipboard_read` and
  `zg_clipboard_write`, on the clipboard pages use.
- **Select dropdowns:** `<select>` elements open a dropdown. In window mode the
  engine shows its own (a popup window drawn like the page, keyboard and
  mouse); a `select_menu` view callback (`zg_select_menu`: the element's box,
  font size, direction, items with optgroup labels and separators;
  `zg_select_menu_choose`, `_cancel`) lets the application show its own, and
  is texture mode's only one.
- `zg_window_popup_menu` reports a dismissed popup as `menu_item` with id 0.
- **Screens:** `zg_screen_count`, `zg_screen_get` (`zg_screen`: bounds, work
  area, scale, refresh rate, primary, name) and `zg_window_screen`.
- **Dialogs:** `zg_window_file_dialog` (open files, open a folder, save; title,
  accept label, location, filters, several files) and `zg_window_message_box`
  (info, warning, error; custom buttons with Enter and Escape defaults). The
  answers come to a callback, always once, never during the call.
- **Native menus** (Windows; `ZG_ERROR_UNSUPPORTED` elsewhere): `zg_menu`
  descriptions with items (ids, `&` access keys, accelerators such as
  `"Ctrl+S"`, checkbox, checked, disabled), separators and submenus;
  `zg_window_set_menu` for the menu bar, `zg_window_menu_set_checked` and
  `_set_enabled`, `zg_window_popup_menu`.

Changes in behavior, for every window:

- `zg_window_config.width` and `height` are logical pixels as documented: on a
  screen scaled to 150%, a 400x300 window now has 600x450 device pixels
  (before, the size was taken as device pixels on Windows and X11).
- A window the user closes is hidden at once (before, it stayed on screen until
  the application destroyed it), and closing the last window no longer ends
  `zg_runtime_run` through SDL's quit event before the application could see
  the request.
- `zg_window_destroy` may be called from callbacks, also for the window whose
  event or page is running: the window goes once the loop is out of its work.
- Pages see the desktop's dark setting through `prefers-color-scheme` (before,
  always light).
- On Windows the clipboard is used directly, in the system's formats: text,
  "HTML Format" (what browsers and Office read and write), PNG, bitmaps, and
  other types under their names. Before, through SDL, `text/html` read back as
  the plain text, a page's copy could put its HTML in the plain text, and a
  read right after another application's change could find nothing.
- `<select>` elements open (before, nothing happened).
- A window shown or uncovered draws again: before, a window whose first frame
  came while it was still hidden (every window but the first, with a page that
  does not change) stayed white.
- Checkboxes draw an inset check mark, readable at small sizes.

Nothing else changed for 0.4 programs.

## 0.4.0

A page's objects read and changed from native code, through references:

- `zg_view_global` gives the main frame's global object (`window`) as a
  `zg_ref`.
- `zg_ref_get` (a reference), `zg_ref_get_bool`, `zg_ref_get_number`,
  `zg_ref_get_string` and `zg_ref_get_json` read a property or a dotted path of
  them (`"document.title"`, `"style.color"`, `"items.0"`); `zg_ref_set` sets one
  to a value made with a NULL call (bytes and references too);
  `zg_ref_call_method` calls a method with `this` its object and returns the
  result as a reference; `zg_ref_to_bool`, `_number`, `_string` and `_json` read
  a reference's own value.
- Typed reads are strict: another type fails with the new `ZG_ERROR_TYPE` and
  both types in `zg_last_error()`. A missing step on a path, a reference whose
  page is gone, and a method that does not exist give `ZG_ERROR_NOT_FOUND`;
  getters, setters and methods that throw `ZG_ERROR_JS_EXCEPTION`.
- `zg_free` frees the strings these functions return.

Nothing changed for 0.3 programs: every addition is new.

## 0.3.0

Frozen. The engine features of roadmap phases 3 and 4:

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
  inside them, navigation preload, and script updates (a controlled navigation
  checks the script; a changed one is installed). Registrations are kept in
  `data_dir/ServiceWorkers/` (in memory for ephemeral contexts). No push,
  notifications or background fetch. No new function. Before, `navigator.serviceWorker` was
  absent (the minimal profile still has none).
- HTTP disk cache: `zg_context_config.http_cache_limit` (0 = 64 MB, negative =
  none), in `data_dir/HTTPCache/`.
- Clipboard: `zg_runtime_set_clipboard` connects the application's clipboard
  (`zg_clipboard`: write, has, read through `zg_clipboard_reply_data`); by
  default the system's once a window exists, else one in process.
  `navigator.clipboard` works.
- File chooser: `zg_view_callbacks.file_chooser` and `zg_file_chooser_*`; window
  mode shows the system's dialog when there is no callback.
- Context menus: `zg_view_callbacks.context_menu` and `zg_context_menu_*` (the
  position, what is under it, WebCore's items as `zg_context_menu_item`;
  `zg_context_menu_select` runs one, `zg_context_menu_cancel`). The engine
  draws no menu; without the callback none appears. Before, right clicks
  showed nothing either.
- Windows: `zg_window_config.transparent` (the page's transparent parts show
  what is behind the window) and `click_through` (clicks there go behind it;
  Windows and X11, at the cost of a CPU copy of each frame); frameless
  windows move by the page's regions marked `--zg-app-region: drag` and
  resize by their edges.
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
- Texture mode for games on Windows, beyond D3D11:
  - `ZG_SURFACE_D3D12` takes the game's `ID3D12CommandQueue`. Frames are
    `ID3D12Resource`s opened on its device. Acquire queues the frame's wait on
    that queue and release queues the signal that the queue is done with it, so
    neither the game's nor the engine's CPU threads ever wait.
  - `ZG_SURFACE_VULKAN` on Windows: frames are NT handles of a texture and of
    two timeline fences (`zg_surface_win32`, `zg_win32_frame`), for Vulkan's
    Win32 external memory and semaphores, or any API that opens them. The game
    waits for the ready value and signals the release value.
  - `ZG_SURFACE_D3D9` takes the game's `IDirect3DDevice9`. A 9Ex device on the
    engine's adapter opens the engine's textures. Any other device gets the
    pixels that changed since its previous frame copied into a texture of its
    own. `zg_surface_device_lost` comes before `IDirect3DDevice9::Reset`.
  - `zg_runtime_config.adapter_luid` picks the GPU the engine renders on (the
    game's). Devices on another adapter are refused with a reason in
    `zg_last_error`; before, they failed when opening the textures. D3D11
    surfaces on another adapter now fail with `ZG_ERROR_UNSUPPORTED` (was
    `ZG_ERROR_GPU`), and so do the other kinds.
  - Before, these kinds were refused or (Vulkan) Linux-only.
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

The API of the [design notes](design/c-api.md) at the end of roadmap phase 2, frozen.
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
