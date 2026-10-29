/* Creating and destroying everything the C API hands out, many times over, with
 * work still in flight (ZeroG.md, "Testing": memory). Meant to run under the
 * AddressSanitizer build (tools/build.py --asan), where a use after free or an
 * overflow stops it; in any build it checks that the memory the runtime reports
 * comes back to where it was:
 *   - contexts with files (copied and in place), a scheme handler and a fallback;
 *   - views loading a page with an iframe, a canvas, a blob: URL, timers and a
 *     large JavaScript object, each destroyed while a scheme request, a fetch and
 *     a binding's Promise are still pending, answered after the view (windows)
 *     or the whole context (views) is gone;
 *   - native windows the same way (not with --no-window).
 *
 * Usage: lifecycle_test [--no-window] [cycles, default 20]
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) do {                      \
        int ok_ = (condition);                          \
        printf("%s ", ok_ ? "ok  " : "FAIL");           \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        if (!ok_)                                       \
            ++failures;                                 \
    } while (0)

static zg_runtime *runtime;

static const char indexHtml[] =
    "<!DOCTYPE html><title>cycle</title>"
    "<canvas id=canvas width=128 height=128></canvas>"
    "<iframe src=\"frame.html\"></iframe>"
    "<script src=\"app.js\"></script>";

static const char frameHtml[] = "<!DOCTYPE html><p>frame</p><script>setInterval(() => {}, 5)</script>";

static const char appJs[] =
    "const canvas = document.getElementById('canvas').getContext('2d');\n"
    "canvas.fillStyle = 'teal'; canvas.fillRect(0, 0, 128, 128);\n"
    "window.big = Array.from({ length: 100000 }, (_, i) => ({ i }));\n"
    "const blobUrl = URL.createObjectURL(new Blob(['x'.repeat(100000)], { type: 'text/plain' }));\n"
    "setInterval(() => { window.ticks = (window.ticks || 0) + 1; }, 1);\n"
    "requestAnimationFrame(function frame() { requestAnimationFrame(frame); });\n"
    "fetch('native://localhost/never').catch(() => {});\n"
    "App.later().catch(() => {});\n"
    "Promise.all([fetch(blobUrl).then(r => r.text()), fetch('native://localhost/now').then(r => r.text()),\n"
    "             fetch('/settings').then(r => r.text())])\n"
    "    .then(([blob, now, fallback]) => { window.done = blob.length === 100000 && now === 'now' && fallback.includes('cycle'); },\n"
    "          e => { window.done = 'failed: ' + e; });\n";

/* ---- Work left pending when a view goes away ---- */

static zg_scheme_request *pendingRequest;
static zg_promise *pendingPromise;
static int releasedFiles;
static size_t liveHeapBytes; /* largest JavaScript heap seen with a page open */
static int loadsFinished;

static void onScheme(zg_scheme_request *req, void *user)
{
    const char *url = zg_scheme_request_url(req, NULL);
    (void)user;
    /* native:// is another origin than the page's app://. */
    zg_scheme_request_add_header(req, "Access-Control-Allow-Origin", "*");
    if (strstr(url, "/never")) {
        /* Only one is held at a time; an earlier one is answered first. */
        if (pendingRequest)
            zg_scheme_request_respond(pendingRequest, 200, "text/plain", "late", 4);
        pendingRequest = req;
        return;
    }
    zg_scheme_request_respond(req, 200, "text/plain", "now", 3);
}

static zg_value *later(zg_call *call, void *user)
{
    (void)user;
    if (pendingPromise)
        zg_promise_reject(pendingPromise, "superseded");
    return zg_call_promise(call, &pendingPromise);
}

static void answerPending(void)
{
    if (pendingRequest)
        zg_scheme_request_respond(pendingRequest, 200, "text/plain", "late", 4);
    pendingRequest = NULL;
    if (pendingPromise)
        zg_promise_resolve(pendingPromise, "\"late\"", 6);
    pendingPromise = NULL;
}

