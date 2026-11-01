/*
 * ZeroG — public C API.
 *
 * Version 0.5: the API of docs/design/c-api.md as frozen at the end of
 * roadmap phase 2 (0.1), with binary values, references to JavaScript values,
 * classes and page scripts (0.2), the engine features of phases 3 and 4 (0.3,
 * frozen): storage, clipboard, file chooser, drag and drop, context menus,
 * windows (transparent, frameless, click-through), the Web Inspector, and
 * texture mode for every graphics API games use; a page's objects read and
 * changed from native code through references (0.4); and what desktop
 * applications need of their windows (0.5): control and events, screens, the
 * system's dialogs, native menus. Within 0.x a minor version
 * may add functions, enum values and fields at the end of config structs;
 * nothing is renamed or removed and no meaning changes without a new minor
 * version and a note in docs/api-changes.md.
 *
 * Rules that hold for every function below:
 *   - C99, opaque handles, no WebKit type ever appears here.
 *   - Single thread: every call happens on the thread that created the
 *     runtime (the runtime thread), and every callback is delivered on that
 *     thread. Calls from another thread fail with ZG_ERROR_WRONG_THREAD or do
 *     nothing. The exceptions are marked "any thread": zg_runtime_quit,
 *     zg_runtime_now_ms, the zg_surface_* calls on frames (acquire, release,
 *     zg_surface_dmabuf, zg_surface_win32, zg_surface_device_lost; a D3D9
 *     surface's on its device's thread), settling a zg_promise,
 *     zg_ref_release, zg_runtime_add_ca_certificates, answering a
 *     zg_scheme_request, a zg_auth_challenge or a zg_file_chooser;
 *     zg_last_error is per thread. Release callbacks (zg_context_add_file,
 *     zg_value_bytes_external) and the engine's own log messages may come on
 *     any thread.
 *   - Config structs start with `size`; set it to sizeof(the struct) so new
 *     fields can be appended without breaking existing clients. Zeroed fields
 *     mean "default".
 *   - Functions that can fail return zg_result; zg_last_error() gives a
 *     readable message for the last failure on the calling thread.
 *   - Strings are UTF-8. Where a length parameter exists, the string need not
 *     be NUL-terminated; otherwise it must be. Strings the engine returns stay
 *     valid as documented for each, at least until the next call on the object.
 *   - Lifetimes: zg_runtime_destroy destroys the contexts left, and
 *     zg_context_destroy its windows and views. A window owns its view. Work the
 *     application answers (scheme requests, auth challenges, promises) stays
 *     valid after its view or context is gone and must still be answered: the
 *     answer is then dropped and the object freed.
 */
#ifndef ZEROG_H
#define ZEROG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ZG_SHARED)
#  if defined(_WIN32)
#    if defined(ZG_BUILDING)
#      define ZG_API __declspec(dllexport)
#    else
#      define ZG_API __declspec(dllimport)
#    endif
#  else
#    define ZG_API __attribute__((visibility("default")))
#  endif
#else
#  define ZG_API
#endif

#define ZG_VERSION_MAJOR 0
#define ZG_VERSION_MINOR 5
#define ZG_VERSION_PATCH 0

/* ------------------------------------------------------------------------ */
/* Handles                                                                   */

typedef struct zg_runtime zg_runtime;
typedef struct zg_context zg_context;
typedef struct zg_view    zg_view;
typedef struct zg_window  zg_window;
typedef struct zg_surface zg_surface;
typedef struct zg_value   zg_value;
typedef struct zg_call    zg_call;
typedef struct zg_promise zg_promise;

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */

typedef enum zg_result {
    ZG_OK = 0,
    ZG_ERROR_INVALID_ARGUMENT,
    ZG_ERROR_WRONG_THREAD,      /* called off the runtime thread */
    ZG_ERROR_ALREADY_EXISTS,    /* e.g. a second zg_runtime_create */
    ZG_ERROR_NOT_FOUND,
    ZG_ERROR_UNSUPPORTED,       /* feature compiled out of this build profile, or
                                   not available here (OS, GPU, adapter) */
    ZG_ERROR_OUT_OF_MEMORY,
    ZG_ERROR_GPU,
    ZG_ERROR_JS_EXCEPTION,
    ZG_ERROR_INTERNAL,
    ZG_ERROR_TYPE               /* (0.4) a value of another type than asked for */
} zg_result;

/* Message for the last failed call on this thread; valid until the next call. */
ZG_API const char *zg_last_error(void);

/* Engine and WebKit revision, for update checks ("Politica di aggiornamento"). */
typedef struct zg_version_info {
    int major, minor, patch;
    const char *webkit_revision; /* commit pinned in WEBKIT_REVISION */
    const char *profile;         /* "minimal", "standard" or "dev" */
} zg_version_info;

ZG_API zg_version_info zg_version(void);

/* ------------------------------------------------------------------------ */
/* Logging and diagnostics                                                   */

typedef enum zg_log_level {
    ZG_LOG_DEBUG,
    ZG_LOG_INFO,
    ZG_LOG_WARNING,
    ZG_LOG_ERROR
} zg_log_level;

typedef enum zg_log_source {
    ZG_LOG_SOURCE_ENGINE,   /* the engine: about a page (network, security, script
                               errors) or its own (GPU, certificates) */
    ZG_LOG_SOURCE_CONSOLE   /* console.log/warn/error from page JavaScript */
} zg_log_source;

typedef struct zg_log_message {
    zg_log_source source;
    zg_log_level level;
    const char *message; size_t message_len;
    const char *file;    size_t file_len;   /* source URL, if any */
    unsigned line, column;
    zg_view *view;                          /* the page's view; NULL for the engine's own */
} zg_log_message;

/* The engine's own messages may come from any thread. */
typedef void (*zg_log_fn)(const zg_log_message *msg, void *user);

/* ------------------------------------------------------------------------ */
/* Runtime: process singleton                                                */

typedef enum zg_jit_mode {
    ZG_JIT_AUTO = 0,   /* on, unless the platform forbids it (ACG, SELinux execmem) */
    ZG_JIT_ALWAYS,     /* fail zg_runtime_create if JIT memory is unavailable */
    ZG_JIT_DISABLED    /* interpreter only, for the whole process */
} zg_jit_mode;

typedef struct zg_runtime_config {
    size_t size;
    zg_jit_mode jit;

    /* Linux only: signal JSC uses to suspend threads during concurrent GC.
       0 picks a free real-time signal. Replaces JSC_SIGNAL_FOR_GC. */
    int gc_signal;

    /* 0 = engine default. Lets hosts with their own pools avoid oversubscription:
       GC marking threads, and compiler threads per JIT tier. */
    unsigned gc_threads;
    unsigned jit_threads;
    /* Nonzero: images decode on the runtime thread, with no decoder threads. */
    int sync_image_decoding;

    /* Soft limits in bytes, 0 = default. Apply to the whole runtime: all views
       share one JavaScript VM and one resource cache. Near js_heap_limit the GC
       collects more often; cache_limit caps WebCore's memory cache. */
    size_t js_heap_limit;
    size_t cache_limit;

    zg_log_fn log;
    void *log_user;
    zg_log_level log_level;

    /* Since 0.3, dev and debug profiles: a nonzero port starts the remote
       inspector's server on 127.0.0.1 (reach it from another machine through a
       tunnel), so a WebKit frontend elsewhere can inspect every view; a warning
       in the log if it cannot listen. Ignored by the other profiles. */
    unsigned inspector_port;

    /* Since 0.3, Windows: the GPU adapter the engine renders on, as its LUID
       (HighPart << 32 | LowPart; DXGI_ADAPTER_DESC.AdapterLuid,
       ID3D12Device::GetAdapterLuid, IDirect3D9Ex::GetAdapterLUID), so a game's
       device on that adapter can take its frames (a laptop's discrete GPU, for
       instance). 0 is the system's default adapter. Texture-mode devices on
       another adapter get ZG_ERROR_UNSUPPORTED, except plain D3D9 (copied). */
    uint64_t adapter_luid;
} zg_runtime_config;

/* Creates the process-wide runtime on the calling thread, which becomes the
   runtime thread. Can be called once per process: a second call, even after
   zg_runtime_destroy, fails with ZG_ERROR_ALREADY_EXISTS. */
ZG_API zg_runtime *zg_runtime_create(const zg_runtime_config *config);

