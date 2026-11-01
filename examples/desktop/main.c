/* A desktop application on ZeroG, every window feature of API 0.5 to try by
 * hand: window control and events, screens, dialogs, native menus (menu bar,
 * accelerators, the page's context menu as a native popup), the clipboard
 * from native code, light and dark, the icon, files dropped, more windows
 * (frameless, transparent) and a close confirmation.
 *
 * The page (web/) calls Desktop.* bindings and shows every window event in
 * its log; the native side answers through events (zg_view_emit). Packed
 * into the binary by tools/assets.py (desktop_assets.c).
 *
 * Usage: desktop [--smoke]   (--smoke: quit once the page has loaded)
 */

#include <zerog.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char desktop_assets[];
extern const size_t desktop_assets_len;

static zg_runtime *rt;
static zg_context *ctx;
static zg_window *win;
static zg_view *view;
static int smoke;
static int confirmClose = 1;
static int keepDrops;
static int fullscreen;
static int onTop;
static zg_context_menu *pendingContextMenu;

enum {
    MENU_NEW_WINDOW = 1, MENU_OPEN, MENU_SAVE, MENU_QUIT,
    MENU_SYSTEM, MENU_LIGHT, MENU_DARK, MENU_ON_TOP, MENU_FULLSCREEN,
    MENU_ABOUT,
    POPUP_HELLO = 500, POPUP_CENTER, POPUP_MAXIMIZE,
    CONTEXT_FIRST = 1000, /* the page's context menu items: 1000 + their id */
    CONTEXT_HELLO = 3000
};

/* ---- Events to the page ---- */

/* `s` as a JSON string into `out`. */
static void jsonString(char *out, size_t size, const char *s)
{
    size_t n = 0;
    if (size < 3)
        return;
    out[n++] = '"';
    for (; s && *s && n + 7 < size; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20)
            n += snprintf(out + n, size - n, "\\u%04x", c);
        else
            out[n++] = (char)c;
    }
    out[n++] = '"';
    out[n] = '\0';
}

/* window.dispatchEvent(new CustomEvent(name, { detail })) in the page. */
static void emit(const char *name, const char *format, ...)
{
    char json[4096];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(json, sizeof json, format, arguments);
    va_end(arguments);
    if (view)
        zg_view_emit(view, name, json, strlen(json));
}

static const char *stateName(zg_window_state state)
{
    static const char *names[] = { "normal", "minimized", "maximized", "fullscreen", "hidden" };
    return (unsigned)state < 5 ? names[state] : "?";
}

/* ---- Window events ---- */

static void onMessageAnswer(zg_window *window, zg_result result, int button, void *user);

static int onCloseRequested(zg_window *window, void *user)
{
    zg_message_button buttons[] = { { 1, "Quit", 0 }, { 2, "Keep working", ZG_MESSAGE_BUTTON_DEFAULT | ZG_MESSAGE_BUTTON_CANCEL } };
    zg_message_box box = { sizeof box };
    (void)user;
    emit("native", "{\"type\":\"close_requested\",\"confirm\":%s}", confirmClose ? "true" : "false");
    if (!confirmClose)
        return 1;
    box.kind = ZG_MESSAGE_WARNING;
    box.title = "Quit the demo?";
    box.message = "The window asked to close (close_requested returned 0 to keep it open).\nQuit now?";
    box.buttons = buttons;
    box.button_count = 2;
    zg_window_message_box(window, &box, onMessageAnswer, "close");
    return 0;
}

static void onResized(zg_window *window, unsigned width, unsigned height, void *user)
{
    (void)window;
    (void)user;
    emit("native", "{\"type\":\"resized\",\"width\":%u,\"height\":%u}", width, height);
}

static void onMoved(zg_window *window, int x, int y, void *user)
{
    (void)window;
    (void)user;
    emit("native", "{\"type\":\"moved\",\"x\":%d,\"y\":%d}", x, y);
}

static void onFocus(zg_window *window, int focused, void *user)
{
    (void)window;
    (void)user;
    emit("native", "{\"type\":\"focus_changed\",\"focused\":%s}", focused ? "true" : "false");
}

