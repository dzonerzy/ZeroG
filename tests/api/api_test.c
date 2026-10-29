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

static int loadsFinished;
static int lastLoadFailed;

static void onLoadFinished(zg_view *view, const char *url, int failed, void *user)
{
    (void)view;
    (void)url;
    (void)user;
    lastLoadFailed = failed;
    ++loadsFinished;
}

/* ---- Schemes and files ---- */

static zg_scheme_request *heldRequest;

#ifdef _WIN32
static DWORD WINAPI respondLater(LPVOID argument)
{
    (void)argument;
    Sleep(50);
#else
static void *respondLater(void *argument)
{
    (void)argument;
    usleep(50000);
#endif
    zg_scheme_request_add_header(heldRequest, "Access-Control-Allow-Origin", "*");
    zg_scheme_request_respond(heldRequest, 200, "text/plain", "later", 5);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* native://: answered now, later from another thread, or failed. */
static void onNativeScheme(zg_scheme_request *request, void *user)
{
    size_t length;
    const char *url = zg_scheme_request_url(request, &length);
    const char *accept = zg_scheme_request_header(request, "ACCEPT", NULL);
    char body[512];
    (void)user;
    if (strstr(url, "/later")) {
        heldRequest = request;
#ifdef _WIN32
        CloseHandle(CreateThread(NULL, 0, respondLater, NULL, 0, NULL));
#else
        {
            pthread_t thread;
            pthread_create(&thread, NULL, respondLater, NULL);
            pthread_detach(thread);
        }
#endif
        return;
    }
    if (strstr(url, "/fail")) {
        zg_scheme_request_fail(request, "refused by the test");
        return;
    }
    snprintf(body, sizeof body, "{\"method\":\"%s\",\"accept\":\"%s\",\"url\":\"%.*s\"}",
        zg_scheme_request_method(request), accept ? accept : "", (int)length, url);
    zg_scheme_request_add_header(request, "Access-Control-Allow-Origin", "*");
    zg_scheme_request_respond(request, 200, "application/json", body, strlen(body));
}

/* tests/api/assets, packed by tools/assets.py at build time. */
extern const unsigned char test_assets[];
extern const size_t test_assets_len;

static int released;

static void onRelease(void *user)
{
    (void)user;
    ++released;
}

/* An archive in tools/assets.py's format with one stored file. */
static size_t makeArchive(unsigned char *out, const char *path, const char *content)
{
    size_t pathLength = strlen(path), contentLength = strlen(content), pathOffset = 16 + 40, dataOffset = pathOffset + pathLength;
    size_t i;
    unsigned char *entry = out + 16;
    memset(out, 0, dataOffset + contentLength);
    memcpy(out, "ZGAR", 4);
    out[4] = 1; /* version */
    out[8] = 1; /* one file */
    for (i = 0; i < 8; ++i) {
        entry[i] = (unsigned char)(dataOffset >> (8 * i));
        entry[8 + i] = (unsigned char)(contentLength >> (8 * i));
        entry[16 + i] = (unsigned char)(contentLength >> (8 * i));
    }
    for (i = 0; i < 4; ++i) {
        entry[24 + i] = (unsigned char)(pathOffset >> (8 * i));
        entry[28 + i] = (unsigned char)(pathLength >> (8 * i));
    }
    memcpy(out + pathOffset, path, pathLength);
    memcpy(out + dataOffset, content, contentLength);
    return dataOffset + contentLength;
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

/* Evaluates an async expression and waits (up to 10 s) for its value as a string. */
static char *awaitJS(zg_view *view, const char *expression)
{
    static char script[1024];
    int i;
    snprintf(script, sizeof script, "window.__result = undefined; (async () => { try { window.__result = String(await (%s)); }"
        " catch (e) { window.__result = 'error: ' + e; } })(); 0", expression);
    evaluate(view, script);
    for (i = 0; i < 2000 && strcmp(evaluate(view, "typeof window.__result"), "\"string\""); ++i) {
        sleepMilliseconds(5);
        frame(view);
    }
    return evaluate(view, "window.__result");
}

static int waitForLoad(zg_view *view, int previous)
{
    int i;
    for (i = 0; i < 2000 && loadsFinished == previous; ++i) {
        sleepMilliseconds(5);
        frame(view);
    }
    return loadsFinished > previous && !lastLoadFailed;
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
    CHECK(zg_view_load_url(view, "not a URL") == ZG_ERROR_INVALID_ARGUMENT, "an invalid URL is refused: %s", zg_last_error());

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

    /* The application's files and schemes. */
    {
        static const char appPage[] = "<!DOCTYPE html><title>App page</title><script>window.fromApp = App.add(20, 22);</script>";
        static const char staticText[] = "used in place";
        static unsigned char archive[256];
        size_t archiveLength = makeArchive(archive, "/pkg/hello.txt", "from the archive");
        zg_memory_stats stats = { sizeof stats };
        int loads;

        CHECK(zg_context_add_file(context, "app", "/index.html", appPage, sizeof appPage - 1, NULL, NULL, NULL) == ZG_OK, "add a file (copied)");
        CHECK(zg_context_add_file(context, "app", "/index.html", "x", 1, NULL, NULL, NULL) == ZG_ERROR_ALREADY_EXISTS, "files are write-once: %s", zg_last_error());
        CHECK(zg_context_add_file(context, "app", "/../secret", "x", 1, NULL, NULL, NULL) == ZG_ERROR_INVALID_ARGUMENT, "'..' is refused: %s", zg_last_error());
        CHECK(zg_context_add_file(context, "app", "/static.txt", staticText, sizeof staticText - 1, "text/plain", onRelease, NULL) == ZG_OK, "add a file (in place)");
        CHECK(zg_context_mount_assets(context, "app", archive, archiveLength) == ZG_OK, "mount an archive");
        CHECK(zg_context_mount_assets(context, "app", archive, archiveLength) == ZG_ERROR_ALREADY_EXISTS, "an archive cannot replace files: %s", zg_last_error());
        CHECK(zg_context_mount_assets(context, "app", "junk", 4) == ZG_ERROR_INVALID_ARGUMENT, "a malformed archive is refused: %s", zg_last_error());
        CHECK(zg_context_register_scheme(context, "app", onNativeScheme, NULL) == ZG_ERROR_ALREADY_EXISTS, "a file scheme has no handler: %s", zg_last_error());
        CHECK(zg_context_register_scheme(context, "native", onNativeScheme, NULL) == ZG_OK, "register native://");
        CHECK(zg_context_register_scheme(context, "https", onNativeScheme, NULL) == ZG_ERROR_INVALID_ARGUMENT, "built-in schemes are refused: %s", zg_last_error());
        CHECK(zg_context_add_file(context, "native", "/x", "x", 1, NULL, NULL, NULL) == ZG_ERROR_ALREADY_EXISTS, "a handler scheme serves no files: %s", zg_last_error());

        callbacks.load_finished = onLoadFinished;
        zg_view_set_callbacks(view, &callbacks);
        loads = loadsFinished;
        CHECK(zg_view_load_url(view, "app://localhost/index.html") == ZG_OK && waitForLoad(view, loads), "app://localhost/index.html loads");
        /* The title reaches the callback in a task of the page's event loop. */
        for (i = 0; i < 200 && strcmp(lastTitle, "App page"); ++i) {
            sleepMilliseconds(5);
            frame(view);
        }
        CHECK(!strcmp(lastTitle, "App page"), "its title: '%s'", lastTitle);
        CHECK(!strcmp(evaluate(view, "fromApp"), "42"), "the app's origin gets the bindings: %s", evaluate(view, "fromApp"));
        CHECK(!strcmp(awaitJS(view, "fetch('/static.txt').then(r => r.text())"), "\"used in place\""), "a file used in place: %s", evaluate(view, "__result"));
        CHECK(!strcmp(awaitJS(view, "fetch('/pkg/hello.txt').then(r => r.text())"), "\"from the archive\""), "a file from the archive: %s", evaluate(view, "__result"));
        CHECK(!strcmp(awaitJS(view, "fetch('native://x/hello?q=1').then(r => r.json()).then(j => [j.method, j.accept, j.url].join(' '))"),
            "\"GET */* native://x/hello?q=1\""), "a scheme handler answers: %s", evaluate(view, "__result"));
        CHECK(!strcmp(awaitJS(view, "fetch('native://x/later').then(r => r.text())"), "\"later\""), "and answers from another thread: %s", evaluate(view, "__result"));
        CHECK(!strncmp(awaitJS(view, "fetch('native://x/fail').then(r => r.text())"), "\"error:", 7), "or fails a request: %s", evaluate(view, "__result"));

        /* A frontend packed by tools/assets.py, under a scheme of its own. */
        CHECK(zg_context_mount_assets(context, "packed", test_assets, test_assets_len) == ZG_OK, "mount the tools/assets.py archive");
        loads = loadsFinished;
        CHECK(zg_view_load_url(view, "packed://localhost/") == ZG_OK && waitForLoad(view, loads), "packed://localhost/ loads its index.html");
        CHECK(!strcmp(evaluate(view, "packed"), "\"rgb(1, 2, 3) H\xc3\xa9llo from an archive\""), "its stylesheet (compressed) and script ran: %s", evaluate(view, "packed"));

        /* data: is not the app's content: the navigation is blocked. */
        loads = loadsFinished;
        zg_view_load_url(view, "data:text/html,<title>data</title>");
        for (i = 0; i < 40; ++i) {
            sleepMilliseconds(5);
            frame(view);
        }
        CHECK(!strcmp(evaluate(view, "location.href"), "\"packed://localhost/\""), "a data: navigation is blocked: %s", evaluate(view, "location.href"));

        zg_runtime_memory_stats(runtime, &stats);
        CHECK(stats.asset_bytes >= sizeof appPage - 1, "memory stats count the copied files: %zu bytes", stats.asset_bytes);
    }
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

    CHECK(!released, "a file used in place stays while its context lives");
    zg_context_destroy(context);
    /* Script objects of its pages (a fetch's Response) go with the next collection. */
    for (i = 0; i < 100 && !released; ++i) {
        zg_runtime_collect_garbage(runtime);
        zg_runtime_update(runtime);
    }
    CHECK(released == 1, "its release callback runs once the context is gone (%d)", released);

    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