/* Valid only at application shutdown; the runtime cannot be recreated. */
ZG_API void zg_runtime_destroy(zg_runtime *rt);

/* The engine owns the loop until zg_runtime_quit, or until the last window is
   closed. Without windows (CPU and texture views) it runs the engine's work and
   sleeps until there is more. */
ZG_API zg_result zg_runtime_run(zg_runtime *rt);
/* Any thread: ends zg_runtime_run, waking it if it sleeps; called before it, the
   next zg_runtime_run returns at once. */
ZG_API void zg_runtime_quit(zg_runtime *rt);

/* Own-loop mode: run due timers, network responses and pending tasks. */
ZG_API void zg_runtime_update(zg_runtime *rt);

/* Native waitable handle, signaled when the engine has work to do:
   HANDLE (event) on Windows, file descriptor on Linux and macOS. */
typedef intptr_t zg_native_handle;
ZG_API zg_native_handle zg_runtime_wakeup_handle(zg_runtime *rt);

/* Milliseconds until the next engine timer, or -1 if none is scheduled.
   Lets an idle host wait on the wakeup handle with a precise timeout. */
ZG_API int64_t zg_runtime_next_timeout(zg_runtime *rt);

/* The engine's monotonic clock, in milliseconds: the clock of zg_view_render's
   frame times and of performance.now() (from another origin). Any thread. */
ZG_API double zg_runtime_now_ms(void);

typedef struct zg_memory_stats {
    size_t size;
    size_t js_heap_bytes;      /* live at the last collection (zg_runtime_collect_garbage) */
    size_t js_heap_capacity;   /* reserved by the heap */
    size_t resource_cache_bytes;
    size_t surface_bytes;   /* graphics surfaces owned by the engine */
    size_t asset_bytes;     /* files the engine holds: copied, or inflated from archives */
} zg_memory_stats;

ZG_API void zg_runtime_memory_stats(zg_runtime *rt, zg_memory_stats *out);
ZG_API void zg_runtime_collect_garbage(zg_runtime *rt);

/* Root certificates trusted for https and wss on top of the system's (Windows'
   store, the distribution's CA bundle on Linux): a private CA, a development
   server's. PEM, any number in one call; process-wide, for connections set up
   from now on. ZG_ERROR_INVALID_ARGUMENT if none could be read. Any thread.
   Since 0.3. */
ZG_API zg_result zg_runtime_add_ca_certificates(zg_runtime *rt, const void *pem, size_t len);

/* ---- Clipboard (0.3) ----

   The clipboard copy, cut, paste and navigator.clipboard use, process-wide. By
   default the system's once a window exists (window mode), else one in
   process. An application with its own windows (texture mode) connects its own
   with zg_runtime_set_clipboard. Data is by MIME type: "text/plain" and
   "text/html" (UTF-8), "text/uri-list", images ("image/png", ...), and the
   page's own data as "org.zerog.custom-pasteboard-data" (keep it if you can).
   Runtime thread. */
typedef struct zg_clipboard_reply zg_clipboard_reply;
/* The data asked for (copied); not calling it means there is none. */
ZG_API void zg_clipboard_reply_data(zg_clipboard_reply *reply, const void *data, size_t len);

typedef struct zg_clipboard {
    size_t size;
    void *user;
    /* Replace the clipboard's contents with `count` items. */
    void (*write)(size_t count, const char *const *types, const void *const *data, const size_t *lens, void *user);
    /* Whether it holds data of `type`. */
    int  (*has)(const char *type, void *user);
    /* The data of `type`, through zg_clipboard_reply_data, during the call. */
    void (*read)(const char *type, zg_clipboard_reply *reply, void *user);
} zg_clipboard;

/* NULL goes back to the default. Copied. */
ZG_API zg_result zg_runtime_set_clipboard(zg_runtime *rt, const zg_clipboard *clipboard);

/* ------------------------------------------------------------------------ */
/* Context: session state (data dir, schemes, network, fonts)                */

/* Trust decisions an origin can ask for. */
typedef enum zg_trust_request {
    ZG_TRUST_NAVIGATE,   /* load or navigate to a page from this origin */
    ZG_TRUST_BINDINGS    /* receive the native bindings (zg_view_bind) */
} zg_trust_request;

/* Optional policy for origins the default rules do not trust. Return nonzero to
   allow. `main_frame` is zero for iframes. Called on the runtime thread. */
typedef int (*zg_trust_fn)(const char *origin, zg_trust_request request, int main_frame, void *user);

typedef struct zg_context_config {
    size_t size;
    const char *app_id;     /* reverse-DNS id, used for the per-app data directory */
    /* Where the context keeps cookies (Cookies.sqlite), localStorage
       (LocalStorage/) and IndexedDB (IndexedDB/). NULL = OS default from app_id
       (%LOCALAPPDATA%\<app_id>, $XDG_DATA_HOME/<app_id>); with neither, all of it
       lives in memory. Contexts on the same directory share what is in it. */
    const char *data_dir;
    int ephemeral;          /* nonzero: never write cookies or storage to disk */

    /* Trusted origins. By default only the app's own assets are trusted: the schemes
       mounted with zg_context_mount_assets or registered with
       zg_context_register_scheme. Bindings go to the main frame only, and the
       origin is checked again on every call. */

    /* Dev server origin (e.g. "http://localhost:5173"), trusted for navigation and
       bindings. Honored only by the dev and debug engine profiles; ignored in
       release profiles, so it cannot ship to production by mistake. */
    const char *dev_server_url;

    /* Optional host policy for any other origin; NULL = deny. */
    zg_trust_fn trust;
    void *trust_user;

    /* Nonzero: content outside trusted origins may be loaded (navigation, iframes).
       The developer takes responsibility for it: ZeroG adds no sandbox, and the JIT
       follows zg_runtime_config.jit, which is not changed by this flag. Untrusted
       origins never receive bindings. */
    int allow_external_content;

    /* HTTP disk cache in data_dir (HTTPCache/), in bytes: 0 = 64 MB, negative =
       none. Responses are kept and revalidated as HTTP caching says (max-age,
       Expires, no-store, no-cache, Vary, ETag and Last-Modified). Ephemeral
       contexts and those without a directory have none. Since 0.3. */
    int64_t http_cache_limit;
} zg_context_config;

ZG_API zg_context *zg_context_create(zg_runtime *rt, const zg_context_config *config);
ZG_API void zg_context_destroy(zg_context *ctx);

/* The application's files: an in-memory file system per scheme (typically
   "app"), served at scheme://<any host>/<path> with a MIME type from the file
   (else its extension), range requests, HEAD, and index.html for directories.
   Files are write-once: a path, once added, never changes and is never removed.
   A scheme serves either files or a zg_context_register_scheme handler, not both.
   Every scheme of a context is secure (crypto.subtle and other secure-context
   APIs work), CORS-enabled, and trusted (navigation and bindings). Pages should
   use one host, e.g. app://localhost/, since the host is part of the origin. */

/* Mount an archive generated at build time (tools/assets.py) at the root of
   scheme://. The archive is not copied and must stay valid while the context
   lives (typically static data). Fails as a whole with ZG_ERROR_ALREADY_EXISTS
   if one of its paths exists, ZG_ERROR_INVALID_ARGUMENT if it is malformed. */
ZG_API zg_result zg_context_mount_assets(zg_context *ctx, const char *scheme,
                                         const void *archive, size_t archive_len);

typedef void (*zg_release_fn)(void *user);

/* Add one file at `path` ("/index.html", UTF-8). Fails with
   ZG_ERROR_ALREADY_EXISTS if the path exists. With `release` NULL the data is
   copied; otherwise it is used in place, and `release(user)` is called, on any
   thread, once the context and every load using the file are done with it (not
   if the call fails). `mime_type` NULL derives the type from the extension
   (text is UTF-8). */
ZG_API zg_result zg_context_add_file(zg_context *ctx, const char *scheme, const char *path,
                                     const void *data, size_t len, const char *mime_type,
                                     zg_release_fn release, void *user);

/* For single-page applications that route with the URL's path: a request for a
   path with no file whose last segment has no extension ("/settings/profile",
   "/users/") gets the file at `path` ("/index.html") with status 200, under the
   requested URL. Missing files with an extension ("/app.js") stay 404. NULL
   turns it off. */
ZG_API zg_result zg_context_set_fallback(zg_context *ctx, const char *scheme, const char *path);

typedef struct zg_scheme_request zg_scheme_request;

