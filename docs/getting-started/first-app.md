# Your first app

The example in
[`examples/hello`](https://github.com/dzonerzy/ZeroG/tree/main/examples/hello):
a native window showing a frontend packed into the binary, with a native
function the page calls synchronously.

## The frontend

Any static web frontend works: plain files, or the build output of Vite, React,
Svelte or anything else. `examples/hello/web/` has an `index.html`, a
stylesheet and this script:

```js
// App.greet is native code, called synchronously: no await.
const message = document.getElementById("message");
message.textContent = App.greet("Daniele");
```

Pack it into C source at build time:

```sh
python tools/assets.py examples/hello/web -o hello_assets.c --c-symbol hello_assets
```

The archive is compressed where it pays off and inflated by the engine on
first use; see [App assets and schemes](../guides/assets-and-schemes.md).

## The program

```c
#include <zerog.h>
#include <stdio.h>

extern const unsigned char hello_assets[];
extern const size_t hello_assets_len;

/* JS: App.greet(name) returns a string. */
static zg_value *on_greet(zg_call *call, void *user)
{
    size_t len;
    const char *name = zg_call_arg_string(call, 0, &len);
    char reply[128];
    snprintf(reply, sizeof reply, "Ciao %.*s", (int)len, name ? name : "");
    return zg_value_string(call, reply);
}

int main(void)
{
    zg_runtime_config runtime_config = { sizeof runtime_config };
    zg_context_config context_config = { sizeof context_config };
    zg_window_config window_config = { sizeof window_config };

    zg_runtime *rt = zg_runtime_create(&runtime_config);
    if (!rt) {
        fprintf(stderr, "zg_runtime_create: %s\n", zg_last_error());
        return 1;
    }

    context_config.app_id = "dev.example.hello";  /* names the data directory */
    zg_context *ctx = zg_context_create(rt, &context_config);
    zg_context_mount_assets(ctx, "app", hello_assets, hello_assets_len);

    window_config.title = "Hello, ZeroG";
    window_config.width = 640;
    window_config.height = 400;
    zg_window *win = zg_window_create(ctx, &window_config);
    zg_view *view = zg_window_view(win);

    zg_view_bind(view, "App.greet", on_greet, NULL);
    zg_view_load_url(view, "app://localhost/");

    zg_runtime_run(rt);       /* until the last window closes, or zg_runtime_quit */
    zg_runtime_destroy(rt);
    return 0;
}
```

Every config struct starts with `size`: set it to `sizeof` the struct, and
leave the fields you do not need at zero for their defaults. New versions of
the API append fields, and older programs keep working.

## What happens

- `zg_runtime_create` starts the engine on the calling thread, which becomes
  the runtime thread.
- The context holds session state: cookies, storage and caches in the data
  directory named after `app_id`, and the schemes it serves.
- `app://` is trusted because the application mounted it: its pages get the
  bindings. A page from anywhere else does not, unless the
  [trust rules](../guides/security.md) allow it.
- `zg_view_bind` installs `App.greet` in every page of a trusted origin, in the
  main frame, before the page's scripts run.
- `zg_runtime_run` runs the event loop: the window, rendering at the display's
  rate, network, timers.

## Next

- [Concepts](../guides/concepts.md): runtime, context, view, surface, window.
- [JavaScript bindings](../guides/bindings.md): arguments, return values,
  Promises, classes, binary data.
- [Window mode](../guides/window-mode.md): frameless and transparent windows,
  drag regions, context menus.
