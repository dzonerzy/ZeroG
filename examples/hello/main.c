/* The example of ZeroG.md ("Esempio d'uso"): a native window showing the
 * application's frontend from app://, with a synchronous binding.
 *
 * The frontend (web/) is packed into the binary at build time by
 * tools/assets.py (hello_assets.c). Usage: hello [--smoke]
 * --smoke quits once the page has called App.greet: a test for the build.
 */

#include <zerog.h>

#include <stdio.h>
#include <string.h>

extern const unsigned char hello_assets[];
extern const size_t hello_assets_len;

static zg_runtime *rt;
static int greeted;

/* JS: const message = App.greet("Daniele") — synchronous, no await. */
static zg_value *on_greet(zg_call *call, void *user)
{
    size_t len;
    const char *name = zg_call_arg_string(call, 0, &len);
    char reply[128];
    (void)user;
    snprintf(reply, sizeof reply, "Ciao %.*s", (int)len, name ? name : "");
    greeted = 1;
    return zg_value_string(call, reply);
}

static zg_value *on_quit(zg_call *call, void *user)
{
    (void)call;
    (void)user;
    zg_runtime_quit(rt);
    return NULL;
}

static void on_console(zg_view *view, const zg_log_message *msg, void *user)
{
    (void)view;
    (void)user;
    printf("console: %.*s\n", (int)msg->message_len, msg->message);
}

int main(int argc, char **argv)
{
    int smoke = argc > 1 && !strcmp(argv[1], "--smoke");
    zg_runtime_config runtime_config = { sizeof runtime_config };
    zg_context_config context_config = { sizeof context_config };
    zg_window_config window_config = { sizeof window_config };
    zg_view_callbacks callbacks = { sizeof callbacks };
    zg_context *ctx;
    zg_window *win;
    zg_view *view;

    rt = zg_runtime_create(&runtime_config);
    if (!rt) {
        fprintf(stderr, "zg_runtime_create: %s\n", zg_last_error());
        return 1;
    }
    context_config.app_id = "dev.example.hello";
    context_config.ephemeral = 1;
    ctx = zg_context_create(rt, &context_config);
    if (zg_context_mount_assets(ctx, "app", hello_assets, hello_assets_len) != ZG_OK) {
        fprintf(stderr, "zg_context_mount_assets: %s\n", zg_last_error());
        return 1;
    }

    window_config.title = "Hello, ZeroG";
    window_config.width = 640;
    window_config.height = 400;
    win = zg_window_create(ctx, &window_config);
    if (!win) {
        fprintf(stderr, "zg_window_create: %s\n", zg_last_error());
        return 1;
    }
    view = zg_window_view(win);
    callbacks.console = on_console;
    zg_view_set_callbacks(view, &callbacks);
    zg_view_bind(view, "App.greet", on_greet, NULL);
    zg_view_bind(view, "App.quit", on_quit, NULL);
    zg_view_load_url(view, smoke ? "app://localhost/#smoke" : "app://localhost/");

    zg_runtime_run(rt);
    zg_runtime_destroy(rt);
    if (smoke && !greeted) {
        fprintf(stderr, "the page did not call App.greet\n");
        return 1;
    }
    return 0;
}