/* Valid until the request is answered. */
ZG_API const char *zg_scheme_request_url(const zg_scheme_request *req, size_t *len);
ZG_API const char *zg_scheme_request_method(const zg_scheme_request *req);
/* NULL if the request has no such header (names are case-insensitive). */
ZG_API const char *zg_scheme_request_header(const zg_scheme_request *req, const char *name, size_t *len);

/* A response header (CORS, caching, ...), before answering, on the thread that
   answers. Content-Type and Content-Length come from zg_scheme_request_respond. */
ZG_API void zg_scheme_request_add_header(zg_scheme_request *req, const char *name, const char *value);

/* Answer exactly once, from any thread; the request is freed then. `body` is
   copied. If the page stopped the load meanwhile the answer is dropped. */
ZG_API void zg_scheme_request_respond(zg_scheme_request *req, int status,
                                      const char *mime_type,
                                      const void *body, size_t body_len);
ZG_API void zg_scheme_request_fail(zg_scheme_request *req, const char *reason);

/* Called on the runtime thread; may answer now or later. */
typedef void (*zg_scheme_fn)(zg_scheme_request *req, void *user);

/* Answers scheme:// with the application's callback. Built-in schemes (http,
   https, file, data, about, blob, ws, ...) cannot be registered. */
ZG_API zg_result zg_context_register_scheme(zg_context *ctx, const char *scheme,
                                            zg_scheme_fn handler, void *user);

/* A TrueType, OpenType, WOFF or WOFF2 font for the context's pages, under the
   family name, weight, style and width inside it, as an @font-face rule every
   page has: a registered family wins over a system font of the same name. Other
   contexts do not see it. The data is copied. Pages already open get it too. */
ZG_API zg_result zg_context_register_font(zg_context *ctx, const void *data, size_t len);

/* ------------------------------------------------------------------------ */
/* Surfaces: rendering targets for texture and CPU modes                     */

typedef enum zg_surface_kind {
    ZG_SURFACE_CPU,       /* BGRA8 premultiplied pixels in memory */
    ZG_SURFACE_D3D11,     /* Windows: engine or host ID3D11Device */
    ZG_SURFACE_GL,        /* Linux: EGL context */
    ZG_SURFACE_VULKAN,    /* Linux: dma-bufs; Windows (0.3): NT handles */
    ZG_SURFACE_METAL,     /* macOS */
    ZG_SURFACE_D3D12,     /* Windows (0.3): host ID3D12CommandQueue */
    ZG_SURFACE_D3D9       /* Windows (0.3): host IDirect3DDevice9 or 9Ex */
} zg_surface_kind;

typedef struct zg_surface_config {
    size_t size;
    zg_surface_kind kind;
    /* Host GPU device for the kind:
       ZG_SURFACE_D3D11: an ID3D11Device* on ANGLE's adapter, or NULL for one the
         engine creates (zg_surface_device). Frames carry the texture opened on it,
         already acquired through its keyed mutex.
       ZG_SURFACE_GL: the application's EGLDisplay (required); frames carry an
         EGLImage for glEGLImageTargetTexture2DOES and an EGLSync to eglWaitSync on.
         Release frames with the context that used them current.
       ZG_SURFACE_VULKAN (0.3), Linux: NULL. Frames are dma-bufs, on the engine's
         own GPU, for Vulkan (VK_EXT_external_memory_dma_buf) or any API that
         imports them: see zg_surface_dmabuf. ZG_ERROR_UNSUPPORTED when the
         driver exports none and no DRM device allocates one it imports.
       ZG_SURFACE_VULKAN (0.3), Windows: NULL. Frames are NT handles of a texture
         and of two timeline fences, for Vulkan (VK_KHR_external_memory_win32,
         VK_KHR_external_semaphore_win32) or any API that opens them: see
         zg_surface_win32. The application signals each frame's release value
         when done with it.
       ZG_SURFACE_D3D12 (0.3): the application's ID3D12CommandQueue*. Frames carry
         an ID3D12Resource* opened on its device (R8G8B8A8_UNORM, state COMMON;
         read it as a shader resource and leave it in COMMON or let it decay
         there). Acquire queues a wait on that queue for the frame, release a
         signal that the queue is done with it: use the frame on that queue, or
         wait for zg_frame.fence (an ID3D12Fence*) at fence_value on another.
       ZG_SURFACE_D3D9 (0.3): the application's IDirect3DDevice9*. Frames carry an
         IDirect3DTexture9* (D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT) to draw with. An
         IDirect3DDevice9Ex gets the engine's texture itself (shared); a plain
         device a copy of the pixels that changed, made in acquire. Acquire and
         release on the device's thread; call zg_surface_device_lost before
         IDirect3DDevice9::Reset.
       The Direct3D kinds and the Windows Vulkan kind need the device on the
       engine's adapter: see zg_runtime_config.adapter_luid. */
    void *device;
    unsigned buffer_count; /* 2 or 3, 0 = 2 */
    /* ZG_SURFACE_GL (0.3): how the application opened its EGLDisplay, the
       platform and native display it gave eglGetPlatformDisplay (for example
       EGL_PLATFORM_WAYLAND_KHR and its wl_display*, EGL_PLATFORM_GBM_KHR and its
       gbm_device*, EGL_PLATFORM_SURFACELESS_MESA and EGL_DEFAULT_DISPLAY).
       WebGL's ANGLE opens the same display with them; with a zero platform pages
       have no WebGL there. Ignored by the other kinds. */
    int egl_platform;
    void *egl_native_display;
} zg_surface_config;

typedef struct zg_frame {
    unsigned width, height;    /* physical pixels */
    /* ZG_SURFACE_CPU */
    const void *pixels;
    size_t stride;
    /* GPU kinds: native texture, fence and (cross-device) shared handle */
    void *texture;
    void *fence;
    uint64_t fence_value;
    zg_native_handle shared_handle;
    /* region changed since the previous acquired frame, physical pixels */
    int dirty_x, dirty_y, dirty_w, dirty_h;
    /* rows run top to bottom (row 0 is the top of the view) in every kind */
    void *internal;            /* engine-owned: identifies the frame in release */
} zg_frame;

/* Any thread: take the newest ready frame, or ZG_ERROR_NOT_FOUND if none.
   The frame stays valid until zg_surface_release. */
ZG_API zg_result zg_surface_acquire(zg_surface *surface, zg_frame *out);
ZG_API void zg_surface_release(zg_surface *surface, const zg_frame *frame);

/* A ZG_SURFACE_VULKAN frame's dma-buf (0.3; Linux): its format and planes, as
   DRM describes them (VkImageDrmFormatModifierExplicitCreateInfoEXT takes them
   as they are; DRM_FORMAT_ABGR8888 is VK_FORMAT_R8G8B8A8_UNORM), and the
   sync_file to wait on before reading it (VkImportSemaphoreFdInfoKHR with
   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, or poll()), -1 when the frame
   is complete already. The descriptors stay the engine's until the frame is
   released: dup them to import (Vulkan takes ownership of what it imports).
   A buffer keeps its descriptors and its buffer_id until the view is resized,
   so an application may keep its imports by buffer_id. Rows run top to bottom. */
typedef struct zg_dmabuf {
    size_t size;                 /* set by the caller */
    uint32_t fourcc;             /* DRM_FORMAT_* */
    uint64_t modifier;           /* DRM_FORMAT_MOD_* (0: linear) */
    unsigned plane_count;        /* 1 to 4 */
    int fd[4];
    uint32_t offset[4];
    uint32_t stride[4];
    int ready_fence_fd;
    uint64_t buffer_id;
} zg_dmabuf;

/* ZG_ERROR_UNSUPPORTED for frames of other kinds. */
ZG_API zg_result zg_surface_dmabuf(zg_surface *surface, const zg_frame *frame, zg_dmabuf *out);
/* zg_surface_release, with a sync_file (Linux, ZG_SURFACE_VULKAN) signaled when
   the application's reads of the frame are done: the engine waits on it before
   drawing into that buffer again. The engine owns the descriptor from then on
   (closing it for other kinds); -1 is zg_surface_release. */
ZG_API void zg_surface_release_with_fence(zg_surface *surface, const zg_frame *frame, int release_fence_fd);
/* The device frames are given for (ZG_SURFACE_D3D11: ID3D11Device*), NULL otherwise. */
ZG_API void *zg_surface_device(zg_surface *surface);

