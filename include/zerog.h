/*
 * ZeroG — public C API.
 *
 * DRAFT: this header tracks the design in ZeroG.md ("API pubblica C") and is
 * not stable until roadmap phase 2 closes. Names and fields may still change.
 *
 * Rules that hold for every function below:
 *   - C99, opaque handles, no WebKit type ever appears here.
 *   - Single thread: every call happens on the thread that created the
 *     runtime, and every callback is delivered on that thread. The only
 *     exceptions are marked "any thread" (zg_surface_acquire/release,
 *     zg_promise_resolve/reject).
 *   - Config structs start with `size`; set it to sizeof(the struct) so new
 *     fields can be appended without breaking existing clients. Zeroed fields
 *     mean "default".
 *   - Functions that can fail return zg_result; zg_last_error() gives a
 *     readable message for the last failure on the calling thread.
 *   - Strings are UTF-8. Where a length parameter exists, the string need not
 *     be NUL-terminated; otherwise it must be.
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
#define ZG_VERSION_MINOR 0
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
    ZG_ERROR_UNSUPPORTED,       /* feature compiled out of this build profile */
    ZG_ERROR_OUT_OF_MEMORY,
    ZG_ERROR_GPU,
    ZG_ERROR_JS_EXCEPTION,
    ZG_ERROR_INTERNAL
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
    ZG_LOG_SOURCE_ENGINE,   /* internal engine logs */
    ZG_LOG_SOURCE_CONSOLE   /* console.log/warn/error from page JavaScript */
} zg_log_source;

typedef struct zg_log_message {
    zg_log_source source;
    zg_log_level level;
    const char *message; size_t message_len;
    const char *file;    size_t file_len;   /* source URL for console messages */
    unsigned line, column;
    zg_view *view;                          /* NULL for engine logs */
} zg_log_message;

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

    /* 0 = engine default. Lets hosts with their own pools avoid oversubscription. */
    unsigned gc_threads;
    unsigned jit_threads;
    unsigned decode_threads;

    /* Soft limits in bytes, 0 = default. Apply to the whole runtime: all views
       share one JavaScript VM. */
    size_t js_heap_limit;
    size_t cache_limit;

    zg_log_fn log;
    void *log_user;
    zg_log_level log_level;
} zg_runtime_config;

/* Creates the process-wide runtime on the calling thread, which becomes the
   runtime thread. Can be called once per process: a second call, even after
   zg_runtime_destroy, fails with ZG_ERROR_ALREADY_EXISTS. */
ZG_API zg_runtime *zg_runtime_create(const zg_runtime_config *config);

/* Valid only at application shutdown; the runtime cannot be recreated. */
ZG_API void zg_runtime_destroy(zg_runtime *rt);

/* Window mode: the engine owns the loop until zg_runtime_quit. */
ZG_API zg_result zg_runtime_run(zg_runtime *rt);
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
    size_t js_heap_bytes;
    size_t js_heap_capacity;
    size_t resource_cache_bytes;
    size_t surface_bytes;   /* graphics surfaces owned by the engine */
    size_t asset_bytes;     /* files the engine holds: copied, or inflated from archives */
} zg_memory_stats;

ZG_API void zg_runtime_memory_stats(zg_runtime *rt, zg_memory_stats *out);
ZG_API void zg_runtime_collect_garbage(zg_runtime *rt);

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
    const char *data_dir;   /* NULL = OS default from app_id */
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
    ZG_SURFACE_VULKAN,    /* Linux */
    ZG_SURFACE_METAL      /* macOS */
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
         Release frames with the context that used them current. */
    void *device;
    unsigned buffer_count; /* 2 or 3, 0 = 2 */
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
/* The device frames are given for (ZG_SURFACE_D3D11: ID3D11Device*), NULL otherwise. */
ZG_API void *zg_surface_device(zg_surface *surface);

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
ZG_API zg_result zg_view_load_html(zg_view *view, const char *html, size_t len, const char *base_url);
ZG_API void zg_view_reload(zg_view *view);
ZG_API void zg_view_stop(zg_view *view);

