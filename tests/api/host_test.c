/* What a page shares with its host, through the C API (0.3):
 *   - the clipboard: copy and cut reach the host's clipboard with text and
 *     HTML, paste reads it, a copy event's own data goes through, and
 *     navigator.clipboard writes and reads it; without a host clipboard, one in
 *     process;
 *   - <input type=file>: the file_chooser callback, choosing and cancelling;
 *   - drops from the host: files (read from the page), text, and leaving;
 *   - drags in the page: selecting text with the mouse, and an element with
 *     draggable dropped on another (dragstart to dragend, with its data).
 *
 * Usage: host_test
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
static zg_view *view;

static const char *evaluate(const char *script)
{
    static char text[1024];
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

static int isTrue(const char *script)
{
    return !strcmp(evaluate(script), "true");
}

static int waitFor(const char *expression)
{
    double end = zg_runtime_now_ms() + 5000;
    while (zg_runtime_now_ms() < end) {
        if (isTrue(expression))
            return 1;
        zg_runtime_update(runtime);
    }
    return 0;
}

static void click(float x, float y)
{
    zg_view_mouse_event(view, ZG_MOUSE_DOWN, x, y, ZG_MOUSE_LEFT, 1, 0);
    zg_view_mouse_event(view, ZG_MOUSE_UP, x, y, ZG_MOUSE_LEFT, 1, 0);
    zg_runtime_update(runtime);
}

static void control(const char *key, const char *code)
{
    zg_view_key_event(view, ZG_KEY_DOWN, key, code, NULL, ZG_MOD_CTRL, 0);
    zg_view_key_event(view, ZG_KEY_UP, key, code, NULL, ZG_MOD_CTRL, 0);
    zg_runtime_update(runtime);
}

/* ---- The host's clipboard ---- */

#define MAX_ITEMS 8
static char clipTypes[MAX_ITEMS][64];
static char clipData[MAX_ITEMS][1024];
static size_t clipLengths[MAX_ITEMS];
static size_t clipCount;
static int clipWrites;

static void clipboardWrite(size_t count, const char *const *types, const void *const *data, const size_t *lens, void *user)
{
    size_t i;
    (void)user;
    ++clipWrites;
    clipCount = 0;
    for (i = 0; i < count && i < MAX_ITEMS; ++i) {
        snprintf(clipTypes[i], sizeof clipTypes[i], "%s", types[i]);
        clipLengths[i] = lens[i] < sizeof clipData[i] - 1 ? lens[i] : sizeof clipData[i] - 1;
        memcpy(clipData[i], data[i], clipLengths[i]);
        clipData[i][clipLengths[i]] = 0;
        ++clipCount;
    }
}

static int clipIndex(const char *type)
{
    size_t i;
    for (i = 0; i < clipCount; ++i) {
        if (!strcmp(clipTypes[i], type))
            return (int)i;
    }
    return -1;
}

static int clipboardHas(const char *type, void *user)
{
    (void)user;
    return clipIndex(type) >= 0;
}

static void clipboardRead(const char *type, zg_clipboard_reply *reply, void *user)
{
    int index = clipIndex(type);
    (void)user;
    if (index >= 0)
        zg_clipboard_reply_data(reply, clipData[index], clipLengths[index]);
}

static void setClipboard(const char *type, const char *data, const char *type2, const char *data2)
{
    const char *types[2] = { type, type2 };
    const void *items[2] = { data, data2 };
    size_t lengths[2] = { strlen(data), data2 ? strlen(data2) : 0 };
    clipboardWrite(type2 ? 2 : 1, types, items, lengths, NULL);
}

static const char *clip(const char *type)
{
    int index = clipIndex(type);
    return index >= 0 ? clipData[index] : "(none)";
}

/* ---- File chooser ---- */

static zg_file_chooser *chooser;
static int choosers;

static void onFileChooser(zg_view *v, zg_file_chooser *c, void *user)
{
    (void)v;
    (void)user;
    chooser = c;
    ++choosers;
}