/* A frame's Win32 handles (0.3; ZG_SURFACE_VULKAN on Windows, ZG_SURFACE_D3D12):
   the texture as an NT handle (Vulkan: VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
   a dedicated allocation; D3D12: OpenSharedHandle; GL:
   EXT_memory_object_win32), its format (DXGI_FORMAT_R8G8B8A8_UNORM, 28: Vulkan's
   VK_FORMAT_R8G8B8A8_UNORM), and two timeline fences as NT handles (Vulkan:
   timeline semaphores with VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT):
   wait for ready_value on the ready fence before reading the texture, and
   signal release_value on the release fence when done with it, before
   zg_surface_release, even if the frame was not used (the engine's GPU waits
   for it before drawing into that texture again). ZG_SURFACE_D3D12 frames have
   no release fence: release signals it on the queue. The handles stay the
   engine's (importing them into Vulkan does not take them); the texture's and
   buffer_id stay the same until the view is resized, so imports can be kept
   by buffer_id; the fences stay the surface's. adapter_luid is the adapter
   the texture is on (HighPart << 32 | LowPart): Vulkan's
   VkPhysicalDeviceIDProperties.deviceLUID must match it. Rows run top to
   bottom. */
typedef struct zg_win32_frame {
    size_t size;                 /* set by the caller */
    zg_native_handle texture_handle;
    unsigned dxgi_format;
    zg_native_handle ready_fence_handle;
    uint64_t ready_value;
    zg_native_handle release_fence_handle; /* 0 for ZG_SURFACE_D3D12 */
    uint64_t release_value;
    uint64_t buffer_id;
    uint64_t adapter_luid;
} zg_win32_frame;

/* ZG_ERROR_UNSUPPORTED for frames of other kinds. */
ZG_API zg_result zg_surface_win32(zg_surface *surface, const zg_frame *frame, zg_win32_frame *out);
/* ZG_SURFACE_D3D9 (0.3): the application's device is about to be reset
   (IDirect3DDevice9::Reset): the engine releases its objects on it, and makes
   them again on the next acquire. The device's thread. Nothing for other kinds. */
ZG_API void zg_surface_device_lost(zg_surface *surface);

/* ------------------------------------------------------------------------ */
/* Views                                                                     */

typedef struct zg_view_config {
    size_t size;
    unsigned width, height;      /* logical pixels */
    float scale;                 /* device scale factor, 0 = 1.0 */
    const zg_surface_config *surface; /* NULL = CPU surface */
    int transparent;             /* nonzero: no default white background */
} zg_view_config;

ZG_API zg_view *zg_view_create(zg_context *ctx, const zg_view_config *config);
ZG_API void zg_view_destroy(zg_view *view);
ZG_API zg_surface *zg_view_surface(zg_view *view);

/* Starts a navigation; load_finished reports its end. The URL must be absolute
   and allowed by the context's trust rules (or allow_external_content). */
ZG_API zg_result zg_view_load_url(zg_view *view, const char *url);
/* Replaces the document with `html` at once, not as a navigation (no
   load_finished): when this returns the page is parsed and its inline scripts
   have run; its subresources load as usual. `base_url` (NULL: about:blank) is the
   document's URL and origin: bindings follow the trust rules for it, as for a
   page loaded from there. */
ZG_API zg_result zg_view_load_html(zg_view *view, const char *html, size_t len, const char *base_url);
ZG_API void zg_view_reload(zg_view *view);
ZG_API void zg_view_stop(zg_view *view);

ZG_API void zg_view_resize(zg_view *view, unsigned width, unsigned height);
ZG_API void zg_view_set_scale(zg_view *view, float scale);
ZG_API void zg_view_set_focus(zg_view *view, int focused);

/* Texture and CPU modes: render one frame of the app's own loop. Runs pending work
   for this view, advances requestAnimationFrame and animations to
   `frame_time_ms` (the app's frame clock, monotonic milliseconds), updates style
   and layout. A CPU view repaints dirty regions into the surface before
   returning. A GPU view (every kind but CPU; since 0.3) composites its layers on the
   engine's compositor thread: the frame reaches zg_surface_acquire shortly after
   this returns (zg_runtime_wakeup_handle is signaled), and while the frame of
   the previous call is still being composited no new rendering update starts.
   CSS animations of transform, opacity and filters are composited there with
   no rendering update at all: one frame per call, at `frame_time_ms`, so they
   move with the app's frames (and pause with its clock); without calls they
   stop. The engine does not pace these views itself: call it once per app frame.
   Window mode: the engine paces rendering from the display's vsync; do not call. */
ZG_API zg_result zg_view_render(zg_view *view, double frame_time_ms);
/* Texture mode (0.3): at most this many composited animation frames per second
   for a GPU view, whatever the app's frame rate (a game running at 144 fps may
   keep its UI at 30, for instance); 0, the default, is one per zg_view_render.
   A CPU view ignores it. ZG_ERROR_INVALID_ARGUMENT for a negative rate, or for
   a window's view, which follows its display. */
ZG_API zg_result zg_view_set_frame_rate(zg_view *view, double frames_per_second);

/* Evaluate a script in the main frame. `result` may be NULL; otherwise the
   caller owns *result and frees it with zg_value_release. */
ZG_API zg_result zg_view_evaluate(zg_view *view, const char *script, size_t len, zg_value **result);

/* ---- Input (texture mode; window mode feeds the same internal path) ---- */

enum {
    ZG_MOD_SHIFT = 1 << 0,
    ZG_MOD_CTRL  = 1 << 1,
    ZG_MOD_ALT   = 1 << 2,
    ZG_MOD_META  = 1 << 3
};

typedef enum zg_mouse_button {
    ZG_MOUSE_NONE, ZG_MOUSE_LEFT, ZG_MOUSE_MIDDLE, ZG_MOUSE_RIGHT
} zg_mouse_button;

typedef enum zg_mouse_event_type {
    ZG_MOUSE_MOVE, ZG_MOUSE_DOWN, ZG_MOUSE_UP
} zg_mouse_event_type;

/* Coordinates in logical view pixels. For ZG_MOUSE_MOVE, `button` is the button
   held down (drags), ZG_MOUSE_NONE otherwise. */
ZG_API void zg_view_mouse_event(zg_view *view, zg_mouse_event_type type,
                                float x, float y, zg_mouse_button button,
                                unsigned click_count, unsigned modifiers);
/* Signs as DOM WheelEvent: positive delta_y scrolls down, positive delta_x right.
   `precise` nonzero: deltas in logical pixels (touchpads); zero: in wheel notches. */
ZG_API void zg_view_scroll_event(zg_view *view, float x, float y,
                                 float delta_x, float delta_y, int precise,
                                 unsigned modifiers);

typedef enum zg_key_event_type {
    ZG_KEY_DOWN, ZG_KEY_UP, ZG_KEY_CHAR
} zg_key_event_type;

/* `key` and `code` follow the DOM KeyboardEvent.key / .code strings;
   `text` is the produced text for ZG_KEY_CHAR, sent after its ZG_KEY_DOWN (as
   WM_KEYDOWN and WM_CHAR); `key` and `code` may be NULL for it. */
ZG_API void zg_view_key_event(zg_view *view, zg_key_event_type type,
                              const char *key, const char *code,
                              const char *text, unsigned modifiers, int is_repeat);

/* IME composition (texture mode). */
ZG_API void zg_view_ime_set_composition(zg_view *view, const char *text, size_t len,
                                        unsigned cursor_begin, unsigned cursor_end);
ZG_API void zg_view_ime_commit(zg_view *view, const char *text, size_t len);
ZG_API void zg_view_ime_cancel(zg_view *view);

/* ---- Drag and drop from outside the page (0.3) ----

   Something the host drags over the view (files from the desktop, text from
   another application), in logical pixels: ENTER, then OVER as it moves, then
   DROP or LEAVE. The page sees dragenter, dragover, drop and dragleave, with
   the files in DataTransfer.files. Window mode does this itself for drops on
   the window. Drags that start in the page need nothing from the host: mouse
   events move them. */
typedef enum zg_drag_event { ZG_DRAG_ENTER, ZG_DRAG_OVER, ZG_DRAG_LEAVE, ZG_DRAG_DROP } zg_drag_event;

typedef struct zg_drag_data {
    size_t size;
    const char *const *files;  /* UTF-8 paths */
    size_t file_count;
    const char *text;          /* UTF-8, NULL for none */
    const char *url;
    const char *html;
} zg_drag_data;

