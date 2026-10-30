/* Web storage and IndexedDB through the C API: what a context keeps in its data
 * directory, and what it does not.
 *   - localStorage persists in data_dir across contexts (and reaches the disk),
 *     is separate per data_dir and per origin, has its quota, and fires storage
 *     events in the context's other views;
 *   - sessionStorage belongs to one view;
 *   - IndexedDB persists the same way, with structured values;
 *   - an ephemeral context keeps both in memory: nothing on disk, nothing left
 *     for the next context.
 *
 * Usage: storage_test
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static const char *evaluate(zg_view *view, const char *script)
{
    static char text[512];
    zg_value *value = NULL;
    size_t length;
    const char *json;
    if (zg_view_evaluate(view, script, strlen(script), &value) != ZG_OK) {
        snprintf(text, sizeof text, "<error: %s>", zg_last_error());
        return text;
    }
    json = zg_value_to_json(value, &length);
    snprintf(text, sizeof text, "%.*s", (int)length, json ? json : "(null)");
    zg_value_release(value);
    return text;
}

static int isTrue(zg_view *view, const char *script)
{
    return !strcmp(evaluate(view, script), "true");
}

/* Runs pending work until `expression` is true in the page (up to 5 s). */
static int waitFor(zg_view *view, const char *expression)
{
    double end = zg_runtime_now_ms() + 5000;
    while (zg_runtime_now_ms() < end) {
        if (isTrue(view, expression))
            return 1;
        zg_runtime_update(runtime);
    }
    return 0;
}

static int exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static char dataDir[512], otherDir[512], ephemeralDir[512];

static zg_context *createContext(const char *directory, int ephemeral)
{
    zg_context_config config = { sizeof config };
    zg_context *context;
    config.app_id = "dev.zerog.storage-test";
    config.data_dir = directory;
    config.ephemeral = ephemeral;
    context = zg_context_create(runtime, &config);
    /* app:// with files: a page origin of the context's own, trusted. */
    zg_context_add_file(context, "app", "/index.html", "<!DOCTYPE html><title>storage</title>", 37, "text/html", NULL, NULL);
    return context;
}

static zg_view *openView(zg_context *context, const char *url)
{
    zg_view_config config = { sizeof config };
    zg_view *view = zg_view_create(context, &config);
    zg_view_load_url(view, url);
    waitFor(view, "document.readyState === 'complete' && location.protocol === 'app:'");
    return view;
}

/* Destroys a context and lets its storage threads finish writing. */
static void closeContext(zg_context *context)
{
    int i;
    zg_context_destroy(context);
    for (i = 0; i < 20; ++i) {
        zg_runtime_collect_garbage(runtime);
        zg_runtime_update(runtime);
    }
}

static const char idbPut[] =
    "window.idbDone = null;"
    "(() => { const r = indexedDB.open('notes', 1);"
    "  r.onupgradeneeded = () => r.result.createObjectStore('items');"
    "  r.onerror = () => window.idbDone = 'open failed: ' + r.error;"
    "  r.onsuccess = () => { const db = r.result; const t = db.transaction('items', 'readwrite');"
    "    t.objectStore('items').put({ text: 'h\\u00e9llo', when: new Date(0), list: [1, 2, 3], bytes: new Uint8Array([7, 8]) }, 'first');"
    "    t.oncomplete = () => { db.close(); window.idbDone = 'stored'; };"
    "    t.onerror = () => window.idbDone = 'put failed: ' + t.error; }; })(); 0";

static const char idbGet[] =
    "window.idbDone = null;"
    "(() => { const r = indexedDB.open('notes', 1);"
    "  r.onupgradeneeded = () => r.result.createObjectStore('items');"
    "  r.onerror = () => window.idbDone = 'open failed: ' + r.error;"
    "  r.onsuccess = () => { const db = r.result; const g = db.transaction('items').objectStore('items').get('first');"
    "    g.onsuccess = () => { const v = g.result; db.close();"
    "      window.idbDone = v ? [v.text, v.when instanceof Date && v.when.getTime(), v.list.join(), v.bytes instanceof Uint8Array && v.bytes.join()].join('|') : 'missing'; };"
    "    g.onerror = () => window.idbDone = 'get failed: ' + g.error; }; })(); 0";

static void tempDirectory(char *out, size_t size, const char *name)
{
    const char *base = getenv("TEMP");
    if (!base)
        base = getenv("TMPDIR");
    if (!base)
        base = "/tmp";
    snprintf(out, size, "%s/%s", base, name);
}