static void onState(zg_window *window, zg_window_state state, void *user)
{
    (void)window;
    (void)user;
    if (state != ZG_WINDOW_FULLSCREEN)
        fullscreen = 0;
    zg_window_menu_set_checked(win, MENU_FULLSCREEN, state == ZG_WINDOW_FULLSCREEN);
    emit("native", "{\"type\":\"state_changed\",\"state\":\"%s\"}", stateName(state));
}

static void onScreen(zg_window *window, void *user)
{
    size_t index = 0;
    (void)user;
    zg_window_screen(window, &index);
    emit("native", "{\"type\":\"screen_changed\",\"screen\":%u}", (unsigned)index);
}

static int onFilesDropped(zg_window *window, const char *const *paths, size_t count, float x, float y, void *user)
{
    char json[4096], path[1024];
    size_t i, n;
    (void)window;
    (void)user;
    n = (size_t)snprintf(json, sizeof json, "{\"type\":\"files_dropped\",\"x\":%.0f,\"y\":%.0f,\"kept\":%s,\"paths\":[", x, y, keepDrops ? "true" : "false");
    for (i = 0; i < count && n < sizeof json - 64; ++i) {
        jsonString(path, sizeof path, paths[i]);
        n += (size_t)snprintf(json + n, sizeof json - n, "%s%s", i ? "," : "", path);
    }
    snprintf(json + n, sizeof json - n, "]}");
    zg_view_emit(view, "native", json, strlen(json));
    return keepDrops;
}

static void setColorScheme(zg_color_scheme scheme)
{
    zg_view_set_color_scheme(view, scheme);
    zg_window_menu_set_checked(win, MENU_SYSTEM, scheme == ZG_COLOR_SCHEME_SYSTEM);
    zg_window_menu_set_checked(win, MENU_LIGHT, scheme == ZG_COLOR_SCHEME_LIGHT);
    zg_window_menu_set_checked(win, MENU_DARK, scheme == ZG_COLOR_SCHEME_DARK);
    emit("native", "{\"type\":\"color_scheme\",\"scheme\":%d}", (int)scheme);
}

static void showFileDialog(zg_file_dialog_kind kind, int multiple);
static void openWindow(const char *kind);

static void onMenuItem(zg_window *window, unsigned id, void *user)
{
    (void)window;
    (void)user;
    /* The page's context menu, shown as a native popup: answered either way. */
    if (pendingContextMenu) {
        zg_context_menu *menu = pendingContextMenu;
        pendingContextMenu = NULL;
        if (id > CONTEXT_FIRST && id < CONTEXT_HELLO)
            zg_context_menu_select(menu, id - CONTEXT_FIRST);
        else
            zg_context_menu_cancel(menu);
        emit("native", "{\"type\":\"context_menu\",\"chosen\":%u}", id);
        if (id == CONTEXT_HELLO)
            emit("native", "{\"type\":\"hello\",\"from\":\"the context menu\"}");
        return;
    }
    emit("native", "{\"type\":\"menu_item\",\"id\":%u}", id);
    switch (id) {
    case MENU_NEW_WINDOW: openWindow("normal"); break;
    case MENU_OPEN: showFileDialog(ZG_FILE_DIALOG_OPEN, 1); break;
    case MENU_SAVE: showFileDialog(ZG_FILE_DIALOG_SAVE, 0); break;
    case MENU_QUIT:
        /* As the close button: confirmed unless that is turned off. */
        if (onCloseRequested(win, NULL))
            zg_runtime_quit(rt);
        break;
    case MENU_SYSTEM: setColorScheme(ZG_COLOR_SCHEME_SYSTEM); break;
    case MENU_LIGHT: setColorScheme(ZG_COLOR_SCHEME_LIGHT); break;
    case MENU_DARK: setColorScheme(ZG_COLOR_SCHEME_DARK); break;
    case MENU_ON_TOP:
        onTop = !onTop;
        zg_window_set_always_on_top(win, onTop);
        zg_window_menu_set_checked(win, MENU_ON_TOP, onTop);
        break;
    case MENU_FULLSCREEN:
        fullscreen = !fullscreen;
        zg_window_set_fullscreen(win, fullscreen);
        break;
    case MENU_ABOUT: {
        zg_message_box box = { sizeof box };
        zg_version_info version = zg_version();
        char message[256];
        snprintf(message, sizeof message, "ZeroG %d.%d.%d, profile %s, WebKit %s.", version.major, version.minor, version.patch, version.profile, version.webkit_revision);
        box.title = "About the ZeroG desktop demo";
        box.message = message;
        zg_window_message_box(win, &box, onMessageAnswer, "about");
        break;
    }
    case POPUP_HELLO: emit("native", "{\"type\":\"hello\",\"from\":\"the popup menu\"}"); break;
    case POPUP_CENTER: zg_window_center(win); break;
    case POPUP_MAXIMIZE: zg_window_maximize(win); break;
    default: break;
    }
}