/* Nonzero if the page accepts a drop there (for DROP: it took it). The data is
   read at ENTER (and at DROP if there was no ENTER). */
ZG_API int zg_view_drag(zg_view *view, zg_drag_event event, float x, float y, const zg_drag_data *data);

/* ---- Web Inspector (0.3; dev and debug profiles) ----

   Opens the Web Inspector for the view's page in a window of its own, or brings
   it to the front; in window mode F12 and Ctrl+Shift+I do the same. The
   inspector's windows run in zg_runtime_run (and zg_runtime_update) and do not
   count as the application's: closing them ends nothing. ZG_ERROR_UNSUPPORTED
   in the minimal and standard profiles, ZG_ERROR_GPU if no window can be
   created. */
ZG_API zg_result zg_view_show_inspector(zg_view *view);

/* ---- Callbacks from the engine to the host ---- */

typedef enum zg_cursor {
    ZG_CURSOR_DEFAULT, ZG_CURSOR_POINTER, ZG_CURSOR_TEXT, ZG_CURSOR_WAIT,
    ZG_CURSOR_CROSSHAIR, ZG_CURSOR_MOVE, ZG_CURSOR_NOT_ALLOWED,
    ZG_CURSOR_RESIZE_EW, ZG_CURSOR_RESIZE_NS, ZG_CURSOR_RESIZE_NESW,
    ZG_CURSOR_RESIZE_NWSE, ZG_CURSOR_NONE
} zg_cursor;

typedef struct zg_js_error {
    const char *message; size_t message_len;
    const char *stack;   size_t stack_len;
    const char *file;    size_t file_len;
    unsigned line, column;
    int unhandled_rejection; /* nonzero for a rejected Promise without handler */
} zg_js_error;

typedef enum zg_dialog_kind { ZG_DIALOG_ALERT, ZG_DIALOG_CONFIRM, ZG_DIALOG_PROMPT } zg_dialog_kind;

/* A server asks for HTTP credentials (401 with WWW-Authenticate). Valid until
   answered, which must happen exactly once, from any thread. Credentials that
   are given are kept for the context's lifetime, in memory, for the same
   protection space (host, port, realm); a failed one is asked again. */
typedef struct zg_auth_challenge zg_auth_challenge;

ZG_API const char *zg_auth_challenge_url(const zg_auth_challenge *challenge);
ZG_API const char *zg_auth_challenge_realm(const zg_auth_challenge *challenge);
/* "basic", "digest", "ntlm", "negotiate" or "other". */
ZG_API const char *zg_auth_challenge_scheme(const zg_auth_challenge *challenge);
/* How many credentials for this request were refused so far. */
ZG_API unsigned zg_auth_challenge_failures(const zg_auth_challenge *challenge);
/* Retry with these credentials (UTF-8, copied). */
ZG_API void zg_auth_challenge_respond(zg_auth_challenge *challenge, const char *user, const char *password);
/* No credentials: the server's 401 response is shown. */
ZG_API void zg_auth_challenge_cancel(zg_auth_challenge *challenge);

/* A download: a navigation to an attachment or to content the view cannot show
   (the transfer continues into the file), or a <a download> link (blob: and
   data: URLs included). Refused unless download_started gives it a destination.
   Valid from download_started until download_finished returns (until
   download_started returns, if refused). The partial file of a download that
   fails or is cancelled is removed. Downloads end with their view. */
typedef struct zg_download zg_download;

ZG_API const char *zg_download_url(const zg_download *download);
/* From the download attribute, Content-Disposition, or the URL. */
ZG_API const char *zg_download_suggested_filename(const zg_download *download);
ZG_API const char *zg_download_mime_type(const zg_download *download);
ZG_API int64_t zg_download_expected_length(const zg_download *download); /* -1: unknown */
ZG_API uint64_t zg_download_received_length(const zg_download *download);
/* During download_started: write the file at `path` (UTF-8; an existing file is
   replaced). */
ZG_API zg_result zg_download_set_destination(zg_download *download, const char *path);
/* Runtime thread. download_finished reports it as failed. */
ZG_API void zg_download_cancel(zg_download *download);

/* <input type=file> wants files (0.3). Valid until answered, which must happen
   exactly once, from any thread. With no file_chooser callback, window mode
   shows the system's file dialog; other views cancel. */
typedef struct zg_file_chooser zg_file_chooser;

ZG_API int zg_file_chooser_allows_multiple(const zg_file_chooser *chooser);
ZG_API int zg_file_chooser_allows_directories(const zg_file_chooser *chooser);
/* The input's accept list: MIME types ("image/png", or a wildcard such as all
   images) and extensions (".txt"). */
ZG_API size_t zg_file_chooser_accept_count(const zg_file_chooser *chooser);
ZG_API const char *zg_file_chooser_accept(const zg_file_chooser *chooser, size_t index);
/* UTF-8 paths, copied. No paths is the same as cancelling. */
ZG_API void zg_file_chooser_choose(zg_file_chooser *chooser, size_t count, const char *const *paths);
ZG_API void zg_file_chooser_cancel(zg_file_chooser *chooser);

/* A context menu (0.3): a right click (on the press; on Windows the release),
   the menu key or Shift+F10, unless the page cancelled its contextmenu event
   (pages build their own menus that way, through bindings). The engine draws
   nothing: the application shows the items as it likes (a native popup menu at
   the position) and answers with the chosen one, or cancels. Valid until
   answered, which must happen exactly once, from any thread. With no
   context_menu callback no menu appears. */
typedef struct zg_context_menu zg_context_menu;

typedef enum zg_context_menu_item_kind {
    ZG_CONTEXT_MENU_ITEM_ACTION,
    ZG_CONTEXT_MENU_ITEM_CHECKBOX,
    ZG_CONTEXT_MENU_ITEM_SEPARATOR,
    ZG_CONTEXT_MENU_ITEM_SUBMENU
} zg_context_menu_item_kind;

/* The items are a flat list in order; a submenu's items follow it and name it
   as their parent. Owned by the menu. */
typedef struct zg_context_menu_item {
    unsigned id;          /* 1 and up, for zg_context_menu_select */
    unsigned parent;      /* 0: the top level; else the submenu's id */
    zg_context_menu_item_kind kind;
    const char *label;    /* UTF-8; empty for separators */
    int enabled;
    int checked;          /* checkboxes */
} zg_context_menu_item;

/* Where, in the view's logical pixels. */
ZG_API void zg_context_menu_position(const zg_context_menu *menu, float *x, float *y);
/* What is under it; NULL when there is none. */
ZG_API const char *zg_context_menu_link_url(const zg_context_menu *menu);
ZG_API const char *zg_context_menu_link_text(const zg_context_menu *menu);
ZG_API const char *zg_context_menu_image_url(const zg_context_menu *menu);
ZG_API const char *zg_context_menu_media_url(const zg_context_menu *menu);
ZG_API const char *zg_context_menu_selected_text(const zg_context_menu *menu);
ZG_API const char *zg_context_menu_frame_url(const zg_context_menu *menu);
ZG_API int zg_context_menu_is_editable(const zg_context_menu *menu);
ZG_API size_t zg_context_menu_item_count(const zg_context_menu *menu);
ZG_API const zg_context_menu_item *zg_context_menu_item_at(const zg_context_menu *menu, size_t index);
/* Runs the item (copy, paste, open the link, download the image...). */
ZG_API void zg_context_menu_select(zg_context_menu *menu, unsigned id);
ZG_API void zg_context_menu_cancel(zg_context_menu *menu);

