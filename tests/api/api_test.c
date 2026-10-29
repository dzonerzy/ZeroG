/* The C API from a C99 program that sees only include/zerog.h: runtime and
 * errors, a CPU view with bindings (synchronous, and a Promise resolved from
 * another thread), evaluate, input, callbacks, frames, and a window whose loop a
 * binding ends.
 *
 * Usage: api_test [--no-window]
 */

#include "zerog.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

static int failures;

#define CHECK(condition, ...) do {                      \
        int ok_ = (condition);                          \
        printf("%s ", ok_ ? "ok  " : "FAIL");           \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        if (!ok_)                                       \
            ++failures;                                 \
    } while (0)

/* ---- Bindings ---- */

static zg_value *add(zg_call *call, void *user)
{
    (void)user;
    if (zg_call_argc(call) != 2 || zg_call_arg_type(call, 0) != ZG_TYPE_NUMBER)
        return zg_call_throw(call, "add expects two numbers");
    return zg_value_number(call, zg_call_arg_number(call, 0) + zg_call_arg_number(call, 1));
}

static zg_value *describe(zg_call *call, void *user)
{
    size_t length;
    const char *json = zg_call_arg_json(call, 0, &length);
    char reply[256];
    (void)user;
    snprintf(reply, sizeof reply, "{\"received\":%.*s,\"origin\":\"%s\"}", (int)length, json, zg_call_origin(call));
    return zg_value_json(call, reply, strlen(reply));
}

static zg_value *big(zg_call *call, void *user)
{
    (void)user;
    return zg_value_int64(call, INT64_C(9007199254740993)); /* 2^53 + 1: a BigInt */
}

static zg_promise *pending;

#ifdef _WIN32
static DWORD WINAPI resolveLater(LPVOID argument)
{
    (void)argument;
    Sleep(50);
    zg_promise_resolve(pending, "{\"done\":true}", 13);
    return 0;
}
#else
static void *resolveLater(void *argument)
{
    (void)argument;
    usleep(50000);
    zg_promise_resolve(pending, "{\"done\":true}", 13);
    return NULL;
}
#endif

/* A Promise settled from another thread, as a host's worker would. */
static zg_value *later(zg_call *call, void *user)
{
    zg_value *promise = zg_call_promise(call, &pending);
    (void)user;
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, resolveLater, NULL, 0, NULL));
#else
    pthread_t thread;
    pthread_create(&thread, NULL, resolveLater, NULL);
    pthread_detach(thread);
#endif
    return promise;
}

static zg_runtime *runtime;

static zg_value *quit(zg_call *call, void *user)
{
    (void)call;
    (void)user;
    zg_runtime_quit(runtime);
    return NULL;
}

/* ---- Callbacks ---- */

static char lastConsole[256];
static char lastTitle[128];
static zg_cursor lastCursor = ZG_CURSOR_DEFAULT;
static int jsErrors;

static void onConsole(zg_view *view, const zg_log_message *message, void *user)
{
    (void)view;
    (void)user;
    snprintf(lastConsole, sizeof lastConsole, "%.*s", (int)message->message_len, message->message);
}

static void onJSError(zg_view *view, const zg_js_error *error, void *user)
{
    (void)view;
    (void)error;
    (void)user;
    ++jsErrors;
}

static void onTitle(zg_view *view, const char *title, size_t length, void *user)
{
    (void)view;
    (void)user;
    snprintf(lastTitle, sizeof lastTitle, "%.*s", (int)length, title);
}

static void onCursor(zg_view *view, zg_cursor cursor, void *user)
{
    (void)view;
    (void)user;
    lastCursor = cursor;
}

/* ---- Helpers ---- */

static char *evaluate(zg_view *view, const char *script)
{
    zg_value *value = NULL;
    static char text[512];
    size_t length;
    const char *json;
    if (zg_view_evaluate(view, script, strlen(script), &value) != ZG_OK) {
        snprintf(text, sizeof text, "<error: %s>", zg_last_error());
        return text;
    }
    json = zg_value_to_json(value, &length);
    snprintf(text, sizeof text, "%.*s", (int)length, json);
    zg_value_release(value);
    return text;
}