/* ---- The page's context menu, as a native popup ---- */

/* The items whose parent is `parent`, with their submenus. */
static zg_menu *contextMenuItems(const zg_context_menu *menu, unsigned parent)
{
    zg_menu *result = zg_menu_create();
    size_t i, count = zg_context_menu_item_count(menu);
    for (i = 0; i < count; ++i) {
        const zg_context_menu_item *item = zg_context_menu_item_at(menu, i);
        if (item->parent != parent)
            continue;
        switch (item->kind) {
        case ZG_CONTEXT_MENU_ITEM_SEPARATOR:
            zg_menu_add_separator(result);
            break;
        case ZG_CONTEXT_MENU_ITEM_SUBMENU: {
            zg_menu *submenu = contextMenuItems(menu, item->id);
            zg_menu_add_submenu(result, item->label, submenu);
            zg_menu_destroy(submenu);
            break;
        }
        default: {
            unsigned flags = item->enabled ? 0 : ZG_MENU_ITEM_DISABLED;
            if (item->kind == ZG_CONTEXT_MENU_ITEM_CHECKBOX)
                flags |= ZG_MENU_ITEM_CHECKBOX | (item->checked ? ZG_MENU_ITEM_CHECKED : 0);
            zg_menu_add_item(result, CONTEXT_FIRST + item->id, item->label, NULL, flags);
            break;
        }
        }
    }
    return result;
}

static void onContextMenu(zg_view *v, zg_context_menu *menu, void *user)
{
    zg_menu *popup;
    float x, y;
    (void)v;
    (void)user;
    zg_context_menu_position(menu, &x, &y);
    popup = contextMenuItems(menu, 0);
    if (zg_context_menu_item_count(menu))
        zg_menu_add_separator(popup);
    zg_menu_add_item(popup, CONTEXT_HELLO, "Say hello (an item of the application's)", NULL, 0);
    pendingContextMenu = menu;
    if (zg_window_popup_menu(win, popup, x, y) != ZG_OK) {
        pendingContextMenu = NULL;
        zg_context_menu_cancel(menu);
    }
    zg_menu_destroy(popup);
}

/* ---- Dialogs ---- */

static void onMessageAnswer(zg_window *window, zg_result result, int button, void *user)
{
    const char *what = user;
    emit("native", "{\"type\":\"message_box\",\"what\":\"%s\",\"result\":%d,\"button\":%d}", what, (int)result, button);
    if (window && !strcmp(what, "close") && button == 1) {
        /* From a callback: the window goes once the loop is out of its work. */
        zg_window_destroy(window);
        win = NULL;
        view = NULL;
        zg_runtime_quit(rt);
    }
}

static void onFileDialog(zg_window *window, zg_result result, const char *const *paths, size_t count, void *user)
{
    char json[4096], path[1024];
    size_t i, n;
    (void)window;
    n = (size_t)snprintf(json, sizeof json, "{\"type\":\"file_dialog\",\"kind\":\"%s\",\"result\":%d,\"paths\":[", (const char *)user, (int)result);
    for (i = 0; i < count && n < sizeof json - 64; ++i) {
        jsonString(path, sizeof path, paths[i]);
        n += (size_t)snprintf(json + n, sizeof json - n, "%s%s", i ? "," : "", path);
    }
    snprintf(json + n, sizeof json - n, "]}");
    if (view)
        zg_view_emit(view, "native", json, strlen(json));
}