typedef struct zg_view_callbacks {
    size_t size;
    void *user;
    void (*console)(zg_view *view, const zg_log_message *msg, void *user);
    void (*js_error)(zg_view *view, const zg_js_error *err, void *user);
    void (*cursor_changed)(zg_view *view, zg_cursor cursor, void *user);
    void (*tooltip_changed)(zg_view *view, const char *text, size_t len, void *user);
    void (*title_changed)(zg_view *view, const char *title, size_t len, void *user);
    /* When `failed`, zg_last_error() gives the reason during the call. */
    void (*load_finished)(zg_view *view, const char *url, int failed, void *user);
    /* Navigation outside the allowed origins. Return nonzero to allow it in
       the view; by default it is blocked and passed to open_external. */
    int  (*navigation_blocked)(zg_view *view, const char *url, void *user);
    void (*open_external)(zg_view *view, const char *url, void *user);
    void (*ime_caret_rect)(zg_view *view, float x, float y, float w, float h, void *user);
    /* JS dialogs. For CONFIRM/PROMPT return nonzero for "OK"; for PROMPT write
       a malloc'd UTF-8 reply to *reply (the engine frees it). */
    int  (*dialog)(zg_view *view, zg_dialog_kind kind, const char *message,
                   const char *default_text, char **reply, void *user);
    /* HTTP authentication; NULL: every challenge is cancelled. */
    void (*auth_challenge)(zg_view *view, zg_auth_challenge *challenge, void *user);
    /* Downloads; NULL download_started: every download is refused. When
       `failed`, zg_last_error() gives the reason during the call. */
    void (*download_started)(zg_view *view, zg_download *download, void *user);
    void (*download_progress)(zg_view *view, zg_download *download, void *user);
    void (*download_finished)(zg_view *view, zg_download *download, int failed, void *user);
    /* <input type=file> (0.3); see zg_file_chooser. */
    void (*file_chooser)(zg_view *view, zg_file_chooser *chooser, void *user);
    /* Context menus (0.3); see zg_context_menu. */
    void (*context_menu)(zg_view *view, zg_context_menu *menu, void *user);
} zg_view_callbacks;

ZG_API void zg_view_set_callbacks(zg_view *view, const zg_view_callbacks *callbacks);

/* ------------------------------------------------------------------------ */
/* Direct JavaScript bindings (JSC C API underneath, no IPC)                 */

/* Return a new value created with zg_value_* on `call`, or NULL for undefined.
   To throw, return zg_call_throw(call, message). */
typedef zg_value *(*zg_bind_fn)(zg_call *call, void *user);

/* Bind a native function at a dotted path on `window` (e.g. "App.greet").
   Installed each time a frame's window object is created, for trusted origins
   only (see zg_context_config) and in the main frame only; the origin is checked
   again on every call. */
ZG_API zg_result zg_view_bind(zg_view *view, const char *path, zg_bind_fn fn, void *user);

/* A script run in every page that gets the bindings, under the same rules, after
   the bindings and classes are installed and before the page's own scripts (a
   framework's JavaScript runtime, for instance). Scripts run in the order they
   were added; the current document runs it too, at once. Its errors reach
   js_error. The source is copied. Since 0.2. */
ZG_API zg_result zg_view_add_script(zg_view *view, const char *source, size_t len);

/* Dispatch `new CustomEvent(name, { detail: JSON.parse(json_detail) })` on window. */
ZG_API zg_result zg_view_emit(zg_view *view, const char *name,
                              const char *json_detail, size_t json_len);

/* ---- Call arguments (valid only during the callback) ---- */

typedef enum zg_value_type {
    ZG_TYPE_UNDEFINED, ZG_TYPE_NULL, ZG_TYPE_BOOLEAN, ZG_TYPE_NUMBER,
    ZG_TYPE_BIGINT, ZG_TYPE_STRING, ZG_TYPE_ARRAY, ZG_TYPE_OBJECT,
    ZG_TYPE_FUNCTION,
    ZG_TYPE_BYTES        /* ArrayBuffer, typed array or DataView (0.2; OBJECT before) */
} zg_value_type;

/* Each reader converts as JavaScript does (Boolean(), Number(), String(),
   JSON.stringify()). A missing argument reads as undefined: false, NaN, 0, and
   NULL for the strings. */
ZG_API size_t        zg_call_argc(const zg_call *call);
ZG_API zg_value_type zg_call_arg_type(const zg_call *call, size_t index);
ZG_API int           zg_call_arg_bool(zg_call *call, size_t index);
ZG_API double        zg_call_arg_number(zg_call *call, size_t index);
/* A BigInt's low 64 bits; a Number truncated toward zero and saturated to the
   int64 range, NaN as 0. */
ZG_API int64_t       zg_call_arg_int64(zg_call *call, size_t index);
/* UTF-8, NUL-terminated; `len` counts embedded NULs. Lives until the callback
   returns. */
ZG_API const char   *zg_call_arg_string(zg_call *call, size_t index, size_t *len);
/* JSON (arrays, objects); "null" for what JSON cannot hold (undefined,
   functions, BigInts, NaN). Lives until the callback returns. */
ZG_API const char   *zg_call_arg_json(zg_call *call, size_t index, size_t *len);
/* The bytes of an ArrayBuffer, typed array or DataView, in place (no copy), and
   writable: a binding can fill a buffer the page passed. NULL for other values
   and detached buffers. Valid until the callback returns or calls into
   JavaScript (zg_view_evaluate, zg_ref_call), which could detach it. Since 0.2. */
ZG_API void         *zg_call_arg_bytes(zg_call *call, size_t index, size_t *len);
ZG_API zg_view      *zg_call_view(const zg_call *call);
ZG_API const char   *zg_call_origin(const zg_call *call);

/* ---- Return values ----

   Made on `call`, a value belongs to the call and the engine frees it. Made with
   `call` NULL (0.2), the caller owns it and frees it with zg_value_release: for
   zg_ref_call arguments. zg_call_throw, zg_call_promise and zg_value_new_instance
   need a call. */

ZG_API zg_value *zg_value_null(zg_call *call);
ZG_API zg_value *zg_value_bool(zg_call *call, int v);
ZG_API zg_value *zg_value_number(zg_call *call, double v);
ZG_API zg_value *zg_value_int64(zg_call *call, int64_t v); /* BigInt beyond 2^53 */
ZG_API zg_value *zg_value_string(zg_call *call, const char *s);
ZG_API zg_value *zg_value_string_len(zg_call *call, const char *s, size_t len);
ZG_API zg_value *zg_value_json(zg_call *call, const char *json, size_t len);
/* A Uint8Array over a new ArrayBuffer with a copy of the bytes. Since 0.2. */
ZG_API zg_value *zg_value_bytes(zg_call *call, const void *data, size_t len);
/* The same, without a copy: JavaScript uses `data` in place and `release(user)`
   is called, on any thread, once the buffer is collected (or the value released
   unused). The value can reach JavaScript once. Since 0.2. */
ZG_API zg_value *zg_value_bytes_external(zg_call *call, void *data, size_t len,
                                         zg_release_fn release, void *user);
ZG_API zg_value *zg_call_throw(zg_call *call, const char *message);

/* ---- Asynchronous calls ---- */

/* Return a pending Promise to JavaScript. Resolve it later from any thread;
   the engine settles it on the runtime thread. Each promise must be settled
   exactly once: until then it keeps its page's script objects alive, even after
   the view is gone. */
ZG_API zg_value *zg_call_promise(zg_call *call, zg_promise **out);
ZG_API void zg_promise_resolve(zg_promise *p, const char *json, size_t len); /* JSON value */
/* Resolves with a Uint8Array of a copy of the bytes (taken at the call). 0.2. */
ZG_API void zg_promise_resolve_bytes(zg_promise *p, const void *data, size_t len);
ZG_API void zg_promise_reject(zg_promise *p, const char *message);

/* ---- JavaScript values kept by the host (0.2) ----

   A zg_ref keeps a value of a page alive past the call that received it: a
   callback to call later, an object to hand back. It keeps the page's script
   objects alive until released, like a pending promise. */
typedef struct zg_ref zg_ref;

/* Argument `index` as a new reference, owned by the caller; NULL if missing. */
ZG_API zg_ref   *zg_call_arg_ref(zg_call *call, size_t index);
/* Return the referenced value (the same object, not a copy). Only into the page
   it came from: elsewhere the call throws. */
ZG_API zg_value *zg_value_ref(zg_call *call, const zg_ref *ref);
ZG_API zg_value_type zg_ref_type(const zg_ref *ref);
/* Calls a referenced function with `this` undefined. `argv` are values made with
   a NULL call (or on a call in progress); they are not consumed. `result` as for
   zg_view_evaluate (may be NULL). ZG_ERROR_JS_EXCEPTION if it throws,
   ZG_ERROR_NOT_FOUND once its page is gone. Runtime thread; may run from inside
   a binding. */
ZG_API zg_result zg_ref_call(zg_ref *fn, size_t argc, zg_value *const *argv, zg_value **result);
/* Any thread. */
ZG_API void      zg_ref_release(zg_ref *ref);