static void frame(zg_view *view)
{
    zg_runtime_update(runtime);
    zg_view_render(view, zg_runtime_now_ms());
}

static void sleepMilliseconds(int milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}

static const char page[] =
    "<!DOCTYPE html><html><head><title>API test</title><style>"
    "body { margin: 0; background: rgb(0, 128, 255); }"
    "#target { position: absolute; left: 10px; top: 10px; width: 100px; height: 50px; cursor: pointer; }"
    "</style></head><body><div id='target'></div><script>"
    "window.results = { sum: App.add(2, 3), described: App.describe({ a: [1, 2] }), big: typeof App.big() };"
    "try { App.add('x'); } catch (e) { results.thrown = e.message; }"
    "App.later().then(value => { results.later = value; });"
    "document.getElementById('target').onclick = e => { results.clicked = e.clientX + ',' + e.clientY; };"
    "window.addEventListener('native', e => { results.native = e.detail; });"
    "console.log('hello from the page');"
    "</script></body></html>";

int main(int argc, char **argv)
{
    int withWindow = !(argc > 1 && !strcmp(argv[1], "--no-window"));
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_view_callbacks callbacks = { sizeof callbacks };
    zg_context *context;
    zg_view *view;
    zg_frame frameData;
    zg_version_info version = zg_version();
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("     ZeroG %d.%d.%d, WebKit %.12s, profile %s\n", version.major, version.minor, version.patch, version.webkit_revision, version.profile);

    runtime = zg_runtime_create(&runtimeConfig);
    CHECK(runtime != NULL, "runtime created");
    CHECK(zg_runtime_create(&runtimeConfig) == NULL, "a second runtime is refused: %s", zg_last_error());

    context = zg_context_create(runtime, &contextConfig);
    viewConfig.width = 300;
    viewConfig.height = 200;
    view = zg_view_create(context, &viewConfig);
    CHECK(view != NULL && zg_view_surface(view) != NULL, "CPU view with a surface");

    callbacks.console = onConsole;
    callbacks.js_error = onJSError;
    callbacks.title_changed = onTitle;
    callbacks.cursor_changed = onCursor;
    zg_view_set_callbacks(view, &callbacks);

    CHECK(zg_view_bind(view, "App.add", add, NULL) == ZG_OK, "bind App.add");
    zg_view_bind(view, "App.describe", describe, NULL);
    zg_view_bind(view, "App.big", big, NULL);
    zg_view_bind(view, "App.later", later, NULL);
    CHECK(zg_view_bind(view, "", add, NULL) == ZG_ERROR_INVALID_ARGUMENT, "an empty binding path is refused");
    CHECK(zg_view_load_url(view, "https://example.com/") == ZG_ERROR_UNSUPPORTED, "URL loading not yet: %s", zg_last_error());

    CHECK(zg_view_load_html(view, page, sizeof page - 1, NULL) == ZG_OK, "load HTML");
    frame(view);

    CHECK(!strcmp(evaluate(view, "results.sum"), "5"), "synchronous binding: %s", evaluate(view, "results.sum"));
    CHECK(strstr(evaluate(view, "results.described"), "\"received\":{\"a\":[1,2]}") != NULL, "JSON argument and result: %s", evaluate(view, "results.described"));
    CHECK(!strcmp(evaluate(view, "results.big"), "\"bigint\""), "int64 beyond 2^53 is a BigInt");
    CHECK(!strcmp(evaluate(view, "results.thrown"), "\"add expects two numbers\""), "binding throws into JavaScript");
    for (i = 0; i < 100 && strcmp(evaluate(view, "results.later ? results.later.done : false"), "true"); ++i) {
        sleepMilliseconds(5);
        frame(view);
    }
    CHECK(!strcmp(evaluate(view, "JSON.stringify(results.later)"), "\"{\\\"done\\\":true}\""), "Promise resolved from another thread: %s", evaluate(view, "JSON.stringify(results.later)"));

    CHECK(!strcmp(lastConsole, "hello from the page"), "console callback: '%s'", lastConsole);
    CHECK(!strcmp(lastTitle, "API test"), "title callback: '%s'", lastTitle);
    {
        static const char throwing[] = "throw new Error('boom')";
        CHECK(zg_view_evaluate(view, throwing, strlen(throwing), NULL) == ZG_ERROR_JS_EXCEPTION && strstr(zg_last_error(), "boom"),
            "evaluate reports exceptions: %s", zg_last_error());
    }
    CHECK(zg_view_emit(view, "native", "{\"n\":7}", 7) == ZG_OK && !strcmp(evaluate(view, "results.native.n"), "7"), "zg_view_emit dispatches a CustomEvent");

    /* Input. */
    zg_view_mouse_event(view, ZG_MOUSE_MOVE, 40, 30, ZG_MOUSE_NONE, 0, 0);
    CHECK(lastCursor == ZG_CURSOR_POINTER, "cursor callback over cursor: pointer");
    zg_view_mouse_event(view, ZG_MOUSE_DOWN, 40, 30, ZG_MOUSE_LEFT, 1, 0);
    zg_view_mouse_event(view, ZG_MOUSE_UP, 40, 30, ZG_MOUSE_LEFT, 1, 0);
    CHECK(!strcmp(evaluate(view, "results.clicked"), "\"40,30\""), "click through the API: %s", evaluate(view, "results.clicked"));
    evaluate(view, "const input = document.createElement('input'); document.body.appendChild(input); input.focus(); 0");
    zg_view_key_event(view, ZG_KEY_DOWN, "h", "KeyH", NULL, 0, 0);
    zg_view_key_event(view, ZG_KEY_CHAR, NULL, NULL, "h", 0, 0);
    zg_view_key_event(view, ZG_KEY_UP, "h", "KeyH", NULL, 0, 0);
    zg_view_key_event(view, ZG_KEY_CHAR, NULL, NULL, "\xc3\xa9", 0, 0); /* é */
    zg_view_ime_set_composition(view, "\xe6\x97\xa5", 3, 1, 1);    /* 日, being composed */
    zg_view_ime_commit(view, "\xe6\x97\xa5\xe6\x9c\xac", 6);       /* 日本 */
    CHECK(!strcmp(evaluate(view, "input.value"), "\"h\xc3\xa9\xe6\x97\xa5\xe6\x9c\xac\""), "keys, text and IME commit: %s", evaluate(view, "input.value"));

    /* Frames. */
    frame(view);
    CHECK(zg_surface_acquire(zg_view_surface(view), &frameData) == ZG_OK, "acquire a frame");
    {
        const unsigned char *pixel = (const unsigned char *)frameData.pixels + 150 * frameData.stride + 250 * 4;
        CHECK(frameData.width == 300 && frameData.height == 200 && pixel[0] == 255 && pixel[1] == 128 && pixel[2] == 0,
            "frame pixels are the page's background (BGRA %d,%d,%d)", pixel[0], pixel[1], pixel[2]);
        zg_surface_release(zg_view_surface(view), &frameData);
    }
    CHECK(zg_surface_acquire(zg_view_surface(view), &frameData) == ZG_ERROR_NOT_FOUND, "no newer frame");
    CHECK(jsErrors == 0, "no JavaScript errors");
    zg_view_destroy(view);

    if (withWindow) {
        zg_window_config windowConfig = { sizeof windowConfig };
        zg_window *window;
        windowConfig.title = "ZeroG API test";
        windowConfig.width = 320;
        windowConfig.height = 200;
        window = zg_window_create(context, &windowConfig);
        CHECK(window != NULL, "window created %s", window ? "" : zg_last_error());
        if (window) {
            static const char windowPage[] =
                "<body style='background: teal'><script>"
                "let frames = 0; (function tick() { if (++frames == 30) App.quit(); else requestAnimationFrame(tick); })();"
                "</script></body>";
            zg_view_bind(zg_window_view(window), "App.quit", quit, NULL);
            zg_view_load_html(zg_window_view(window), windowPage, sizeof windowPage - 1, NULL);
            CHECK(zg_runtime_run(runtime) == ZG_OK, "zg_runtime_run returns when a binding calls zg_runtime_quit");
            CHECK(!strcmp(evaluate(zg_window_view(window), "frames"), "30"), "the window ran 30 animation frames");
            zg_window_destroy(window);
        }
    }

    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