static void showFileDialog(zg_file_dialog_kind kind, int multiple)
{
    static const zg_file_filter filters[] = {
        { "Text and Markdown", "txt;md" }, { "Images", "png;jpg;jpeg;webp;svg" }, { "All files", "*" }
    };
    static const char *kinds[] = { "open", "folder", "save" };
    zg_file_dialog dialog = { sizeof dialog };
    dialog.kind = kind;
    dialog.multiple = multiple;
    if (kind == ZG_FILE_DIALOG_OPEN) {
        dialog.title = "Open files (zg_window_file_dialog)";
        dialog.filters = filters;
        dialog.filter_count = 3;
    } else if (kind == ZG_FILE_DIALOG_OPEN_FOLDER) {
        dialog.title = "Choose a folder";
        dialog.accept_label = "Use this folder";
    } else {
        dialog.title = "Save a copy";
        dialog.location = "notes.txt";
        dialog.filters = filters;
        dialog.filter_count = 1;
    }
    if (zg_window_file_dialog(win, &dialog, onFileDialog, (void *)kinds[kind]) != ZG_OK)
        emit("native", "{\"type\":\"error\",\"message\":\"file dialog refused\"}");
}

/* ---- More windows ---- */

static zg_value *closeSecond(zg_call *call, void *user)
{
    (void)call;
    /* From the window's own page: destroying it here is allowed. */
    zg_window_destroy((zg_window *)user);
    emit("native", "{\"type\":\"window_closed\",\"from\":\"its own page\"}");
    return NULL;
}

static void openWindow(const char *kind)
{
    zg_window_config config = { sizeof config };
    zg_window *second;
    int frameless = !strcmp(kind, "frameless") || !strcmp(kind, "transparent");
    config.title = "Another window";
    config.width = 420;
    config.height = 280;
    config.frameless = frameless;
    config.transparent = !strcmp(kind, "transparent");
    second = zg_window_create(ctx, &config);
    if (!second) {
        emit("native", "{\"type\":\"error\",\"message\":\"cannot create the window\"}");
        return;
    }
    zg_view_bind(zg_window_view(second), "Second.close", closeSecond, second);
    {
        char url[128];
        snprintf(url, sizeof url, "app://localhost/second.html?kind=%s", kind);
        zg_view_load_url(zg_window_view(second), url);
    }
    emit("native", "{\"type\":\"window_opened\",\"kind\":\"%s\"}", kind);
}

/* ---- Bindings: Desktop.* ---- */

#define ARG_NUMBER(i) ((int)zg_call_arg_number(call, (i)))
#define ARG_BOOL(i) zg_call_arg_bool(call, (i))

static zg_value *bindMinimize(zg_call *call, void *user) { (void)user; zg_window_minimize(win); return zg_value_null(call); }
static zg_value *bindMaximize(zg_call *call, void *user) { (void)user; zg_window_maximize(win); return zg_value_null(call); }
static zg_value *bindRestore(zg_call *call, void *user) { (void)user; zg_window_restore(win); return zg_value_null(call); }
static zg_value *bindCenter(zg_call *call, void *user) { (void)user; return zg_value_bool(call, zg_window_center(win) == ZG_OK); }
static zg_value *bindFocus(zg_call *call, void *user) { (void)user; zg_window_focus(win); return zg_value_null(call); }
static zg_value *bindShow(zg_call *call, void *user) { (void)user; zg_window_show(win); return zg_value_null(call); }
static zg_value *bindHide(zg_call *call, void *user) { (void)user; zg_window_hide(win); return zg_value_null(call); }
static zg_value *bindSetSize(zg_call *call, void *user) { (void)user; return zg_value_bool(call, zg_window_set_size(win, ARG_NUMBER(0), ARG_NUMBER(1)) == ZG_OK); }
static zg_value *bindSetMinSize(zg_call *call, void *user) { (void)user; zg_window_set_min_size(win, ARG_NUMBER(0), ARG_NUMBER(1)); return zg_value_null(call); }
static zg_value *bindSetMaxSize(zg_call *call, void *user) { (void)user; zg_window_set_max_size(win, ARG_NUMBER(0), ARG_NUMBER(1)); return zg_value_null(call); }
static zg_value *bindSetPosition(zg_call *call, void *user) { (void)user; return zg_value_bool(call, zg_window_set_position(win, ARG_NUMBER(0), ARG_NUMBER(1)) == ZG_OK); }
static zg_value *bindResizable(zg_call *call, void *user) { (void)user; zg_window_set_resizable(win, ARG_BOOL(0)); return zg_value_null(call); }
static zg_value *bindFullscreen(zg_call *call, void *user) { (void)user; fullscreen = ARG_BOOL(0); zg_window_set_fullscreen(win, fullscreen); return zg_value_null(call); }

