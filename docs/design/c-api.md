# Public C API

The API is pure C99, with opaque handles, no WebKit type exposed, and the `zg_` prefix. The `WK` prefix must be avoided because it is already used by WebKit's C API.

## Principles

- **Opaque handles** with explicit creation and destruction (`zg_view_create` / `zg_view_destroy`). No internal structure in the header.
- **A single thread**: all calls happen on the thread that created the runtime, and callbacks arrive on the same thread.
- **ABI stability**: configuration structs with an initial `size` field, so fields can be added without breaking clients.
- **Explicit errors**: functions that can fail return `zg_result` and a readable message with `zg_last_error()`.
- **UTF-8 strings** with explicit length where needed.

## Objects

| Object | Responsibility |
| --- | --- |
| `zg_runtime` | Global initialization, loop, engine advancement |
| `zg_context` | Session: data directory, custom schemes, networking, registered fonts |
| `zg_view` | A page: loading, dimensions, input, rendering, JavaScript |
| `zg_window` | Optional native window hosting a view (window mode) |
| `zg_surface` | Rendering destination in texture or CPU mode |
| `zg_value` | JavaScript value as seen by the host, with conversions to C types |

## Loop

- `zg_runtime_run()`: the engine owns the loop. It is the desktop framework case.
- `zg_runtime_update()`: one advancement step (timers, network responses, pending tasks), to be called at every tick of the application's loop.
- `zg_runtime_wakeup_handle()`: handle to wait on when the application is idle, signaled when the engine has work to do.

## Direct JavaScript bindings

With a single process the page's JavaScript runs in the same process and on the same thread as the application. Native functions are registered **directly in the page's JSC context**, with JavaScriptCore's C API, without IPC.

- **Synchronous calls (default)**: the native callback runs during the JS call and the return value reaches the JavaScript immediately (`const r = App.greet("Daniele")`).
- **Asynchronous calls (supported, to be declared)**: for long work the callback creates a `Promise` with `zg_call_promise`, returns it immediately to the JavaScript and runs the work on an application thread. `zg_promise_resolve` / `zg_promise_reject` can be called from any thread: they receive plain C data (JSON or UTF-8) that the engine copies, and the engine resolves the `Promise` on the main thread. JSC values (`zg_value`) never cross threads.
- **Installation**: the bindings are installed when WebCore creates a frame's `window` object (the frame loader client's "window object cleared" callback), only for trusted origins, and only in the main frame unless explicitly requested. The origin is also checked at every call, not only at installation.
- **Trusted origins**, in three levels:
  1. **automatic (default)**: the application's assets (`app://` and the schemes registered with `zg_context_register_scheme`). No configuration.
  2. **dev server**: a single field, `dev_server_url` in `zg_context_config`, set by the framework in the dev profile. The engine ignores it in release profiles, so it cannot end up in production by mistake.
  3. **optional policy**: a host callback that decides at runtime whether an origin can navigate or receive the bindings, for the cases the first two levels do not cover. Optional.
- **Events to JS**: `zg_view_emit` creates and dispatches a `CustomEvent` directly on the page's `window`.

## Usage example

```c
#include <zerog.h>

static zg_value *on_greet(zg_call *call, void *user) {
    size_t len;
    const char *name = zg_call_arg_string(call, 0, &len);
    char reply[128];
    snprintf(reply, sizeof reply, "Ciao %.*s", (int)len, name);
    return zg_value_string(call, reply);
}

int main(void) {
    zg_runtime *rt = zg_runtime_create(&(zg_runtime_config){ .size = sizeof(zg_runtime_config) });
    zg_context *ctx = zg_context_create(rt, &(zg_context_config){
        .size = sizeof(zg_context_config),
        .app_id = "dev.example.demo",
    });
    zg_context_mount_assets(ctx, "app", embedded_assets, embedded_assets_len);

    zg_window *win = zg_window_create(ctx, &(zg_window_config){
        .size = sizeof(zg_window_config), .title = "Demo", .width = 1024, .height = 768,
    });
    zg_view *view = zg_window_view(win);
    zg_view_bind(view, "App.greet", on_greet, NULL);
    zg_view_load_url(view, "app://index.html");

    zg_runtime_run(rt);
    zg_runtime_destroy(rt);
    return 0;
}
```

JS side: `const msg = App.greet("Daniele")`, without `await`. The Zig framework generates the typed bindings on top of this API.

## Developer diagnostics

- **Console**: every `console.log`, `warn` and `error` reaches a host callback with level, message, file and line. The framework prints it in the terminal in dev mode.
- **Unhandled JS errors**: dedicated callback with message and stack trace, also for `Promise`s rejected without a handler.
- **Web Inspector**: in the dev and debug profiles the engine starts WebKit's remote inspector on a local port, and the inspector frontend (included in the binary only in these profiles) opens in a second window. In the minimal and standard profiles it is not there, for size and attack surface.
- **Internal engine logs**: configurable levels, routed to the same callback, so the host decides where to write them.