int main(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context *context;
    zg_view *view, *second;
    char path[700];

    setvbuf(stdout, NULL, _IONBF, 0);
    tempDirectory(dataDir, sizeof dataDir, "zerog-storage-test");
    tempDirectory(otherDir, sizeof otherDir, "zerog-storage-test-other");
    tempDirectory(ephemeralDir, sizeof ephemeralDir, "zerog-storage-test-ephemeral");
    runtime = zg_runtime_create(&runtimeConfig);

    /* ---- A context with a data directory ---- */
    context = createContext(dataDir, 0);
    view = openView(context, "app://localhost/");
    CHECK(isTrue(view, "window.localStorage !== null && window.sessionStorage !== null"), "localStorage and sessionStorage exist: %s",
        evaluate(view, "[typeof localStorage, String(localStorage)]"));
    /* Left by an earlier run: start clean. */
    evaluate(view, "localStorage.clear(); indexedDB.deleteDatabase('notes'); 0");
    zg_runtime_update(runtime);
    CHECK(isTrue(view, "(() => { localStorage.setItem('kept', 'h\\u00e9llo'); return localStorage.getItem('kept') === 'h\\u00e9llo' && localStorage.length === 1; })()"),
        "localStorage stores a value");
    CHECK(isTrue(view, "(() => { try { localStorage.setItem('big', 'x'.repeat(6 * 1024 * 1024)); return false; } catch (e) { return e.name === 'QuotaExceededError'; } })()"),
        "and has a quota");
    evaluate(view, idbPut);
    CHECK(waitFor(view, "window.idbDone !== null") && !strcmp(evaluate(view, "idbDone"), "\"stored\""), "IndexedDB stores a value: %s", evaluate(view, "idbDone"));

    /* Another view of the context: shares localStorage, gets storage events, has its own sessionStorage. */
    second = openView(context, "app://localhost/");
    /* A window gets storage events once it uses that storage, as in WebKit. */
    evaluate(second, "window.events = []; addEventListener('storage', e => events.push(e.key + '=' + e.newValue)); localStorage.length");
    evaluate(view, "sessionStorage.setItem('mine', '1'); localStorage.setItem('shared', 'yes'); 0");
    CHECK(isTrue(second, "localStorage.getItem('shared') === 'yes'"), "the context's views share localStorage");
    CHECK(waitFor(second, "events.includes('shared=yes')"), "and get its storage events: %s", evaluate(second, "events"));
    CHECK(isTrue(second, "sessionStorage.getItem('mine') === null") && isTrue(view, "sessionStorage.getItem('mine') === '1'"), "sessionStorage belongs to one view");
    zg_view_destroy(second);
    zg_view_destroy(view);
    closeContext(context);

    snprintf(path, sizeof path, "%s/LocalStorage", dataDir);
    CHECK(exists(path), "local storage is on disk: %s", path);
    snprintf(path, sizeof path, "%s/IndexedDB", dataDir);
    CHECK(exists(path), "IndexedDB is on disk: %s", path);

    /* ---- A new context on the same directory sees it all ---- */
    context = createContext(dataDir, 0);
    view = openView(context, "app://localhost/");
    CHECK(isTrue(view, "localStorage.getItem('kept') === 'h\\u00e9llo' && localStorage.getItem('shared') === 'yes'"), "localStorage persists across contexts: %s",
        evaluate(view, "JSON.stringify(Object.entries(localStorage))"));
    CHECK(isTrue(view, "sessionStorage.length === 0"), "sessionStorage does not");
    evaluate(view, idbGet);
    CHECK(waitFor(view, "window.idbDone !== null") && !strcmp(evaluate(view, "idbDone"), "\"h\xc3\xa9llo|0|1,2,3|7,8\""), "IndexedDB persists across contexts: %s", evaluate(view, "idbDone"));
    zg_view_destroy(view);

    /* Another origin of the same context has storage of its own. */
    zg_context_add_file(context, "other", "/index.html", "<!DOCTYPE html>", 15, "text/html", NULL, NULL);
    view = zg_view_create(context, NULL);
    zg_view_load_url(view, "other://localhost/");
    waitFor(view, "document.readyState === 'complete' && location.protocol === 'other:'");
    CHECK(isTrue(view, "localStorage.getItem('kept') === null"), "each origin has its own: %s", evaluate(view, "[location.origin, localStorage.length]"));
    zg_view_destroy(view);
    closeContext(context);

    /* ---- Another directory: nothing of the first ---- */
    context = createContext(otherDir, 0);
    view = openView(context, "app://localhost/");
    CHECK(isTrue(view, "localStorage.getItem('kept') === null"), "another data_dir has its own storage");
    evaluate(view, idbGet);
    CHECK(waitFor(view, "window.idbDone !== null") && !strcmp(evaluate(view, "idbDone"), "\"missing\""), "and its own databases: %s", evaluate(view, "idbDone"));
    zg_view_destroy(view);
    closeContext(context);

    /* ---- Ephemeral: memory only ---- */
    context = createContext(ephemeralDir, 1);
    view = openView(context, "app://localhost/");
    evaluate(view, "localStorage.setItem('gone', '1'); 0");
    evaluate(view, idbPut);
    CHECK(isTrue(view, "localStorage.getItem('gone') === '1'"), "an ephemeral context has localStorage");
    CHECK(waitFor(view, "window.idbDone !== null") && !strcmp(evaluate(view, "idbDone"), "\"stored\""), "and IndexedDB: %s", evaluate(view, "idbDone"));
    zg_view_destroy(view);
    closeContext(context);
    snprintf(path, sizeof path, "%s/LocalStorage", ephemeralDir);
    CHECK(!exists(path), "but writes nothing to disk");
    context = createContext(ephemeralDir, 1);
    view = openView(context, "app://localhost/");
    CHECK(isTrue(view, "localStorage.getItem('gone') === null"), "and leaves nothing for the next context");
    evaluate(view, idbGet);
    CHECK(waitFor(view, "window.idbDone !== null") && !strcmp(evaluate(view, "idbDone"), "\"missing\""), "in IndexedDB either: %s", evaluate(view, "idbDone"));
    zg_view_destroy(view);
    closeContext(context);

    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