/* ---- A page's objects from native code (0.4) ----

   The page's objects, read and changed as JavaScript would, through references:
   start from the global object (window) and go to any value from there, e.g.
   document.getElementById("title").textContent = "Hello" is
       zg_ref *window = zg_view_global(view), *title;
       zg_value *id = zg_value_string(NULL, "title");
       zg_ref_call_method(window, "document.getElementById", 1, &id, &title);
       zg_value *text = zg_value_string(NULL, "Hello");
       zg_ref_set(title, "textContent", text);
   (then zg_value_release and zg_ref_release what was made).
   `name` is a property name or a dotted path of them ("document.title",
   "style.color", "items.0"); every step before the last must be an object.
   Typed reads are strict: a value of another type fails with ZG_ERROR_TYPE
   (zg_ref_get_json converts any). Getters, setters and methods that throw
   give ZG_ERROR_JS_EXCEPTION with the message, a missing step on a path
   ZG_ERROR_NOT_FOUND, and a reference whose page is gone ZG_ERROR_NOT_FOUND.
   Strings returned are UTF-8 copies the caller frees with zg_free. Runtime
   thread. */

/* The main frame's global object (window), or NULL when the view has no
   document. Owned by the caller (zg_ref_release); a new document has a new one. */
ZG_API zg_ref   *zg_view_global(zg_view *view);
/* The value at `name` as a new reference (undefined if the last step is
   missing). */
ZG_API zg_result zg_ref_get(zg_ref *object, const char *name, zg_ref **out);
ZG_API zg_result zg_ref_get_bool(zg_ref *object, const char *name, int *out);
ZG_API zg_result zg_ref_get_number(zg_ref *object, const char *name, double *out);
ZG_API zg_result zg_ref_get_string(zg_ref *object, const char *name, char **out, size_t *len);
/* JSON.stringify of the value ("null" for what JSON cannot hold). */
ZG_API zg_result zg_ref_get_json(zg_ref *object, const char *name, char **out, size_t *len);
/* Sets `name` to `value`, made with a NULL call (or zg_value_ref for another
   reference); NULL sets undefined. The value is not consumed. */
ZG_API zg_result zg_ref_set(zg_ref *object, const char *name, const zg_value *value);
/* Calls the method at `name` with `this` its object (for "a.b.c", a.b); the
   result as a new reference, `result` may be NULL. Arguments as for
   zg_ref_call. */
ZG_API zg_result zg_ref_call_method(zg_ref *object, const char *name, size_t argc,
                                    zg_value *const *argv, zg_ref **result);
/* The referenced value itself, typed as the getters above. */
ZG_API zg_result zg_ref_to_bool(const zg_ref *ref, int *out);
ZG_API zg_result zg_ref_to_number(const zg_ref *ref, double *out);
ZG_API zg_result zg_ref_to_string(const zg_ref *ref, char **out, size_t *len);
ZG_API zg_result zg_ref_to_json(const zg_ref *ref, char **out, size_t *len);
/* Frees a string the API returned (zg_ref_get_string and the like). */
ZG_API void      zg_free(void *pointer);

/* ---- Classes (0.2) ----

   A JavaScript class whose instances carry a native pointer. Installed with the
   bindings (same rules, at a dotted path such as "App.File"), with its methods
   and accessors on the prototype. `instanceof`, subclassing with `extends` and
   Symbol.toStringTag (the last path segment) work as for built-in classes. */
typedef struct zg_class zg_class;

/* Called once for each native pointer set on an instance, on the runtime thread
   after the instance was collected (not during the collection: it may call the
   API), or when the view is destroyed. Not at process exit. */
typedef void (*zg_finalize_fn)(void *native, void *user);

/* `constructor` runs for `new`, with the new instance as `this`: it sets its
   pointer with zg_call_set_native, or throws. NULL: `new` throws a TypeError and
   instances come only from zg_value_new_instance. Calling the class without
   `new` throws. Owned by the view; add methods and properties before the page
   that uses them loads. */
ZG_API zg_class *zg_view_define_class(zg_view *view, const char *path, zg_bind_fn constructor,
                                      zg_finalize_fn finalize, void *user);
/* Methods and accessors get the instance as `this`: zg_call_native gives its
   pointer; called on another object they throw a TypeError. The setter gets the
   new value as argument 0; a NULL setter makes the property read-only. */
ZG_API zg_result zg_class_add_method(zg_class *cls, const char *name, zg_bind_fn fn, void *user);
ZG_API zg_result zg_class_add_property(zg_class *cls, const char *name,
                                       zg_bind_fn getter, zg_bind_fn setter, void *user);
/* In a constructor: the instance's native pointer (once). */
ZG_API zg_result zg_call_set_native(zg_call *call, void *native);
/* In a constructor, method or accessor: the instance's native pointer. */
ZG_API void     *zg_call_native(zg_call *call);
/* A new instance of `cls` in the call's page, without running its constructor:
   for bindings that return objects. */
ZG_API zg_value *zg_value_new_instance(zg_call *call, zg_class *cls, void *native);

/* ---- Values returned by zg_view_evaluate (caller-owned) ---- */

ZG_API zg_value_type zg_value_type_of(const zg_value *v);
/* JSON of the value; "null" for undefined and functions. Lives as long as `v`. */
ZG_API const char   *zg_value_to_json(zg_value *v, size_t *len);
ZG_API void          zg_value_release(zg_value *v);

/* ------------------------------------------------------------------------ */
/* Windows (optional native window hosting one view)                        */

typedef struct zg_window_config {
    size_t size;
    const char *title;
    unsigned width, height;  /* logical pixels */
    int frameless;
    int fullscreen;
    int fixed_size;          /* nonzero: not resizable by the user */
    /* (0.3) Nonzero: what the page leaves transparent (no html/body background,
       or a translucent one) shows the desktop behind the window, alpha
       blended by the window system. With frameless, a window of any shape. */
    int transparent;
    /* (0.3) With transparent, nonzero: clicks where the page is fully
       transparent go to what is behind the window. Costs a copy of every
       frame to the CPU (on Windows the window is then a layered window
       updated from it). Windows and X11; Wayland ignores it. */
    int click_through;
    /* (0.5) Nonzero: the window starts hidden; zg_window_show shows it.
       Otherwise it appears with its first frame. */
    int hidden;
} zg_window_config;

/* Frameless windows move by the page's regions marked with the CSS custom
   property --zg-app-region: drag (inherited: inside such a region, mark
   controls --zg-app-region: no-drag), with the system's snapping and double
   click to maximize, and resize by their edges when resizable. */

ZG_API zg_window *zg_window_create(zg_context *ctx, const zg_window_config *config);
ZG_API void zg_window_destroy(zg_window *win);
ZG_API zg_view *zg_window_view(zg_window *win);
ZG_API void zg_window_set_title(zg_window *win, const char *title);
ZG_API void zg_window_set_fullscreen(zg_window *win, int fullscreen);
ZG_API void *zg_window_native_handle(zg_window *win); /* HWND, NSWindow*, X11/Wayland surface */

/* ---- Window control (0.5) ----

   Sizes are logical pixels, the page's CSS pixels (as zg_window_config's).
   Positions are desktop coordinates, those of zg_screen's bounds: the window
   system's own units (physical pixels on Windows and X11). Wayland lets no
   application place its windows: setting or reading a position fails there
   with ZG_ERROR_UNSUPPORTED. */

typedef enum zg_window_state {
    ZG_WINDOW_NORMAL = 0,
    ZG_WINDOW_MINIMIZED,
    ZG_WINDOW_MAXIMIZED,
    ZG_WINDOW_FULLSCREEN,
    ZG_WINDOW_HIDDEN
} zg_window_state;

ZG_API zg_result zg_window_set_size(zg_window *win, unsigned width, unsigned height);
ZG_API zg_result zg_window_get_size(zg_window *win, unsigned *width, unsigned *height);
/* The smallest and largest size the user can give the window; 0 for no limit. */
ZG_API zg_result zg_window_set_min_size(zg_window *win, unsigned width, unsigned height);
ZG_API zg_result zg_window_set_max_size(zg_window *win, unsigned width, unsigned height);
ZG_API zg_result zg_window_set_resizable(zg_window *win, int resizable);
/* The position of the window's top-left corner (its frame's, if it has one). */
ZG_API zg_result zg_window_set_position(zg_window *win, int x, int y);
ZG_API zg_result zg_window_get_position(zg_window *win, int *x, int *y);
/* Centered on the screen it is on (zg_window_screen). */
ZG_API zg_result zg_window_center(zg_window *win);
ZG_API zg_result zg_window_minimize(zg_window *win);
ZG_API zg_result zg_window_maximize(zg_window *win);
/* Back to normal from minimized or maximized. */
ZG_API zg_result zg_window_restore(zg_window *win);
ZG_API zg_result zg_window_show(zg_window *win);
ZG_API zg_result zg_window_hide(zg_window *win);
/* Above the other windows with the keyboard focus, where the window system
   allows it (it may only flash the window in the taskbar). */