static zg_value *bindAlwaysOnTop(zg_call *call, void *user)
{
    (void)user;
    onTop = ARG_BOOL(0);
    zg_window_set_always_on_top(win, onTop);
    zg_window_menu_set_checked(win, MENU_ON_TOP, onTop);
    return zg_value_null(call);
}

static zg_value *bindTitle(zg_call *call, void *user)
{
    size_t length;
    const char *title = zg_call_arg_string(call, 0, &length);
    char copy[256];
    (void)user;
    snprintf(copy, sizeof copy, "%.*s", (int)length, title ? title : "");
    zg_window_set_title(win, copy);
    return zg_value_null(call);
}

static zg_value *bindInfo(zg_call *call, void *user)
{
    char json[256];
    unsigned width = 0, height = 0;
    int x = 0, y = 0, placed;
    size_t screen = 0;
    (void)user;
    zg_window_get_size(win, &width, &height);
    placed = zg_window_get_position(win, &x, &y) == ZG_OK;
    zg_window_screen(win, &screen);
    if (placed)
        snprintf(json, sizeof json, "{\"state\":\"%s\",\"width\":%u,\"height\":%u,\"x\":%d,\"y\":%d,\"screen\":%u}",
            stateName(zg_window_get_state(win)), width, height, x, y, (unsigned)screen);
    else
        snprintf(json, sizeof json, "{\"state\":\"%s\",\"width\":%u,\"height\":%u,\"x\":null,\"y\":null,\"screen\":%u}",
            stateName(zg_window_get_state(win)), width, height, (unsigned)screen);
    return zg_value_json(call, json, strlen(json));
}

static zg_value *bindScreens(zg_call *call, void *user)
{
    char json[4096], name[300];
    size_t i, n, count = zg_screen_count(rt);
    (void)user;
    n = (size_t)snprintf(json, sizeof json, "[");
    for (i = 0; i < count && n < sizeof json - 400; ++i) {
        zg_screen screen = { sizeof screen };
        zg_screen_get(rt, i, &screen);
        jsonString(name, sizeof name, screen.name);
        n += (size_t)snprintf(json + n, sizeof json - n,
            "%s{\"name\":%s,\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u,\"workX\":%d,\"workY\":%d,\"workWidth\":%u,\"workHeight\":%u,"
            "\"scale\":%.2f,\"refresh\":%.1f,\"primary\":%s}",
            i ? "," : "", name, screen.x, screen.y, screen.width, screen.height, screen.work_x, screen.work_y,
            screen.work_width, screen.work_height, screen.scale, screen.refresh_rate, screen.primary ? "true" : "false");
    }
    snprintf(json + n, sizeof json - n, "]");
    return zg_value_json(call, json, strlen(json));
}

static zg_value *bindOpenFile(zg_call *call, void *user) { (void)user; showFileDialog(ZG_FILE_DIALOG_OPEN, ARG_BOOL(0)); return zg_value_null(call); }
static zg_value *bindOpenFolder(zg_call *call, void *user) { (void)user; showFileDialog(ZG_FILE_DIALOG_OPEN_FOLDER, 0); return zg_value_null(call); }
static zg_value *bindSaveFile(zg_call *call, void *user) { (void)user; showFileDialog(ZG_FILE_DIALOG_SAVE, 0); return zg_value_null(call); }

static zg_value *bindMessage(zg_call *call, void *user)
{
    static const zg_message_button buttons[] = {
        { 1, "Save", ZG_MESSAGE_BUTTON_DEFAULT }, { 2, "Don't save", 0 }, { 3, "Cancel", ZG_MESSAGE_BUTTON_CANCEL }
    };
    zg_message_box box = { sizeof box };
    int kind = ARG_NUMBER(0);
    (void)user;
    box.kind = (zg_message_kind)(kind < 0 || kind > 2 ? 0 : kind);
    box.title = kind == 2 ? "Something went wrong" : kind == 1 ? "Unsaved changes" : "Information";
    box.message = kind == 1 ? "Save the document before closing? (three buttons; Escape or closing the box is Cancel)"
        : kind == 2 ? "An error message box, with the system's error icon." : "An information message box, with one OK button.";
    if (kind == 1) {
        box.buttons = buttons;
        box.button_count = 3;
    }
    zg_window_message_box(win, &box, onMessageAnswer, kind == 2 ? "error" : kind == 1 ? "question" : "info");
    return zg_value_null(call);
}

