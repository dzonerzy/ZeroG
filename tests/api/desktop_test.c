/* What desktop applications need of their windows (API 0.5), from C:
 *   - screens: count, bounds, work area, scale, the primary one, the window's;
 *   - window control: logical sizes, limits, positions (not on Wayland),
 *     hidden at creation then shown, state changes, always on top;
 *   - window events: resized, moved, state, close requested and refused;
 *   - destroying windows from callbacks, the window running its own;
 *   - dialogs: arguments checked, the answer for a destroyed window, and on
 *     Windows a message box and a file dialog closed by the test;
 *   - native menus: on Windows a menu bar chosen by WM_COMMAND and by its
 *     accelerator (and not while disabled), a popup menu chosen by keyboard;
 *     elsewhere ZG_ERROR_UNSUPPORTED.
 *
 * Usage: desktop_test
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
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

static zg_runtime *runtime;

/* ---- Running the loop until something happened ----

   The window's page calls Test.tick every 10 ms; the loop ends when the
   condition holds or the time is up. */

static int (*condition)(void);
static double deadline;

static zg_value *tick(zg_call *call, void *user)
{
    (void)call;
    (void)user;
    if ((condition && condition()) || zg_runtime_now_ms() > deadline)
        zg_runtime_quit(runtime);
    return NULL;
}

static int runUntil(int (*until)(void), double milliseconds)
{
    condition = until;
    deadline = zg_runtime_now_ms() + milliseconds;
    zg_runtime_run(runtime);
    condition = NULL;
    return until ? until() : 1;
}

static const char tickingPage[] =
    "<!DOCTYPE html><body style='background: #335'><script>"
    "setInterval(() => Test.tick(), 10);"
    "</script></body>";

static zg_window *createWindow(zg_context *context, const char *title, int hidden)
{
    zg_window_config config = { sizeof config };
    zg_window *window;
    config.title = title;
    config.width = 400;
    config.height = 300;
    config.hidden = hidden;
    window = zg_window_create(context, &config);
    if (window) {
        zg_view_bind(zg_window_view(window), "Test.tick", tick, NULL);
        zg_view_load_html(zg_window_view(window), tickingPage, sizeof tickingPage - 1, NULL);
    }
    return window;
}

/* ---- Window events ---- */

static unsigned resizedWidth, resizedHeight;
static int movedX = -99999, movedY = -99999;
static zg_window_state lastState = (zg_window_state)-1;
static int stateChanges;
static unsigned chosenItem;
static int menuChoices;
static int closeRequests;
static int allowClose;

static int onCloseRequested(zg_window *window, void *user)
{
    (void)window;
    (void)user;
    ++closeRequests;
    return allowClose;
}

static void onResized(zg_window *window, unsigned width, unsigned height, void *user)
{
    (void)window;
    (void)user;
    resizedWidth = width;
    resizedHeight = height;
}

static void onMoved(zg_window *window, int x, int y, void *user)
{
    (void)window;
    (void)user;
    movedX = x;
    movedY = y;
}

static void onStateChanged(zg_window *window, zg_window_state state, void *user)
{
    (void)window;
    (void)user;
    lastState = state;
    ++stateChanges;
}

static void onMenuItem(zg_window *window, unsigned id, void *user)
{
    (void)window;
    (void)user;
    chosenItem = id;
    ++menuChoices;
}

static zg_window *mainWindow;

static int isShown(void) { return lastState == ZG_WINDOW_NORMAL; }
static int isResized(void) { return resizedWidth == 500 && resizedHeight == 350; }
static int isMoved(void) { return movedX == 120 && movedY == 140; }
static int isMaximized(void) { return lastState == ZG_WINDOW_MAXIMIZED; }
static int isMinimized(void) { return lastState == ZG_WINDOW_MINIMIZED; }
static int isNormal(void) { return lastState == ZG_WINDOW_NORMAL; }
static int isHidden(void) { return lastState == ZG_WINDOW_HIDDEN; }
static int hasMenuChoice(void) { return menuChoices > 0; }

static char droppedPath[512];
static int drops;

static int onFilesDropped(zg_window *window, const char *const *paths, size_t count, float x, float y, void *user)
{
    (void)window;
    (void)x;
    (void)y;
    (void)user;
    if (count)
        snprintf(droppedPath, sizeof droppedPath, "%s", paths[0]);
    ++drops;
    return 1;
}

static int hasDrop(void) { return drops > 0; }

/* ---- Select dropdowns ---- */

static zg_select_menu *selectMenu;

static void onSelectMenu(zg_view *view, zg_select_menu *menu, void *user)
{
    (void)view;
    (void)user;
    selectMenu = menu;
}