static const char page[] =
    "<!DOCTYPE html><title>host</title><style>"
    "body { margin: 0; font: 16px sans-serif; }"
    "#edit, #paste, #file, #drop, #text, #drag, #button { position: absolute; left: 0; margin: 0; }"
    "#edit { top: 0; width: 300px; height: 30px; } #paste { top: 40px; width: 300px; height: 30px; }"
    "#file { top: 80px; width: 200px; height: 30px; } #drop { top: 120px; width: 200px; height: 100px; background: #eee; }"
    "#text { top: 230px; white-space: nowrap; } #drag { top: 120px; left: 300px; width: 80px; height: 40px; background: #ccf; }"
    "#button { top: 0; left: 400px; width: 100px; height: 30px; }"
    "</style>"
    "<div id=edit contenteditable>hello world</div>"
    "<div id=paste contenteditable></div>"
    "<input id=file type=file multiple accept='.txt,image/png'>"
    "<div id=drop>drop here</div>"
    "<p id=text>selectable text for the mouse</p>"
    "<div id=drag draggable=true>drag me</div>"
    "<button id=button>clipboard</button>"
    "<script>"
    "window.log = []; window.mode = '';"
    "const drop = document.getElementById('drop');"
    "for (const type of ['dragenter', 'dragover']) drop.addEventListener(type, e => { e.preventDefault(); log.push(type); });"
    "drop.addEventListener('dragleave', () => log.push('dragleave'));"
    "drop.addEventListener('drop', async e => { e.preventDefault(); log.push('drop'); const files = e.dataTransfer.files;"
    "  window.dropped = files.length ? files[0].name + ':' + await files[0].text() : 'data:' + e.dataTransfer.getData('text/plain'); });"
    "const drag = document.getElementById('drag');"
    "drag.addEventListener('dragstart', e => { log.push('dragstart'); e.dataTransfer.setData('text/plain', 'from page'); });"
    "drag.addEventListener('dragend', () => log.push('dragend'));"
    "document.addEventListener('copy', e => { if (mode !== 'custom') return; e.clipboardData.setData('text/plain', 'custom');"
    "  e.clipboardData.setData('application/x-mine', 'mine'); e.preventDefault(); });"
    "document.getElementById('button').onclick = () => {"
    "  if (mode === 'write') navigator.clipboard.writeText('async text').then(() => window.written = true, e => window.written = String(e));"
    "  if (mode === 'read') navigator.clipboard.readText().then(t => window.readBack = t, e => window.readBack = 'error: ' + e); };"
    "function select(id) { const node = document.getElementById(id); node.focus(); const range = document.createRange();"
    "  range.selectNodeContents(node); getSelection().removeAllRanges(); getSelection().addRange(range); }"
    "</script>";

