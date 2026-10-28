/* Compile-only check: zerog.h is valid C99 and the usage example from
   ZeroG.md ("Esempio d'uso") compiles against it unchanged. */
#include <zerog.h>
#include <stdio.h>

static const unsigned char embedded_assets[] = { 0 };
static const size_t embedded_assets_len = sizeof embedded_assets;

static zg_value *on_greet(zg_call *call, void *user) {
    size_t len;
    const char *name = zg_call_arg_string(call, 0, &len);
    char reply[128];
    (void)user;
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
