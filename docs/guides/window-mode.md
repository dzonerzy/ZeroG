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
own native popup and answer.

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

## File dialogs, drops and the clipboard

- `<input type=file>` shows the system's file dialog, unless you set a
  `file_chooser` callback.
- Files, text and URLs dropped on the window reach the page as `drop` events,
  with the files in `DataTransfer.files`.
- The clipboard is the system's once a window exists: copy, cut, paste and
  `navigator.clipboard` work.

## The Web Inspector

In the `dev` and `debug` profiles F12 and Ctrl+Shift+I open the
[Web Inspector](inspector.md).

## Several windows

Each window gets its own events and renders independently. Closing the last
window ends `zg_runtime_run`.

## The console

A desktop application on Windows is linked as a GUI program (the `WINDOWS`
subsystem, `/SUBSYSTEM:WINDOWS` or `-mwindows`), so no console opens; that is a
property of your executable, not of the engine. Route the engine's messages
with `zg_runtime_config.log` and pages' console output with the `console`
callback.