static zg_value *bindClipboardWrite(zg_call *call, void *user)
{
    size_t textLength = 0, htmlLength = 0;
    const char *text = zg_call_arg_string(call, 0, &textLength);
    const char *html = zg_call_arg_string(call, 1, &htmlLength);
    const char *types[2] = { "text/plain", "text/html" };
    const void *data[2];
    size_t lengths[2];
    (void)user;
    data[0] = text;
    data[1] = html;
    lengths[0] = textLength;
    lengths[1] = htmlLength;
    return zg_value_bool(call, zg_clipboard_write(rt, htmlLength ? 2 : 1, types, data, lengths) == ZG_OK);
}

static zg_value *bindClipboardRead(zg_call *call, void *user)
{
    char *text = NULL, *html = NULL;
    size_t length;
    char json[8192], textJSON[3000], htmlJSON[3000];
    (void)user;
    zg_clipboard_read(rt, "text/plain", &text, &length);
    zg_clipboard_read(rt, "text/html", &html, &length);
    jsonString(textJSON, sizeof textJSON, text ? text : "");
    jsonString(htmlJSON, sizeof htmlJSON, html ? html : "");
    snprintf(json, sizeof json, "{\"text\":%s,\"hasText\":%s,\"html\":%s,\"hasHTML\":%s}", textJSON, text ? "true" : "false", htmlJSON, html ? "true" : "false");
    zg_free(text);
    zg_free(html);
    return zg_value_json(call, json, strlen(json));
}

static zg_value *bindColorScheme(zg_call *call, void *user)
{
    (void)user;
    setColorScheme((zg_color_scheme)ARG_NUMBER(0));
    return zg_value_null(call);
}

static zg_value *bindSystemColorScheme(zg_call *call, void *user)
{
    (void)user;
    return zg_value_string(call, zg_system_color_scheme(rt) == ZG_COLOR_SCHEME_DARK ? "dark" : "light");
}

/* A round icon of the given color, drawn here. */
static void setIcon(unsigned rgb)
{
    static unsigned char pixels[64 * 64 * 4];
    int x, y;
    for (y = 0; y < 64; ++y) {
        for (x = 0; x < 64; ++x) {
            unsigned char *pixel = pixels + (y * 64 + x) * 4;
            int dx = x - 32, dy = y - 32, inside = dx * dx + dy * dy < 30 * 30, ring = dx * dx + dy * dy < 18 * 18;
            pixel[0] = ring ? 255 : (unsigned char)(rgb >> 16);
            pixel[1] = ring ? 255 : (unsigned char)(rgb >> 8);
            pixel[2] = ring ? 255 : (unsigned char)rgb;
            pixel[3] = inside ? 255 : 0;
            if (ring && dx * dx + dy * dy < 10 * 10) {
                pixel[0] = (unsigned char)(rgb >> 16);
                pixel[1] = (unsigned char)(rgb >> 8);
                pixel[2] = (unsigned char)rgb;
            }
        }
    }
    zg_window_set_icon(win, pixels, 64, 64);
}

static zg_value *bindSetIcon(zg_call *call, void *user)
{
    static const unsigned colors[] = { 0x29d3f4, 0xf45b29, 0x5bd329, 0xb029f4 };
    int index = ARG_NUMBER(0);
    (void)user;
    setIcon(colors[(unsigned)index % 4]);
    return zg_value_null(call);
}

static zg_value *bindSetConfirmClose(zg_call *call, void *user) { (void)user; confirmClose = ARG_BOOL(0); return zg_value_null(call); }
static zg_value *bindSetKeepDrops(zg_call *call, void *user) { (void)user; keepDrops = ARG_BOOL(0); return zg_value_null(call); }
static zg_value *bindEnableSave(zg_call *call, void *user) { (void)user; zg_window_menu_set_enabled(win, MENU_SAVE, ARG_BOOL(0)); return zg_value_null(call); }