ZG_API void zg_view_resize(zg_view *view, unsigned width, unsigned height);
ZG_API void zg_view_set_scale(zg_view *view, float scale);
ZG_API void zg_view_set_focus(zg_view *view, int focused);

/* Texture and CPU modes: render one frame of the app's own loop. Runs pending work
   for this view, advances requestAnimationFrame and animations to
   `frame_time_ms` (the app's frame clock, monotonic milliseconds), updates style
   and layout and repaints dirty regions into the surface. The engine does not
   pace these views itself: call it once per app frame.
   Window mode: the engine paces rendering from the display's vsync; do not call. */
ZG_API zg_result zg_view_render(zg_view *view, double frame_time_ms);

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

/* Dispatch `new CustomEvent(name, { detail: JSON.parse(json_detail) })` on window. */
ZG_API zg_result zg_view_emit(zg_view *view, const char *name,
                              const char *json_detail, size_t json_len);

/* ---- Call arguments (valid only during the callback) ---- */

typedef enum zg_value_type {
    ZG_TYPE_UNDEFINED, ZG_TYPE_NULL, ZG_TYPE_BOOLEAN, ZG_TYPE_NUMBER,
    ZG_TYPE_BIGINT, ZG_TYPE_STRING, ZG_TYPE_ARRAY, ZG_TYPE_OBJECT,
    ZG_TYPE_FUNCTION
} zg_value_type;

ZG_API size_t        zg_call_argc(const zg_call *call);
ZG_API zg_value_type zg_call_arg_type(const zg_call *call, size_t index);
ZG_API int           zg_call_arg_bool(zg_call *call, size_t index);
ZG_API double        zg_call_arg_number(zg_call *call, size_t index);
ZG_API int64_t       zg_call_arg_int64(zg_call *call, size_t index);
/* The returned string lives until the callback returns. */
ZG_API const char   *zg_call_arg_string(zg_call *call, size_t index, size_t *len);
/* Structured arguments as JSON (arrays, objects); lives until the callback returns. */
ZG_API const char   *zg_call_arg_json(zg_call *call, size_t index, size_t *len);
ZG_API zg_view      *zg_call_view(const zg_call *call);
ZG_API const char   *zg_call_origin(const zg_call *call);

/* ---- Return values (owned by the call; the engine frees them) ---- */

ZG_API zg_value *zg_value_null(zg_call *call);
ZG_API zg_value *zg_value_bool(zg_call *call, int v);
ZG_API zg_value *zg_value_number(zg_call *call, double v);
ZG_API zg_value *zg_value_int64(zg_call *call, int64_t v); /* BigInt beyond 2^53 */
ZG_API zg_value *zg_value_string(zg_call *call, const char *s);
ZG_API zg_value *zg_value_string_len(zg_call *call, const char *s, size_t len);
ZG_API zg_value *zg_value_json(zg_call *call, const char *json, size_t len);
ZG_API zg_value *zg_call_throw(zg_call *call, const char *message);

/* ---- Asynchronous calls ---- */

/* Return a pending Promise to JavaScript. Resolve it later from any thread;
   the engine settles it on the runtime thread. Each promise is settled once. */
ZG_API zg_value *zg_call_promise(zg_call *call, zg_promise **out);
ZG_API void zg_promise_resolve(zg_promise *p, const char *json, size_t len); /* JSON value */
ZG_API void zg_promise_reject(zg_promise *p, const char *message);

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
} zg_window_config;

ZG_API zg_window *zg_window_create(zg_context *ctx, const zg_window_config *config);
ZG_API void zg_window_destroy(zg_window *win);
ZG_API zg_view *zg_window_view(zg_window *win);
ZG_API void zg_window_set_title(zg_window *win, const char *title);
ZG_API void zg_window_set_fullscreen(zg_window *win, int fullscreen);
ZG_API void *zg_window_native_handle(zg_window *win); /* HWND, NSWindow*, X11/Wayland surface */

#ifdef __cplusplus
}
#endif

#endif /* ZEROG_H */
