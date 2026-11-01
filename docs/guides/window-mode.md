# Window mode

`zg_window_create` gives you a native window and the view inside it. The engine
renders at the display's rate on its compositor thread, takes input from the
window (mouse, keyboard, IME, drops) and presents; you run `zg_runtime_run`.

```c
zg_window_config config = { sizeof config };
config.title = "My App";
config.width = 1024;    /* logical pixels: the window follows the display's scale */
config.height = 768;
zg_window *win = zg_window_create(ctx, &config);
zg_view *view = zg_window_view(win);
```

Windows uses Win32 with ANGLE over Direct3D 11; Linux uses X11 or Wayland with
the system's EGL driver. `zg_window_native_handle` gives the `HWND` or the
X11/Wayland surface, for platform calls ZeroG does not wrap.

A window appears with its first frame, so it never shows blank. Set `hidden`
to keep it hidden until `zg_window_show`, for instance to size and place it
first.

## Controlling the window

Sizes are logical pixels, the page's CSS pixels. Positions are desktop
coordinates, the units of the screens' bounds (physical pixels on Windows and
X11).

```c
zg_window_set_size(win, 1200, 800);
zg_window_set_min_size(win, 640, 480);   /* 0, 0: no limit */
zg_window_center(win);
zg_window_maximize(win);
zg_window_set_always_on_top(win, 1);
if (zg_window_get_state(win) == ZG_WINDOW_MINIMIZED)
    zg_window_restore(win);
```

There are also `set_max_size`, `set_resizable`, `set_position`/`get_position`,
`minimize`, `show`, `hide` and `focus`. Wayland lets no application place its
windows: the position functions fail there with `ZG_ERROR_UNSUPPORTED`.

## Window events

```c
static int on_close(zg_window *win, void *user)
{
    return !has_unsaved_changes();   /* 0 keeps the window open */
}

zg_window_callbacks callbacks = { sizeof callbacks };
callbacks.close_requested = on_close;
callbacks.resized = on_resized;          /* logical pixels */
callbacks.moved = on_moved;              /* not on Wayland */
callbacks.focus_changed = on_focus;
callbacks.state_changed = on_state;      /* normal, minimized, maximized, fullscreen, hidden */
callbacks.screen_changed = on_screen;    /* another screen, or a new scale */
callbacks.menu_item = on_menu_item;      /* see "Native menus" */
zg_window_set_callbacks(win, &callbacks);
```

A closed window is hidden at once and stays valid until `zg_window_destroy`.
Callbacks, bindings included, may destroy any window, the one they run for
too.

`files_dropped` gets the paths of files dropped on the window, which pages
never see (their `File` objects have names, not paths). Return nonzero to keep
the drop from the page.

## Icon

On Windows a window has the executable's icon (its first icon resource). Set
another, or one at all on Linux, with RGBA pixels:

```c
zg_window_set_icon(win, rgba, 256, 256);
```

## Light and dark

Pages see the desktop's dark or light setting through `prefers-color-scheme`,
and follow it when it changes; a page with `color-scheme: light dark` also gets
dark form controls and scrollbars. A window's frame follows its view (a dark
title bar on Windows). An application with its own theme setting overrides it:

```c
zg_view_set_color_scheme(view, ZG_COLOR_SCHEME_DARK);   /* or LIGHT, SYSTEM */
zg_color_scheme desktop = zg_system_color_scheme(rt);
```

## Screens

```c
for (size_t i = 0; i < zg_screen_count(rt); ++i) {
    zg_screen screen = { sizeof screen };
    zg_screen_get(rt, i, &screen);
    /* screen.x, y, width, height; the work area without taskbars and panels;
       screen.scale (1.5 at 150%), refresh_rate, primary, name */
}
size_t mine;
zg_window_screen(win, &mine);
```

## Dialogs

The system's own dialogs, modal to their window: the Windows ones, and on Linux
the desktop's through xdg-desktop-portal (zenity if no portal runs). The answer
comes to a callback, always once and never during the call; `win` is NULL if
the window was destroyed first.

```c
static void on_opened(zg_window *win, zg_result result, const char *const *paths,
                      size_t count, void *user)
{
    for (size_t i = 0; i < count; ++i)
        open_document(paths[i]);   /* count 0: cancelled */
}

zg_file_filter filters[] = { { "Images", "png;jpg;webp" }, { "All files", "*" } };
zg_file_dialog dialog = { sizeof dialog };
dialog.kind = ZG_FILE_DIALOG_OPEN;   /* or OPEN_FOLDER, SAVE */
dialog.title = "Open an image";
dialog.filters = filters;
dialog.filter_count = 2;
dialog.multiple = 1;
zg_window_file_dialog(win, &dialog, on_opened, NULL);
```

`zg_window_message_box` shows an information, warning or error box with your
buttons; the callback gets the chosen button's id (the cancel button's if the
box was closed). The window's page waits while the box is open.

```c
zg_message_button buttons[] = {
    { 1, "Save", ZG_MESSAGE_BUTTON_DEFAULT },
    { 2, "Discard", 0 },
    { 3, "Cancel", ZG_MESSAGE_BUTTON_CANCEL },
};
zg_message_box box = { sizeof box };
box.kind = ZG_MESSAGE_WARNING;
box.title = "Unsaved changes";
box.message = "Save the document before closing?";
box.buttons = buttons;
box.button_count = 3;
zg_window_message_box(win, &box, on_answer, NULL);
```