static int hasSelectMenu(void) { return selectMenu != NULL; }

static const char *evaluate(zg_view *view, const char *script);

static int selectChanged(void)
{
    return !strcmp(evaluate(zg_window_view(mainWindow), "window.changed || ''"), "\"a\"") || !strcmp(evaluate(zg_window_view(mainWindow), "window.changed || ''"), "\"c\"");
}

static const char *evaluate(zg_view *view, const char *script)
{
    static char text[256];
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
static int hasCloseRequest(void) { return closeRequests > 0; }

/* ---- Destroying windows from callbacks ---- */

static zg_window *selfDestroying;

static zg_value *destroySelf(zg_call *call, void *user)
{
    (void)call;
    (void)user;
    /* The window whose page runs this call, from inside its own work. */
    zg_window_destroy(selfDestroying);
    selfDestroying = NULL;
    zg_runtime_quit(runtime);
    return NULL;
}

/* ---- Dialogs ---- */

static int dialogAnswers;
static zg_window *dialogWindow;
static zg_result dialogResult;
static int dialogButton;
static size_t dialogPaths;

static void onMessageBox(zg_window *window, zg_result result, int button, void *user)
{
    (void)user;
    dialogWindow = window;
    dialogResult = result;
    dialogButton = button;
    ++dialogAnswers;
}

static void onFileDialog(zg_window *window, zg_result result, const char *const *paths, size_t count, void *user)
{
    (void)paths;
    (void)user;
    dialogWindow = window;
    dialogResult = result;
    dialogPaths = count;
    ++dialogAnswers;
}

static int hasDialogAnswer(void) { return dialogAnswers > 0; }

/* ---- Windows opened and closed from callbacks, as an application's buttons do ---- */

static zg_context *sharedContext;
static zg_window *openedFirst, *openedSecond;

static zg_value *closeOpened(zg_call *call, void *user)
{
    (void)call;
    zg_window_destroy((zg_window *)user);
    if (user == openedFirst)
        openedFirst = NULL;
    return NULL;
}

static zg_window *openFromCallback(void)
{
    static const char page[] = "<body style='margin: 0; background: rgb(20, 200, 40)'></body>";
    zg_window *window = createWindow(sharedContext, "opened from a callback", 0);
    zg_view_bind(zg_window_view(window), "Test.close", closeOpened, window);
    zg_view_load_html(zg_window_view(window), page, sizeof page - 1, NULL);
    return window;
}

static int openFirst(void) { openedFirst = openFromCallback(); return 1; }
static int closeFirst(void)
{
    static const char script[] = "Test.close()";
    zg_view_evaluate(zg_window_view(openedFirst), script, sizeof script - 1, NULL);
    return 1;
}
static int openSecond(void) { openedSecond = openFromCallback(); return 1; }

#ifdef _WIN32
/* The color at the middle of a window's client area, as it is on screen. */
static unsigned middlePixel(zg_window *window)
{
    HWND hwnd = (HWND)zg_window_native_handle(window);
    RECT client;
    HDC screen = GetDC(NULL), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap;
    COLORREF color;
    GetClientRect(hwnd, &client);
    bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    SelectObject(memory, bitmap);
    PrintWindow(hwnd, memory, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    color = GetPixel(memory, client.right / 2, client.bottom / 2);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);
    return (unsigned)color; /* 0x00BBGGRR */
}

static int isGreen(unsigned color) { return (color & 0xff) < 60 && ((color >> 8) & 0xff) > 160 && ((color >> 16) & 0xff) < 80; }

/* Closes the first window of `className` titled `title` that appears, from a
   thread of its own (dialogs and menus run modal loops on theirs). */
typedef struct {
    const wchar_t *className;
    const wchar_t *title;
    int keys; /* 1: Down then Enter (a menu), 2: Escape, 0: WM_CLOSE */
} Closer;

static DWORD WINAPI closeWhenShown(void *argument)
{
    Closer *closer = argument;
    int attempt;
    for (attempt = 0; attempt < 200; ++attempt) {
        HWND found = FindWindowW(closer->className, closer->title);
        if (found && IsWindowVisible(found)) {
            Sleep(100);
            if (closer->keys == 2)
                PostMessageW(found, WM_KEYDOWN, VK_ESCAPE, 0);
            else if (closer->keys) {
                PostMessageW(found, WM_KEYDOWN, VK_DOWN, 0);
                PostMessageW(found, WM_KEYDOWN, VK_RETURN, 0);
            } else
                PostMessageW(found, WM_CLOSE, 0, 0);
            return 0;
        }
        Sleep(25);
    }
    return 1;
}

static void closeLater(Closer *closer)
{
    CloseHandle(CreateThread(NULL, 0, closeWhenShown, closer, 0, NULL));
}

/* A key press as the window gets it from Windows. */
static void postKey(HWND window, UINT message, WPARAM key)
{
    UINT scancode = MapVirtualKeyW((UINT)key, MAPVK_VK_TO_VSC);
    LPARAM lParam = 1 | ((LPARAM)scancode << 16);
    /* Home, End, the arrows: the extended keys, not the keypad's. */
    if (key == VK_HOME || key == VK_END || key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT)
        lParam |= (LPARAM)1 << 24;
    if (message == WM_KEYUP)
        lParam |= (LPARAM)3 << 30;
    PostMessageW(window, message, key, lParam);
}

static void pressCtrl(HWND window, WPARAM key)
{
    postKey(window, WM_KEYDOWN, VK_CONTROL);
    postKey(window, WM_KEYDOWN, key);
    postKey(window, WM_KEYUP, key);
    postKey(window, WM_KEYUP, VK_CONTROL);
}
#endif

int main(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_window_callbacks callbacks = { sizeof callbacks };
    zg_screen screen = { sizeof screen };
    zg_context *context;
    size_t screens, index, primaries = 0;
    unsigned width = 0, height = 0;
    int x = 0, y = 0;
    int placeable;
    zg_result result;

    setvbuf(stdout, NULL, _IONBF, 0);
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);

    /* Screens, before any window. */
    screens = zg_screen_count(runtime);
    CHECK(screens >= 1, "%u screen(s) connected", (unsigned)screens);
    for (index = 0; index < screens; ++index) {
        screen.size = sizeof screen;
        CHECK(zg_screen_get(runtime, index, &screen) == ZG_OK && screen.width > 0 && screen.height > 0 && screen.scale > 0
            && screen.work_width > 0 && screen.work_width <= screen.width && screen.work_height <= screen.height,
            "screen %u '%s': %ux%u at %d,%d, work area %ux%u, scale %.2f, %.0f Hz%s", (unsigned)index, screen.name,
            screen.width, screen.height, screen.x, screen.y, screen.work_width, screen.work_height, screen.scale,
            screen.refresh_rate, screen.primary ? ", primary" : "");
        primaries += screen.primary != 0;
    }
    CHECK(primaries == 1, "one primary screen (%u)", (unsigned)primaries);
    CHECK(zg_screen_get(runtime, screens, &screen) == ZG_ERROR_NOT_FOUND, "no screen past the last: %s", zg_last_error());

    /* A window created hidden, shown later. */
    mainWindow = createWindow(context, "ZeroG desktop test", 1);
    CHECK(mainWindow != NULL, "window created %s", mainWindow ? "" : zg_last_error());
    if (!mainWindow)
        return 1;
    callbacks.close_requested = onCloseRequested;
    callbacks.resized = onResized;
    callbacks.moved = onMoved;
    callbacks.state_changed = onStateChanged;
    callbacks.menu_item = onMenuItem;
    callbacks.files_dropped = onFilesDropped;
    CHECK(zg_window_set_callbacks(mainWindow, &callbacks) == ZG_OK, "window callbacks set");
    CHECK(zg_window_get_state(mainWindow) == ZG_WINDOW_HIDDEN, "created hidden: state %d", zg_window_get_state(mainWindow));
    runUntil(NULL, 200);
    CHECK(zg_window_get_state(mainWindow) == ZG_WINDOW_HIDDEN, "it stays hidden while its page runs");
    CHECK(zg_window_get_size(mainWindow, &width, &height) == ZG_OK && width == 400 && height == 300, "its size is the logical size asked for: %ux%u", width, height);
    CHECK(zg_window_show(mainWindow) == ZG_OK && runUntil(isShown, 3000), "shown: state_changed NORMAL (%d)", lastState);
    CHECK(zg_window_screen(mainWindow, &index) == ZG_OK && index < screens, "on screen %u", (unsigned)index);

    /* Sizes. */
    CHECK(zg_window_set_size(mainWindow, 500, 350) == ZG_OK && runUntil(isResized, 3000), "resized to 500x350: callback %ux%u", resizedWidth, resizedHeight);
    CHECK(zg_window_get_size(mainWindow, &width, &height) == ZG_OK && width == 500 && height == 350, "and reads 500x350: %ux%u", width, height);
    CHECK(zg_window_set_size(mainWindow, 0, 100) == ZG_ERROR_INVALID_ARGUMENT, "a size of 0 is refused: %s", zg_last_error());
    zg_window_set_min_size(mainWindow, 300, 200);
    zg_window_set_size(mainWindow, 100, 100);
    zg_window_get_size(mainWindow, &width, &height);
    CHECK(width >= 300 && height >= 200, "the minimum size holds: %ux%u", width, height);
    zg_window_set_min_size(mainWindow, 0, 0);
    zg_window_set_max_size(mainWindow, 450, 320);
    zg_window_set_size(mainWindow, 800, 600);
    zg_window_get_size(mainWindow, &width, &height);
    CHECK(width <= 450 && height <= 320, "the maximum size holds: %ux%u", width, height);
    zg_window_set_max_size(mainWindow, 0, 0);
    zg_window_set_size(mainWindow, 500, 350);
    CHECK(zg_window_set_resizable(mainWindow, 0) == ZG_OK && zg_window_set_resizable(mainWindow, 1) == ZG_OK, "resizable off and on");
    CHECK(zg_window_set_always_on_top(mainWindow, 1) == ZG_OK && zg_window_set_always_on_top(mainWindow, 0) == ZG_OK, "always on top on and off");
    CHECK(zg_window_focus(mainWindow) == ZG_OK, "focus asked for");

    /* Positions, where the window system lets applications place windows. */
    result = zg_window_set_position(mainWindow, 120, 140);
    placeable = result == ZG_OK;
    if (placeable) {
        CHECK(runUntil(isMoved, 3000), "moved to 120,140: callback %d,%d", movedX, movedY);
        CHECK(zg_window_get_position(mainWindow, &x, &y) == ZG_OK && x == 120 && y == 140, "and reads 120,140: %d,%d", x, y);
        CHECK(zg_window_center(mainWindow) == ZG_OK, "centered");
    } else {
        CHECK(result == ZG_ERROR_UNSUPPORTED && zg_window_get_position(mainWindow, &x, &y) == ZG_ERROR_UNSUPPORTED
            && zg_window_center(mainWindow) == ZG_ERROR_UNSUPPORTED, "no positions here: %s", zg_last_error());
    }

#ifdef _WIN32
    /* States (they need a window manager: Windows has one; Xvfb has none). */
    CHECK(zg_window_maximize(mainWindow) == ZG_OK && runUntil(isMaximized, 3000), "maximized (%d)", lastState);
    CHECK(zg_window_restore(mainWindow) == ZG_OK && runUntil(isNormal, 3000), "restored (%d)", lastState);
    CHECK(zg_window_minimize(mainWindow) == ZG_OK && runUntil(isMinimized, 3000), "minimized (%d)", lastState);
    CHECK(zg_window_restore(mainWindow) == ZG_OK && runUntil(isNormal, 3000), "restored (%d)", lastState);
#endif
    CHECK(zg_window_hide(mainWindow) == ZG_OK && runUntil(isHidden, 3000), "hidden (%d)", lastState);
    CHECK(zg_window_show(mainWindow) == ZG_OK && runUntil(isShown, 3000), "shown again (%d)", lastState);

    /* Light and dark. */
    {
        zg_view *view = zg_window_view(mainWindow);
        zg_color_scheme system = zg_system_color_scheme(runtime);
        const char *query = "matchMedia('(prefers-color-scheme: dark)').matches";
        CHECK(system == ZG_COLOR_SCHEME_LIGHT || system == ZG_COLOR_SCHEME_DARK, "the desktop is %s", system == ZG_COLOR_SCHEME_DARK ? "dark" : "light");
        CHECK(!strcmp(evaluate(view, query), system == ZG_COLOR_SCHEME_DARK ? "true" : "false"), "pages follow it by default: %s", evaluate(view, query));
        CHECK(zg_view_set_color_scheme(view, ZG_COLOR_SCHEME_DARK) == ZG_OK && !strcmp(evaluate(view, query), "true"), "dark: prefers-color-scheme is dark");
        CHECK(zg_view_set_color_scheme(view, ZG_COLOR_SCHEME_LIGHT) == ZG_OK && !strcmp(evaluate(view, query), "false"), "light: it is not");
        CHECK(zg_view_set_color_scheme(view, ZG_COLOR_SCHEME_SYSTEM) == ZG_OK, "back to the desktop's");
        CHECK(zg_view_set_color_scheme(view, (zg_color_scheme)9) == ZG_ERROR_INVALID_ARGUMENT, "an unknown scheme is refused");
    }

    /* Select dropdowns: the application's own (select_menu), then the engine's. */
    {
        zg_view *view = zg_window_view(mainWindow);
        zg_view_callbacks viewCallbacks = { sizeof viewCallbacks };
        float x = 0, y = 0, w = 0, h = 0;
        static const char script[] =
            "document.body.insertAdjacentHTML('afterbegin', \"<select id=s style='position:fixed;left:10px;top:10px;width:160px;font-size:16px'>"
            "<option value=a>Apple</option><optgroup label=Group><option value=b>Banana</option><option value=c>Cherry</option></optgroup>"
            "<option value=d disabled>Date</option></select>\");"
            "window.changed = ''; s.addEventListener('change', () => window.changed = s.value); 0";
        zg_view_evaluate(view, script, sizeof script - 1, NULL);
        viewCallbacks.select_menu = onSelectMenu;
        zg_view_set_callbacks(view, &viewCallbacks);
        zg_view_mouse_event(view, ZG_MOUSE_DOWN, 50, 20, ZG_MOUSE_LEFT, 1, 0);
        zg_view_mouse_event(view, ZG_MOUSE_UP, 50, 20, ZG_MOUSE_LEFT, 1, 0);
        CHECK(runUntil(hasSelectMenu, 3000), "a click on a <select> asks select_menu");
        if (selectMenu) {
            const zg_select_item *group = zg_select_menu_item_at(selectMenu, 1), *date = zg_select_menu_item_at(selectMenu, 4);
            zg_select_menu_bounds(selectMenu, &x, &y, &w, &h);
            CHECK(zg_select_menu_item_count(selectMenu) == 5 && group && group->kind == ZG_SELECT_ITEM_GROUP && !strcmp(group->label, "Group")
                && date && !date->enabled && zg_select_menu_item_at(selectMenu, 0)->selected,
                "its items: options, the group's label, a disabled one, the selected one (%u items)", (unsigned)zg_select_menu_item_count(selectMenu));
            CHECK(x >= 9 && x <= 11 && y >= 9 && y <= 11 && w >= 159 && w <= 161 && h > 10 && zg_select_menu_font_size(selectMenu) == 16,
                "its box %.0f,%.0f %.0fx%.0f and font size %.0f", x, y, w, h, zg_select_menu_font_size(selectMenu));
            zg_select_menu_choose(selectMenu, 3);
            selectMenu = NULL;
            CHECK(runUntil(selectChanged, 3000) && !strcmp(evaluate(view, "s.value"), "\"c\""), "an option chosen: the value changes, change fires (%s)", evaluate(view, "s.value"));
        }
        viewCallbacks.select_menu = NULL;
        zg_view_set_callbacks(view, &viewCallbacks);
        zg_view_evaluate(view, "window.changed = ''", 19, NULL);
#ifdef _WIN32
        {
            /* The engine's popup: opened by the click, chosen with the keyboard. */
            HWND hwnd = (HWND)zg_window_native_handle(mainWindow);
            zg_view_mouse_event(view, ZG_MOUSE_DOWN, 50, 20, ZG_MOUSE_LEFT, 1, 0);
            zg_view_mouse_event(view, ZG_MOUSE_UP, 50, 20, ZG_MOUSE_LEFT, 1, 0);
            runUntil(NULL, 600);
            postKey(hwnd, WM_KEYDOWN, VK_HOME);
            postKey(hwnd, WM_KEYUP, VK_HOME);
            postKey(hwnd, WM_KEYDOWN, VK_RETURN);
            postKey(hwnd, WM_KEYUP, VK_RETURN);
            CHECK(runUntil(selectChanged, 3000) && !strcmp(evaluate(view, "s.value"), "\"a\""), "the engine's dropdown: Home and Enter choose the first option (%s)", evaluate(view, "s.value"));
        }
#endif
        zg_view_evaluate(view, "s.remove()", 10, NULL);
    }

    /* The clipboard from native code. */
    {
        const char *types[2] = { "text/plain", "text/html" };
        const char *data[2] = { "from ZeroG \xe2\x9c\x93", "<b>bold</b>" };
        size_t lengths[2];
        char *read = NULL;
        size_t length = 0;
        lengths[0] = strlen(data[0]);
        lengths[1] = strlen(data[1]);
        CHECK(zg_clipboard_write(runtime, 2, types, (const void *const *)data, lengths) == ZG_OK, "text and HTML written to the clipboard");
        if (!placeable) {
            /* Wayland sets the selection only for the client with the keyboard
               focus, which a headless compositor gives no one. */
            printf("skip reading it back on Wayland (no keyboard focus in a headless session)\n");
        } else {
            CHECK(zg_clipboard_read(runtime, "text/plain", &read, &length) == ZG_OK && length == lengths[0] && !strcmp(read, data[0]),
                "the text read back: '%s'", read ? read : "");
            zg_free(read);
            read = NULL;
            CHECK(zg_clipboard_read(runtime, "text/html", &read, &length) == ZG_OK && strstr(read, "<b>bold</b>") != NULL,
                "the HTML read back: '%s'", read ? read : "");
            zg_free(read);
            CHECK(zg_clipboard_read(runtime, "image/png", &read, &length) == ZG_ERROR_NOT_FOUND, "no image: %s", zg_last_error());
        }
    }

    /* The icon. */
    {
        static unsigned char pixels[32 * 32 * 4];
        size_t i;
        for (i = 0; i < sizeof pixels; i += 4) {
            pixels[i] = 0x29;
            pixels[i + 1] = 0xd3;
            pixels[i + 2] = 0xf4;
            pixels[i + 3] = 0xff;
        }
        result = zg_window_set_icon(mainWindow, pixels, 32, 32);
        if (placeable)
            CHECK(result == ZG_OK, "an icon set");
        else
            CHECK(result == ZG_OK || result == ZG_ERROR_UNSUPPORTED, "an icon set, or the compositor takes none: %s", result ? zg_last_error() : "set");
#ifdef _WIN32
        CHECK(SendMessageW((HWND)zg_window_native_handle(mainWindow), WM_GETICON, ICON_BIG, 0) != 0, "the window has it");
#endif
        CHECK(zg_window_set_icon(mainWindow, NULL, 32, 32) == ZG_ERROR_INVALID_ARGUMENT, "an icon needs pixels");
    }

#ifdef _WIN32
    /* Files dropped from outside, to the application (WM_DROPFILES). */
    {
        static const wchar_t path[] = L"C:\\zerog\\dropped.txt";
        HGLOBAL memory = GlobalAlloc(GHND, sizeof(DROPFILES) + sizeof path + sizeof(wchar_t));
        DROPFILES *drop = (DROPFILES *)GlobalLock(memory);
        drop->pFiles = sizeof(DROPFILES);
        drop->pt.x = 30;
        drop->pt.y = 40;
        drop->fWide = TRUE;
        memcpy((char *)drop + sizeof(DROPFILES), path, sizeof path);
        GlobalUnlock(memory);
        PostMessageW((HWND)zg_window_native_handle(mainWindow), WM_DROPFILES, (WPARAM)memory, 0);
        CHECK(runUntil(hasDrop, 3000) && !strcmp(droppedPath, "C:\\zerog\\dropped.txt"), "a dropped file's path: '%s'", droppedPath);
    }
#endif

    /* Menus. */
    {
        zg_menu *bar = zg_menu_create(), *file = zg_menu_create(), *popup = zg_menu_create();
        CHECK(zg_menu_add_item(file, 1, "&Open", "Ctrl+O", 0) == ZG_OK, "an item with an accelerator");
        CHECK(zg_menu_add_item(file, 2, "&Save", "Ctrl+Shift+S", ZG_MENU_ITEM_DISABLED) == ZG_OK, "a disabled item");
        CHECK(zg_menu_add_separator(file) == ZG_OK, "a separator");
        CHECK(zg_menu_add_item(file, 3, "&Word wrap", NULL, ZG_MENU_ITEM_CHECKBOX | ZG_MENU_ITEM_CHECKED) == ZG_OK, "a checkbox item");
        CHECK(zg_menu_add_submenu(bar, "&File", file) == ZG_OK, "a submenu");
        CHECK(zg_menu_add_item(file, 0, "Zero", NULL, 0) == ZG_ERROR_INVALID_ARGUMENT, "id 0 is refused: %s", zg_last_error());
        CHECK(zg_menu_add_item(file, 70000, "Big", NULL, 0) == ZG_ERROR_INVALID_ARGUMENT, "ids past 65535 are refused");
        CHECK(zg_menu_add_item(file, 4, "Bad", "Ctrl+Nope", 0) == ZG_ERROR_INVALID_ARGUMENT, "an unknown key is refused: %s", zg_last_error());
        zg_menu_add_item(popup, 10, "First", NULL, 0);
        zg_menu_add_item(popup, 11, "Second", NULL, 0);
        zg_menu_destroy(file); /* copied into the bar */
#ifdef _WIN32
        {
            HWND hwnd = (HWND)zg_window_native_handle(mainWindow);
            static Closer menuCloser = { L"#32768", NULL, 1 };
            CHECK(zg_window_set_menu(mainWindow, bar) == ZG_OK && GetMenu(hwnd) != NULL, "a native menu bar");
            menuChoices = 0;
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1, 0), 0);
            CHECK(runUntil(hasMenuChoice, 3000) && chosenItem == 1, "an item chosen by WM_COMMAND: %u", chosenItem);
            menuChoices = 0;
            pressCtrl(hwnd, 'O');
            CHECK(runUntil(hasMenuChoice, 3000) && chosenItem == 1, "an item chosen by its accelerator: %u", chosenItem);
            menuChoices = 0;
            pressCtrl(hwnd, 'S'); /* not Ctrl+Shift+S, and disabled */
            CHECK(zg_window_menu_set_enabled(mainWindow, 1, 0) == ZG_OK, "an item disabled");
            pressCtrl(hwnd, 'O');
            runUntil(NULL, 300);
            CHECK(menuChoices == 0, "disabled items and other keys choose nothing (%d)", menuChoices);
            CHECK(zg_window_menu_set_enabled(mainWindow, 1, 1) == ZG_OK && zg_window_menu_set_checked(mainWindow, 3, 0) == ZG_OK
                && !(GetMenuState(GetMenu(hwnd), 3, MF_BYCOMMAND) & MF_CHECKED), "a check mark removed");
            menuChoices = 0;
            closeLater(&menuCloser);
            CHECK(zg_window_popup_menu(mainWindow, popup, 20, 20) == ZG_OK && runUntil(hasMenuChoice, 3000) && chosenItem == 10,
                "a popup menu's item chosen by keyboard: %u", chosenItem);
            {
                static Closer dismisser = { L"#32768", NULL, 2 };
                menuChoices = 0;
                chosenItem = 99;
                closeLater(&dismisser);
                CHECK(zg_window_popup_menu(mainWindow, popup, 20, 20) == ZG_OK && runUntil(hasMenuChoice, 3000) && chosenItem == 0,
                    "a popup menu dismissed: menu_item with 0 (%u)", chosenItem);
            }
            CHECK(zg_window_set_menu(mainWindow, NULL) == ZG_OK && GetMenu(hwnd) == NULL, "the menu bar removed");
        }