static zg_value *bindPopup(zg_call *call, void *user)
{
    zg_menu *menu = zg_menu_create(), *more = zg_menu_create();
    (void)user;
    zg_menu_add_item(menu, POPUP_HELLO, "Say &hello", NULL, 0);
    zg_menu_add_item(menu, POPUP_CENTER, "&Center the window", NULL, 0);
    zg_menu_add_separator(menu);
    zg_menu_add_item(more, POPUP_MAXIMIZE, "&Maximize", NULL, 0);
    zg_menu_add_item(more, 599, "Disabled item", NULL, ZG_MENU_ITEM_DISABLED);
    zg_menu_add_item(more, 598, "Checked item", NULL, ZG_MENU_ITEM_CHECKBOX | ZG_MENU_ITEM_CHECKED);
    zg_menu_add_submenu(menu, "&More", more);
    zg_window_popup_menu(win, menu, (float)zg_call_arg_number(call, 0), (float)zg_call_arg_number(call, 1));
    zg_menu_destroy(more);
    zg_menu_destroy(menu);
    return zg_value_null(call);
}

static zg_value *bindOpenWindow(zg_call *call, void *user)
{
    size_t length;
    const char *kind = zg_call_arg_string(call, 0, &length);
    char copy[32];
    (void)user;
    snprintf(copy, sizeof copy, "%.*s", (int)length, kind ? kind : "normal");
    openWindow(copy);
    return zg_value_null(call);
}

static zg_value *bindQuit(zg_call *call, void *user)
{
    (void)user;
    zg_runtime_quit(rt);
    return zg_value_null(call);
}

/* ---- Setup ---- */

static zg_menu *createMenuBar(void)
{
    zg_menu *bar = zg_menu_create(), *file = zg_menu_create(), *viewMenu = zg_menu_create(), *help = zg_menu_create();
    zg_menu_add_item(file, MENU_NEW_WINDOW, "&New window", "Ctrl+N", 0);
    zg_menu_add_item(file, MENU_OPEN, "&Open...", "Ctrl+O", 0);
    zg_menu_add_item(file, MENU_SAVE, "&Save as...", "Ctrl+Shift+S", ZG_MENU_ITEM_DISABLED);
    zg_menu_add_separator(file);
    zg_menu_add_item(file, MENU_QUIT, "&Quit", "Ctrl+Q", 0);
    zg_menu_add_item(viewMenu, MENU_SYSTEM, "Follow the &system", NULL, ZG_MENU_ITEM_CHECKBOX | ZG_MENU_ITEM_CHECKED);
    zg_menu_add_item(viewMenu, MENU_LIGHT, "&Light", NULL, ZG_MENU_ITEM_CHECKBOX);
    zg_menu_add_item(viewMenu, MENU_DARK, "&Dark", NULL, ZG_MENU_ITEM_CHECKBOX);
    zg_menu_add_separator(viewMenu);
    zg_menu_add_item(viewMenu, MENU_ON_TOP, "Always on &top", "Ctrl+T", ZG_MENU_ITEM_CHECKBOX);
    zg_menu_add_item(viewMenu, MENU_FULLSCREEN, "&Full screen", "F11", ZG_MENU_ITEM_CHECKBOX);
    zg_menu_add_item(help, MENU_ABOUT, "&About", "F1", 0);
    zg_menu_add_submenu(bar, "&File", file);
    zg_menu_add_submenu(bar, "&View", viewMenu);
    zg_menu_add_submenu(bar, "&Help", help);
    zg_menu_destroy(file);
    zg_menu_destroy(viewMenu);
    zg_menu_destroy(help);
    return bar;
}

static void onLoadFinished(zg_view *v, const char *url, int failed, void *user)
{
    (void)v;
    (void)user;
    if (failed) {
        fprintf(stderr, "cannot load %s: %s\n", url, zg_last_error());
        return;
    }
    /* Created hidden: shown once configured and loaded. */
    zg_window_show(win);
    if (smoke)
        zg_runtime_quit(rt);
}

static void onConsole(zg_view *v, const zg_log_message *message, void *user)
{
    (void)v;
    (void)user;
    printf("console: %.*s\n", (int)message->message_len, message->message);
}