ZG_API zg_result zg_window_focus(zg_window *win);
ZG_API zg_result zg_window_set_always_on_top(zg_window *win, int on_top);
ZG_API zg_window_state zg_window_get_state(zg_window *win);

/* ---- Window events (0.5) ----

   Delivered on the runtime thread while it runs the loop. Without callbacks
   a window closes when the user asks it to, and the runtime's loop ends once
   every window is closed. A closed window is hidden; it stays valid until
   zg_window_destroy, which callbacks may call (for any window). */
typedef struct zg_window_callbacks {
    size_t size;
    void *user;
    /* The user asked to close the window (close button, Alt+F4, the window
       menu). Return nonzero to close it, 0 to keep it open. NULL: it closes. */
    int  (*close_requested)(zg_window *win, void *user);
    /* The new size, logical pixels. */
    void (*resized)(zg_window *win, unsigned width, unsigned height, void *user);
    /* The new position, desktop coordinates (not on Wayland). */
    void (*moved)(zg_window *win, int x, int y, void *user);
    void (*focus_changed)(zg_window *win, int focused, void *user);
    void (*state_changed)(zg_window *win, zg_window_state state, void *user);
    /* An item of the window's menu bar or of a popup menu was chosen, by
       click or by its accelerator. */
    void (*menu_item)(zg_window *win, unsigned id, void *user);
    /* The window moved to another screen (zg_window_screen gives which), or
       its screen's scale changed. */
    void (*screen_changed)(zg_window *win, void *user);
} zg_window_callbacks;

ZG_API zg_result zg_window_set_callbacks(zg_window *win, const zg_window_callbacks *callbacks);

/* ---- Screens (0.5) ----

   The connected screens, in the window system's order. They are known once a
   window exists; before, zg_screen_count opens the window system's display. */
typedef struct zg_screen {
    size_t size;               /* set by the caller: sizeof(zg_screen) */
    int x, y;                  /* bounds, desktop coordinates */
    unsigned width, height;
    int work_x, work_y;        /* the bounds without taskbars, docks and panels */
    unsigned work_width, work_height;
    float scale;               /* device pixels per logical pixel (1.5 at 150%) */
    float refresh_rate;        /* Hz; 0 if unknown */
    int primary;
    char name[128];            /* UTF-8, possibly truncated; may be empty */
} zg_screen;

ZG_API size_t    zg_screen_count(zg_runtime *rt);
ZG_API zg_result zg_screen_get(zg_runtime *rt, size_t index, zg_screen *out);
/* The index of the screen the window is mostly on. */
ZG_API zg_result zg_window_screen(zg_window *win, size_t *index);

/* ---- Dialogs (0.5) ----

   The system's own dialogs, modal to their window: the Windows ones, and on
   Linux the desktop's through xdg-desktop-portal (zenity if no portal runs).
   The answer comes to a callback on the runtime thread, never during the
   call that shows the dialog, and always exactly once: if the window was
   destroyed first, with `win` NULL and ZG_ERROR_NOT_FOUND. */

typedef enum zg_file_dialog_kind {
    ZG_FILE_DIALOG_OPEN = 0,   /* existing files */
    ZG_FILE_DIALOG_OPEN_FOLDER,
    ZG_FILE_DIALOG_SAVE
} zg_file_dialog_kind;

typedef struct zg_file_filter {
    const char *name;          /* "Images" */
    const char *pattern;       /* extensions separated by ';' ("png;jpg"), or "*" */
} zg_file_filter;

typedef struct zg_file_dialog {
    size_t size;
    zg_file_dialog_kind kind;
    const char *title;         /* NULL: the system's */
    const char *accept_label;  /* the accept button's label; NULL: the system's */
    const char *location;      /* a folder, or (SAVE) a suggested file path */
    const zg_file_filter *filters; /* not for OPEN_FOLDER */
    size_t filter_count;
    int multiple;              /* OPEN and OPEN_FOLDER: several may be chosen */
} zg_file_dialog;

/* `result` ZG_OK with `count` 0: the user cancelled. Another result: the
   dialog could not be shown, zg_last_error() says why during the call. The
   paths live until the callback returns. */
typedef void (*zg_file_dialog_fn)(zg_window *win, zg_result result, const char *const *paths,
                                  size_t count, void *user);
/* The strings in `dialog` are copied. */
ZG_API zg_result zg_window_file_dialog(zg_window *win, const zg_file_dialog *dialog,
                                       zg_file_dialog_fn done, void *user);

typedef enum zg_message_kind {
    ZG_MESSAGE_INFO = 0,
    ZG_MESSAGE_WARNING,
    ZG_MESSAGE_ERROR
} zg_message_kind;

typedef enum zg_message_button_flags {
    ZG_MESSAGE_BUTTON_DEFAULT = 1, /* chosen by Enter */
    ZG_MESSAGE_BUTTON_CANCEL  = 2  /* chosen by Escape and by closing the box */
} zg_message_button_flags;

typedef struct zg_message_button {
    int id;
    const char *label;
    unsigned flags;            /* zg_message_button_flags */
} zg_message_button;

typedef struct zg_message_box {
    size_t size;
    zg_message_kind kind;
    const char *title;
    const char *message;
    const zg_message_button *buttons; /* in display order; none: one "OK", id 0 */
    size_t button_count;
} zg_message_box;

/* `button_id` is the chosen button's id (the CANCEL one if the box was closed,
   -1 if it has none). The window's page does not run while the box is open. */
typedef void (*zg_message_box_fn)(zg_window *win, zg_result result, int button_id, void *user);
ZG_API zg_result zg_window_message_box(zg_window *win, const zg_message_box *box,
                                       zg_message_box_fn done, void *user);

/* ---- Native menus (0.5) ----

   A zg_menu describes a menu: the window builds its native menu from it when
   it is set, so the description may be changed or destroyed afterwards
   (change the window's items with zg_window_menu_set_*). Chosen items come to
   the window's menu_item callback with their id; a checkbox item's mark
   changes only with zg_window_menu_set_checked.

   Windows only: elsewhere zg_window_set_menu and zg_window_popup_menu fail
   with ZG_ERROR_UNSUPPORTED (draw menus in the page there). */
typedef struct zg_menu zg_menu;

typedef enum zg_menu_item_flags {
    ZG_MENU_ITEM_CHECKBOX = 1,
    ZG_MENU_ITEM_CHECKED  = 2,
    ZG_MENU_ITEM_DISABLED = 4
} zg_menu_item_flags;

ZG_API zg_menu  *zg_menu_create(void);
ZG_API void      zg_menu_destroy(zg_menu *menu);
/* `id` from 1 to 65535, unique in the window's menus. `label` may mark its access key
   with '&' ("&Open"). `accelerator` (may be NULL) is a key with modifiers,
   "Ctrl+S", "Ctrl+Shift+N", "Alt+F4", "F5": shown in the item, and pressing
   it while the window has the focus chooses the item, ahead of the page. */
ZG_API zg_result zg_menu_add_item(zg_menu *menu, unsigned id, const char *label,
                                  const char *accelerator, unsigned flags);
ZG_API zg_result zg_menu_add_separator(zg_menu *menu);
/* `submenu`'s items are copied: it can be destroyed after the call. */
ZG_API zg_result zg_menu_add_submenu(zg_menu *menu, const char *label, const zg_menu *submenu);

/* The window's menu bar, NULL to remove it. Its submenus are the top-level
   entries. */
ZG_API zg_result zg_window_set_menu(zg_window *win, const zg_menu *menu);
ZG_API zg_result zg_window_menu_set_checked(zg_window *win, unsigned id, int checked);
ZG_API zg_result zg_window_menu_set_enabled(zg_window *win, unsigned id, int enabled);
/* A popup menu at (x, y), the view's logical pixels (a zg_context_menu's
   position). The chosen item, if any, comes to menu_item after the call. */
ZG_API zg_result zg_window_popup_menu(zg_window *win, const zg_menu *menu, float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* ZEROG_H */