#else
        CHECK(zg_window_set_menu(mainWindow, bar) == ZG_ERROR_UNSUPPORTED, "no native menus here: %s", zg_last_error());
        CHECK(zg_window_popup_menu(mainWindow, popup, 20, 20) == ZG_ERROR_UNSUPPORTED, "nor popup menus");
        CHECK(zg_window_menu_set_checked(mainWindow, 3, 0) == ZG_ERROR_UNSUPPORTED, "nor their items");
#endif
        zg_menu_destroy(bar);
        zg_menu_destroy(popup);
    }

    /* Dialogs. */
    {
        zg_message_box box = { sizeof box };
        zg_message_button buttons[2] = { { 7, "Yes", ZG_MESSAGE_BUTTON_DEFAULT }, { 8, "No", ZG_MESSAGE_BUTTON_CANCEL } };
        zg_file_dialog dialog = { sizeof dialog };
        zg_file_filter filter = { "Text", "txt;md" };
        zg_window *doomed;

        dialog.kind = (zg_file_dialog_kind)99;
        CHECK(zg_window_file_dialog(mainWindow, &dialog, onFileDialog, NULL) == ZG_ERROR_INVALID_ARGUMENT, "an unknown dialog kind is refused");
        dialog.kind = ZG_FILE_DIALOG_OPEN;
        dialog.filter_count = 1;
        CHECK(zg_window_file_dialog(mainWindow, &dialog, onFileDialog, NULL) == ZG_ERROR_INVALID_ARGUMENT, "filters counted but missing are refused");
        CHECK(zg_window_message_box(mainWindow, &box, NULL, NULL) == ZG_ERROR_INVALID_ARGUMENT, "a dialog needs its callback");

        /* Answered after its window was destroyed: NULL, NOT_FOUND. */
        doomed = createWindow(context, "doomed", 1);
        box.title = "never shown";
        box.message = "the window goes first";
        dialogAnswers = 0;
        CHECK(zg_window_message_box(doomed, &box, onMessageBox, NULL) == ZG_OK, "a message box asked for");
        CHECK(dialogAnswers == 0, "the answer does not come during the call");
        zg_window_destroy(doomed);
        CHECK(runUntil(hasDialogAnswer, 3000) && dialogWindow == NULL && dialogResult == ZG_ERROR_NOT_FOUND,
            "its window destroyed first: the answer comes with no window (%d)", dialogResult);

#ifdef _WIN32
        {
            static Closer boxCloser = { NULL, L"ZeroG test box", 0 };
            static Closer dialogCloser = { NULL, L"ZeroG test open", 0 };
            box.title = "ZeroG test box";
            box.message = "Closed by the test";
            box.kind = ZG_MESSAGE_WARNING;
            box.buttons = buttons;
            box.button_count = 2;
            dialogAnswers = 0;
            closeLater(&boxCloser);
            CHECK(zg_window_message_box(mainWindow, &box, onMessageBox, NULL) == ZG_OK && runUntil(hasDialogAnswer, 5000)
                && dialogWindow == mainWindow && dialogResult == ZG_OK && dialogButton == 8,
                "a message box closed: its cancel button's id (%d, %s)", dialogButton, zg_last_error());

            dialog.title = "ZeroG test open";
            dialog.filters = &filter;
            dialogAnswers = 0;
            closeLater(&dialogCloser);
            CHECK(zg_window_file_dialog(mainWindow, &dialog, onFileDialog, NULL) == ZG_OK && runUntil(hasDialogAnswer, 5000)
                && dialogWindow == mainWindow && dialogResult == ZG_OK && dialogPaths == 0,
                "a file dialog cancelled: no paths (%d)", dialogResult);
        }
#else
        (void)buttons;
        (void)filter;
#endif
    }

    /* A window that destroys itself from its own page's call. */
    selfDestroying = createWindow(context, "self-destroying", 1);
    {
        static const char page[] = "<script>setTimeout(() => Test.destroySelf(), 50);</script>";
        zg_view_bind(zg_window_view(selfDestroying), "Test.destroySelf", destroySelf, NULL);
        zg_view_load_html(zg_window_view(selfDestroying), page, sizeof page - 1, NULL);
    }
    runUntil(NULL, 3000);
    CHECK(selfDestroying == NULL, "a window destroyed from its own page's call");
    CHECK(zg_window_get_size(mainWindow, &width, &height) == ZG_OK, "and the loop goes on with the others");
    {
        /* A window opened after it: its page loads, renders and follows the desktop. */
        zg_window *after = createWindow(context, "after", 0);
        static const char page[] = "<body style='background: rgb(1, 2, 3)'><script>document.title = 'after'</script></body>";
        zg_view_load_html(zg_window_view(after), page, sizeof page - 1, NULL);
        runUntil(NULL, 1500);
        CHECK(!strcmp(evaluate(zg_window_view(after), "document.title"), "\"after\""), "a window opened next loads its page: %s", evaluate(zg_window_view(after), "document.title"));
        CHECK(!strcmp(evaluate(zg_window_view(after), "matchMedia('(prefers-color-scheme: dark)').matches"), zg_system_color_scheme(runtime) == ZG_COLOR_SCHEME_DARK ? "true" : "false"),
            "and follows the desktop's scheme");
#ifdef _WIN32
        CHECK(middlePixel(after) == 0x030201, "and shows it (0x%06x)", middlePixel(after));
#endif
        zg_window_destroy(after);
    }
    {
        /* Opened from a callback, closed by its own page, then another opened
           from a callback: each shows its page. */
        sharedContext = context;
#ifdef _WIN32
        CHECK(middlePixel(mainWindow) == 0x553333, "the main window shows its page, read back (0x%06x)", middlePixel(mainWindow));
#endif
        runUntil(openFirst, 3000);
        runUntil(NULL, 1500);
#ifdef _WIN32
        CHECK(isGreen(middlePixel(openedFirst)), "a window opened from a callback shows its page (0x%06x)", middlePixel(openedFirst));
#endif
        runUntil(closeFirst, 3000);
        runUntil(NULL, 300);
        CHECK(openedFirst == NULL, "and closes from its own page");
        runUntil(openSecond, 3000);
        runUntil(NULL, 1500);
        CHECK(!strcmp(evaluate(zg_window_view(openedSecond), "document.body.style.background"), "\"rgb(20, 200, 40)\""), "the next one opened from a callback loads its page");
#ifdef _WIN32
        CHECK(isGreen(middlePixel(openedSecond)), "and shows it (0x%06x)", middlePixel(openedSecond));
#endif
        zg_window_destroy(openedSecond);
    }

#ifdef _WIN32
    /* Close requests: refused, then accepted (the loop ends with the last window). */
    {
        HWND hwnd = (HWND)zg_window_native_handle(mainWindow);
        allowClose = 0;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        CHECK(runUntil(hasCloseRequest, 3000) && zg_window_get_state(mainWindow) != ZG_WINDOW_HIDDEN, "a close request refused: the window stays");
        allowClose = 1;
        closeRequests = 0;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        runUntil(NULL, 3000);
        CHECK(closeRequests == 1 && zg_window_get_state(mainWindow) == ZG_WINDOW_HIDDEN, "a close request accepted: hidden, and the loop ended (%d requests, state %d)",
            closeRequests, zg_window_get_state(mainWindow));
    }
#endif

    zg_window_destroy(mainWindow);
    zg_runtime_destroy(runtime);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