int main(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_view_callbacks callbacks = { sizeof callbacks };
    zg_clipboard clipboard = { sizeof clipboard };
    zg_drag_data dragData = { sizeof dragData };
    zg_context *context;
    char filePath[512], script[1024];
    const char *base;
    const char *files[1];
    FILE *file;

    setvbuf(stdout, NULL, _IONBF, 0);
    base = getenv("TEMP") ? getenv("TEMP") : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(filePath, sizeof filePath, "%s/zerog-host-test.txt", base);
    file = fopen(filePath, "wb");
    fputs("hello file", file);
    fclose(file);

    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);
    zg_context_add_file(context, "app", "/index.html", page, sizeof page - 1, "text/html", NULL, NULL);
    viewConfig.width = 600;
    viewConfig.height = 400;
    view = zg_view_create(context, &viewConfig);
    callbacks.file_chooser = onFileChooser;
    zg_view_set_callbacks(view, &callbacks);
    zg_view_set_focus(view, 1);
    zg_view_load_url(view, "app://localhost/");
    CHECK(waitFor("document.readyState === 'complete' && typeof select === 'function'"), "the page loads");

    /* ---- Clipboard: the host's ---- */
    clipboard.write = clipboardWrite;
    clipboard.has = clipboardHas;
    clipboard.read = clipboardRead;
    CHECK(zg_runtime_set_clipboard(runtime, &clipboard) == ZG_OK, "the host's clipboard");
    evaluate("select('edit'); 0");
    control("c", "KeyC");
    CHECK(!strcmp(clip("text/plain"), "hello world"), "Ctrl+C writes the text: %s", clip("text/plain"));
    CHECK(strstr(clip("text/html"), "hello world") != NULL, "and HTML: %.80s", clip("text/html"));

    setClipboard("text/html", "<b>pasted</b>", "text/plain", "pasted");
    evaluate("select('paste'); 0");
    control("v", "KeyV");
    CHECK(isTrue("document.getElementById('paste').innerHTML.includes('<b>pasted</b>')"), "Ctrl+V pastes the host's HTML: %s", evaluate("document.getElementById('paste').innerHTML"));

    evaluate("mode = 'custom'; select('edit'); 0");
    control("c", "KeyC");
    CHECK(!strcmp(clip("text/plain"), "custom"), "a copy event's own text: %s", clip("text/plain"));
    CHECK(clipIndex("org.zerog.custom-pasteboard-data") >= 0, "and its custom data, under the ZeroG type");
    evaluate("mode = ''; select('edit'); 0");
    control("x", "KeyX");
    CHECK(!strcmp(clip("text/plain"), "hello world") && isTrue("document.getElementById('edit').textContent === ''"), "Ctrl+X cuts");

    evaluate("mode = 'write'; 0");
    click(450, 15);
    CHECK(waitFor("window.written !== undefined") && isTrue("window.written === true") && !strcmp(clip("text/plain"), "async text"),
        "navigator.clipboard.writeText: %s, %s", evaluate("String(window.written)"), clip("text/plain"));
    setClipboard("text/plain", "from host", NULL, NULL);
    evaluate("mode = 'read'; 0");
    click(450, 15);
    CHECK(waitFor("window.readBack !== undefined") && isTrue("window.readBack === 'from host'"), "navigator.clipboard.readText: %s", evaluate("window.readBack"));

    /* Without the host's: the clipboard in process. */
    CHECK(zg_runtime_set_clipboard(runtime, NULL) == ZG_OK, "back to the default clipboard");
    clipWrites = 0;
    evaluate("mode = ''; document.getElementById('edit').textContent = 'in process'; select('edit'); 0");
    control("c", "KeyC");
    evaluate("document.getElementById('paste').textContent = ''; select('paste'); 0");
    control("v", "KeyV");
    CHECK(isTrue("document.getElementById('paste').textContent === 'in process'") && !clipWrites, "copy and paste in process: %s",
        evaluate("document.getElementById('paste').textContent"));

    /* ---- File chooser ---- */
    click(50, 95);
    CHECK(waitFor("true") && choosers == 1 && chooser, "clicking a file input calls file_chooser");
    if (chooser) {
        size_t i;
        int text = 0, png = 0;
        for (i = 0; i < zg_file_chooser_accept_count(chooser); ++i) {
            text |= !strcmp(zg_file_chooser_accept(chooser, i), ".txt");
            png |= !strcmp(zg_file_chooser_accept(chooser, i), "image/png");
        }
        CHECK(zg_file_chooser_allows_multiple(chooser) && !zg_file_chooser_allows_directories(chooser) && text && png, "with multiple and the accept list");
        files[0] = filePath;
        zg_file_chooser_choose(chooser, 1, files);
        chooser = NULL;
        CHECK(waitFor("document.getElementById('file').files.length === 1"), "a chosen file reaches the input");
        evaluate("document.getElementById('file').files[0].text().then(t => window.chosen = document.getElementById('file').files[0].name + ':' + t); 0");
        CHECK(waitFor("window.chosen !== undefined") && isTrue("window.chosen === 'zerog-host-test.txt:hello file'"), "and can be read: %s", evaluate("window.chosen"));
    }
    click(50, 95);
    if (chooser) {
        zg_file_chooser_cancel(chooser);
        chooser = NULL;
    }
    zg_runtime_update(runtime);
    CHECK(choosers == 2 && isTrue("document.getElementById('file').files.length === 1"), "cancelling keeps the choice");

    /* ---- Drops from the host ---- */
    files[0] = filePath;
    dragData.files = files;
    dragData.file_count = 1;
    CHECK(zg_view_drag(view, ZG_DRAG_ENTER, 50, 170, &dragData), "a file dragged over a drop zone is accepted");
    zg_view_drag(view, ZG_DRAG_OVER, 60, 175, NULL);
    CHECK(zg_view_drag(view, ZG_DRAG_DROP, 60, 175, NULL), "and dropped");
    CHECK(waitFor("window.dropped !== undefined") && isTrue("window.dropped === 'zerog-host-test.txt:hello file'"), "the page reads it: %s", evaluate("window.dropped"));
    CHECK(isTrue("log.join() === 'dragenter,dragover,dragover,drop'"), "with the events in order: %s", evaluate("log.join()"));

    evaluate("log = []; window.dropped = undefined; 0");
    memset(&dragData, 0, sizeof dragData);
    dragData.size = sizeof dragData;
    dragData.text = "dragged text";
    zg_view_drag(view, ZG_DRAG_ENTER, 50, 170, &dragData);
    CHECK(zg_view_drag(view, ZG_DRAG_DROP, 50, 170, NULL) && waitFor("window.dropped !== undefined") && isTrue("window.dropped === 'data:dragged text'"),
        "dropped text: %s", evaluate("String(window.dropped)"));
    evaluate("log = []; 0");
    zg_view_drag(view, ZG_DRAG_ENTER, 50, 170, &dragData);
    zg_view_drag(view, ZG_DRAG_LEAVE, 50, 170, NULL);
    CHECK(isTrue("log.includes('dragleave')"), "leaving: %s", evaluate("log.join()"));
    CHECK(!zg_view_drag(view, ZG_DRAG_ENTER, 500, 300, &dragData), "outside a drop zone nothing accepts it");
    zg_view_drag(view, ZG_DRAG_LEAVE, 500, 300, NULL);

    /* ---- Drags in the page ---- */
    evaluate("getSelection().removeAllRanges(); 0");
    zg_view_mouse_event(view, ZG_MOUSE_DOWN, 2, 245, ZG_MOUSE_LEFT, 1, 0);
    zg_view_mouse_event(view, ZG_MOUSE_MOVE, 60, 245, ZG_MOUSE_LEFT, 0, 0);
    zg_view_mouse_event(view, ZG_MOUSE_MOVE, 150, 245, ZG_MOUSE_LEFT, 0, 0);
    zg_view_mouse_event(view, ZG_MOUSE_UP, 150, 245, ZG_MOUSE_LEFT, 1, 0);
    zg_runtime_update(runtime);
    CHECK(isTrue("getSelection().toString().length > 5"), "dragging the mouse selects text: '%s'", evaluate("getSelection().toString()"));

    evaluate("log = []; window.dropped = undefined; getSelection().removeAllRanges(); 0");
    zg_view_mouse_event(view, ZG_MOUSE_DOWN, 320, 130, ZG_MOUSE_LEFT, 1, 0);
    zg_view_mouse_event(view, ZG_MOUSE_MOVE, 330, 140, ZG_MOUSE_LEFT, 0, 0);
    zg_view_mouse_event(view, ZG_MOUSE_MOVE, 100, 160, ZG_MOUSE_LEFT, 0, 0);
    zg_view_mouse_event(view, ZG_MOUSE_MOVE, 60, 170, ZG_MOUSE_LEFT, 0, 0);
    zg_view_mouse_event(view, ZG_MOUSE_UP, 60, 170, ZG_MOUSE_LEFT, 1, 0);
    snprintf(script, sizeof script, "window.dropped !== undefined && log.includes('dragend')");
    CHECK(waitFor(script) && isTrue("window.dropped === 'data:from page'"), "an element dragged onto a drop zone: %s", evaluate("String(window.dropped)"));
    CHECK(isTrue("/^dragstart,dragenter,(dragover,)+drop,dragend$/.test(log.join())"), "dragstart to dragend: %s", evaluate("log.join()"));

    remove(filePath);
    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