static void onRelease(void *user)
{
    (void)user;
    ++releasedFiles;
}

static void onLoadFinished(zg_view *view, const char *url, int failed, void *user)
{
    (void)view;
    (void)url;
    (void)user;
    if (!failed)
        ++loadsFinished;
}

/* ---- Helpers ---- */

static void runFor(double milliseconds)
{
    double end = zg_runtime_now_ms() + milliseconds;
    while (zg_runtime_now_ms() < end)
        zg_runtime_update(runtime);
}

static int evaluateIsTrue(zg_view *view, const char *script)
{
    zg_value *value = NULL;
    size_t length = 0;
    const char *json;
    int result;
    if (zg_view_evaluate(view, script, strlen(script), &value) != ZG_OK)
        return 0;
    json = zg_value_to_json(value, &length);
    result = json && !strcmp(json, "true");
    zg_value_release(value);
    return result;
}

/* Runs pending work until the page sets window.done (up to 10 s). */
static int waitForPage(zg_view *view)
{
    double end = zg_runtime_now_ms() + 10000;
    while (zg_runtime_now_ms() < end) {
        if (evaluateIsTrue(view, "window.done === true"))
            return 1;
        zg_runtime_update(runtime);
    }
    {
        static const char script[] = "String(window.done)";
        zg_value *value = NULL;
        size_t length = 0;
        const char *json = NULL;
        if (zg_view_evaluate(view, script, sizeof script - 1, &value) == ZG_OK)
            json = zg_value_to_json(value, &length);
        printf("     page not done: window.done is %s\n", json ? json : zg_last_error());
        zg_value_release(value);
    }
    return 0;
}

static zg_context *createContext(void)
{
    zg_context_config config = { sizeof config };
    zg_context *context;
    static const char inPlace[] = "/* used in place */";
    config.app_id = "dev.zerog.lifecycle";
    config.ephemeral = 1;
    context = zg_context_create(runtime, &config);
    if (!context)
        return NULL;
    zg_context_add_file(context, "app", "/index.html", indexHtml, sizeof indexHtml - 1, NULL, NULL, NULL);
    zg_context_add_file(context, "app", "/frame.html", frameHtml, sizeof frameHtml - 1, NULL, NULL, NULL);
    zg_context_add_file(context, "app", "/app.js", appJs, sizeof appJs - 1, NULL, NULL, NULL);
    zg_context_add_file(context, "app", "/in-place.js", inPlace, sizeof inPlace - 1, NULL, onRelease, NULL);
    zg_context_set_fallback(context, "app", "/index.html");
    zg_context_register_scheme(context, "native", onScheme, NULL);
    return context;
}

static void onJSError(zg_view *view, const zg_js_error *err, void *user)
{
    (void)view;
    (void)user;
    printf("     page error: %s\n", err->message);
}

static void prepareView(zg_view *view)
{
    zg_view_callbacks callbacks = { sizeof callbacks };
    callbacks.load_finished = onLoadFinished;
    callbacks.js_error = onJSError;
    zg_view_set_callbacks(view, &callbacks);
    zg_view_bind(view, "App.later", later, NULL);
    zg_view_load_url(view, "app://localhost/");
}

/* One context with two CPU views; each view renders, the first is destroyed with
   its work pending, the second while it is still loading. */
static int viewCycle(void)
{
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_context *context = createContext();
    zg_view *view, *loading;
    int done;
    if (!context)
        return 0;
    viewConfig.width = 320;
    viewConfig.height = 240;
    view = zg_view_create(context, &viewConfig);
    loading = zg_view_create(context, &viewConfig);
    if (!view || !loading)
        return 0;
    prepareView(view);
    done = waitForPage(view);
    zg_view_render(view, zg_runtime_now_ms());
    {
        zg_memory_stats stats = { sizeof stats };
        zg_runtime_collect_garbage(runtime);
        zg_runtime_memory_stats(runtime, &stats);
        if (stats.js_heap_bytes > liveHeapBytes)
            liveHeapBytes = stats.js_heap_bytes;
    }
    prepareView(loading);
    zg_runtime_update(runtime);
    zg_view_destroy(view);
    zg_view_destroy(loading);
    zg_runtime_update(runtime);
    /* Answered after the context too: the answers are dropped. */
    zg_context_destroy(context);
    answerPending();
    zg_runtime_update(runtime);
    return done;
}