int main(int argc, char **argv)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_window_config windowConfig = { sizeof windowConfig };
    zg_window_callbacks windowCallbacks = { sizeof windowCallbacks };
    zg_view_callbacks viewCallbacks = { sizeof viewCallbacks };
    zg_menu *bar;
    zg_result menus;

    smoke = argc > 1 && !strcmp(argv[1], "--smoke");
    rt = zg_runtime_create(&runtimeConfig);
    if (!rt) {
        fprintf(stderr, "zg_runtime_create: %s\n", zg_last_error());
        return 1;
    }
    contextConfig.app_id = "dev.zerog.desktop-demo";
    contextConfig.ephemeral = 1;
    ctx = zg_context_create(rt, &contextConfig);
    if (zg_context_mount_assets(ctx, "app", desktop_assets, desktop_assets_len) != ZG_OK) {
        fprintf(stderr, "zg_context_mount_assets: %s\n", zg_last_error());
        return 1;
    }

    windowConfig.title = "ZeroG desktop demo";
    windowConfig.width = 1100;
    windowConfig.height = 760;
    windowConfig.hidden = 1;
    win = zg_window_create(ctx, &windowConfig);
    if (!win) {
        fprintf(stderr, "zg_window_create: %s\n", zg_last_error());
        return 1;
    }
    view = zg_window_view(win);

    windowCallbacks.close_requested = onCloseRequested;
    windowCallbacks.resized = onResized;
    windowCallbacks.moved = onMoved;
    windowCallbacks.focus_changed = onFocus;
    windowCallbacks.state_changed = onState;
    windowCallbacks.menu_item = onMenuItem;
    windowCallbacks.screen_changed = onScreen;
    windowCallbacks.files_dropped = onFilesDropped;
    zg_window_set_callbacks(win, &windowCallbacks);

    viewCallbacks.console = onConsole;
    viewCallbacks.load_finished = onLoadFinished;
    viewCallbacks.context_menu = onContextMenu;
    zg_view_set_callbacks(view, &viewCallbacks);

    bar = createMenuBar();
    menus = zg_window_set_menu(win, bar);
    zg_menu_destroy(bar);
    if (menus != ZG_OK)
        printf("no native menu bar here: %s\n", zg_last_error());
    zg_window_set_min_size(win, 640, 480);
    setIcon(0x29d3f4);
    zg_window_center(win);

#define BIND(name, fn) zg_view_bind(view, "Desktop." name, fn, NULL)
    BIND("minimize", bindMinimize);
    BIND("maximize", bindMaximize);
    BIND("restore", bindRestore);
    BIND("center", bindCenter);
    BIND("focus", bindFocus);
    BIND("show", bindShow);
    BIND("hide", bindHide);
    BIND("setSize", bindSetSize);
    BIND("setMinSize", bindSetMinSize);
    BIND("setMaxSize", bindSetMaxSize);
    BIND("setPosition", bindSetPosition);
    BIND("resizable", bindResizable);
    BIND("fullscreen", bindFullscreen);
    BIND("alwaysOnTop", bindAlwaysOnTop);
    BIND("title", bindTitle);
    BIND("info", bindInfo);
    BIND("screens", bindScreens);
    BIND("openFile", bindOpenFile);
    BIND("openFolder", bindOpenFolder);
    BIND("saveFile", bindSaveFile);
    BIND("message", bindMessage);
    BIND("clipboardWrite", bindClipboardWrite);
    BIND("clipboardRead", bindClipboardRead);
    BIND("colorScheme", bindColorScheme);
    BIND("systemColorScheme", bindSystemColorScheme);
    BIND("setIcon", bindSetIcon);
    BIND("setConfirmClose", bindSetConfirmClose);
    BIND("setKeepDrops", bindSetKeepDrops);
    BIND("enableSave", bindEnableSave);
    BIND("popup", bindPopup);
    BIND("openWindow", bindOpenWindow);
    BIND("quit", bindQuit);
#undef BIND

    zg_view_load_url(view, "app://localhost/index.html");
    zg_runtime_run(rt);

    if (win)
        zg_window_destroy(win);
    zg_runtime_destroy(rt);
    return 0;
}