## Native menus

On Windows, windows have native menu bars and popup menus. A `zg_menu`
describes one; the window builds its menu from it, so the description can be
destroyed afterwards. Chosen items come to the `menu_item` callback with their
id, by click or by their accelerator (handled ahead of the page).

```c
zg_menu *file = zg_menu_create();
zg_menu_add_item(file, 1, "&Open...", "Ctrl+O", 0);
zg_menu_add_item(file, 2, "&Save", "Ctrl+S", ZG_MENU_ITEM_DISABLED);
zg_menu_add_separator(file);
zg_menu_add_item(file, 3, "&Word wrap", NULL, ZG_MENU_ITEM_CHECKBOX | ZG_MENU_ITEM_CHECKED);

zg_menu *bar = zg_menu_create();
zg_menu_add_submenu(bar, "&File", file);   /* copied */
zg_window_set_menu(win, bar);
zg_menu_destroy(file);
zg_menu_destroy(bar);

zg_window_menu_set_enabled(win, 2, 1);     /* once there is something to save */
zg_window_menu_set_checked(win, 3, 0);     /* check marks change only this way */
```

Ids go from 1 to 65535. `zg_window_popup_menu(win, menu, x, y)` shows a popup
menu at a point of the view, for instance at a `zg_context_menu`'s position;
`menu_item` then comes with the chosen id, or 0 if it was dismissed.
`examples/desktop` shows the page's context menu natively this way.

On Linux there are no native menus (`ZG_ERROR_UNSUPPORTED`): applications draw
them in the page there, where they look the same on every desktop.

## Frameless windows and drag regions

`frameless` removes the system's title bar and borders. The page then says
which parts move the window, with a CSS custom property:

```css
header { --zg-app-region: drag; }        /* drags the window */
header button { --zg-app-region: no-drag; } /* but its buttons stay buttons */
```

Dragging gets the system's behaviour (snapping, double click to maximize), and
a resizable frameless window resizes by its edges. Your page draws its own
close and minimize buttons and calls bindings for them.

## Transparent and shaped windows

`transparent` makes what the page leaves transparent (no `html`/`body`
background, or a translucent one) show the desktop behind the window, alpha
blended by the window system. With `frameless`, the window can have any shape:
a rounded card, a floating widget.

`click_through`, with `transparent`, sends clicks where the page is fully
transparent to whatever is behind the window. It costs a copy of every frame to
the CPU (on Windows the window becomes a layered window updated from it). It
works on Windows and X11; Wayland has no way to do it.

## Context menus

A right click, the menu key or Shift+F10 asks your `context_menu` callback,
with the position, what is under it (a link, an image, selected text, an
editable field) and WebCore's items for it. The engine draws no menu: show your
own popup (on Windows with `zg_window_popup_menu`) and answer.

```c
static void on_context_menu(zg_view *view, zg_context_menu *menu, void *user)
{
    float x, y;
    zg_context_menu_position(menu, &x, &y);
    for (size_t i = 0; i < zg_context_menu_item_count(menu); ++i) {
        const zg_context_menu_item *item = zg_context_menu_item_at(menu, i);
        /* item->label, item->kind, item->enabled, item->parent for submenus */
    }
    unsigned chosen = show_native_menu(menu, x, y);  /* your code */
    if (chosen)
        zg_context_menu_select(menu, chosen);  /* copy, paste, open the link... */
    else
        zg_context_menu_cancel(menu);
}
```

Without the callback no menu appears. A page that cancels its `contextmenu`
event (to show its own HTML menu) gets no host menu.

## Select dropdowns

A `<select>` opens a dropdown under it, drawn by the engine in a popup window
like the page (light or dark), chosen with the mouse or the keyboard (arrows,
Home, End, Page Up and Down, Enter, Escape, typing an option's first letters).
To show your own instead, set the `select_menu` view callback: it gets the
element's box, the font size and the items (options, optgroup labels,
separators), and answers with `zg_select_menu_choose` or `_cancel`.

## File dialogs, drops and the clipboard

- `<input type=file>` shows the system's file dialog, unless you set a
  `file_chooser` callback.
- Files, text and URLs dropped on the window reach the page as `drop` events,
  with the files in `DataTransfer.files`.
- The clipboard is the system's once a window exists: copy, cut, paste and
  `navigator.clipboard` work, and the application reads and writes it with
  `zg_clipboard_read` and `zg_clipboard_write` (text, HTML, images by MIME
  type).

## The Web Inspector

In the `dev` and `debug` profiles F12 and Ctrl+Shift+I open the
[Web Inspector](inspector.md).

## Several windows

Each window gets its own events and renders independently. Once every window
is closed, `zg_runtime_run` returns.

## The console

A desktop application on Windows is linked as a GUI program (the `WINDOWS`
subsystem, `/SUBSYSTEM:WINDOWS` or `-mwindows`), so no console opens; that is a
property of your executable, not of the engine. Route the engine's messages
with `zg_runtime_config.log` and pages' console output with the `console`
callback.