static int windowCycle(void)
{
    zg_window_config windowConfig = { sizeof windowConfig };
    zg_context *context = createContext();
    zg_window *window;
    int done;
    if (!context)
        return 0;
    windowConfig.title = "lifecycle";
    windowConfig.width = 320;
    windowConfig.height = 240;
    window = zg_window_create(context, &windowConfig);
    if (!window)
        return 0;
    prepareView(zg_window_view(window));
    done = waitForPage(zg_window_view(window));
    runFor(50);
    zg_window_destroy(window);
    answerPending();
    zg_runtime_update(runtime);
    zg_context_destroy(context);
    return done;
}

static zg_memory_stats settledMemory(void)
{
    zg_memory_stats stats = { sizeof stats };
    int i;
    /* Detached documents go once their timers and loads are gone; a few rounds
       let the collector see everything they referenced. */
    for (i = 0; i < 4; ++i) {
        runFor(20);
        zg_runtime_collect_garbage(runtime);
    }
    zg_runtime_memory_stats(runtime, &stats);
    return stats;
}

int main(int argc, char **argv)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_memory_stats before, after;
    int withWindows = 1, cycles = 20, i, pages = 0, windowPages = 0;
    const int warmup = 3;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-window"))
            withWindows = 0;
        else
            cycles = atoi(argv[i]);
    }
    if (cycles <= warmup)
        cycles = warmup + 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime = zg_runtime_create(&runtimeConfig);
    CHECK(runtime != NULL, "runtime created");
    if (!runtime)
        return 1;

    /* Caches, fonts and JIT code warm up in the first cycles: measure after them. */
    for (i = 0; i < warmup; ++i)
        pages += viewCycle();
    before = settledMemory();
    for (; i < cycles; ++i)
        pages += viewCycle();
    after = settledMemory();
    CHECK(pages == cycles, "%d of %d pages loaded and ran before their views were destroyed", pages, cycles);
    CHECK(loadsFinished >= cycles, "load_finished for %d loads", loadsFinished);
    CHECK(releasedFiles == cycles, "every file used in place released with its context: %d of %d", releasedFiles, cycles);
    CHECK(after.asset_bytes == before.asset_bytes && after.asset_bytes == 0,
        "no asset bytes left by destroyed contexts: %zu", after.asset_bytes);
    CHECK(after.surface_bytes == 0, "no surface bytes left by destroyed views: %zu", after.surface_bytes);
    /* Each page holds about 3 MB of JavaScript objects: a leaked document per cycle
       would add tens of megabytes over the measured cycles. */
    CHECK(liveHeapBytes > 2 * 1024 * 1024, "the JavaScript heap counts an open page's objects: %zu KB", liveHeapBytes / 1024);
    CHECK(after.js_heap_bytes < before.js_heap_bytes + 4 * 1024 * 1024,
        "JavaScript heap after %d more cycles: %zu KB, was %zu KB", cycles - warmup, after.js_heap_bytes / 1024, before.js_heap_bytes / 1024);
    printf("     resource cache %zu KB, was %zu KB\n", after.resource_cache_bytes / 1024, before.resource_cache_bytes / 1024);

    if (withWindows) {
        int windowCycles = cycles / 4 > 2 ? cycles / 4 : 2;
        for (i = 0; i < windowCycles; ++i)
            windowPages += windowCycle();
        after = settledMemory();
        CHECK(windowPages == windowCycles, "%d of %d windows loaded their page before being destroyed", windowPages, windowCycles);
        CHECK(after.surface_bytes == 0, "no surface bytes left by destroyed windows: %zu", after.surface_bytes);
    }

    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
