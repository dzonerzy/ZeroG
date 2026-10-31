# Input, IME, clipboard, accessibility

In window mode the engine translates native events on its own; in texture mode it is the application that injects them via the API. In both cases events go through the same internal path, to avoid different behaviors.

## Events

- Mouse (movement, buttons, wheel with precise deltas), keyboard (keycode, text, modifiers), touch and pen in a later phase.
- In texture mode coordinates arrive in the view's logical pixels; for a game UI on a 3D surface it is the application that projects the pointer.
- Callbacks from the engine to the host: cursor change, tooltip, focus request, opening of external links.

## IME

Support for input methods (Chinese, Japanese, Korean, emoji) is mandatory for a usable engine. In window mode it integrates with the system IME (IBus or Fcitx on Linux, TSF on Windows, NSTextInputClient on macOS). In texture mode the engine exposes the text cursor position and composition events, and the application decides how to show them.

## Clipboard, drag and drop, dialogs

- System clipboard for reading and writing, with text, HTML and images (`zg_runtime_set_clipboard`, or SDL3's in window mode); `navigator.clipboard` included.
- Drag and drop of files, text and URLs onto the view (`zg_view_drag`), and HTML5 drag inside the page; dragging out of the view later.
- File chooser and JS dialogs (`alert`, `confirm`, `prompt`) are callbacks to the host, with a default native implementation in window mode.
- Context menu: the engine does not draw menus. A right click (on press; on release on Windows), the menu key or Shift+F10 call the host (`zg_view_callbacks.context_menu`) with the position, what is underneath (link, image, media, selection, editable field) and WebCore's items for that point; the host shows them as it wants (a native menu, in the Zig framework) and replies with the chosen item, which the engine executes (copy, paste, open the link, download). The items that an engine for applications does not have (new windows, web search, dictionary, speech, spelling) are removed. A page that cancels the `contextmenu` event makes its own menu (with the bindings) and the host is not called. Without a callback nothing appears.

## Frameless and transparent windows

- `frameless` removes borders and title bar; `transparent` (0.3) shows what is behind the window where the page is transparent, with the alpha composited by the system: on Windows a DirectComposition swap chain with premultiplied alpha (ANGLE's `EGL_ANGLE_direct_composition`), on Linux an ARGB visual (X11) or a surface with alpha (Wayland).
- Frameless windows are moved from the regions that the page marks with the CSS custom property `--zg-app-region: drag` (inherited: controls inside a region say `no-drag`), and are resized from the edges. The system asks what is at a point (`WM_NCHITTEST` on Windows, through SDL's hit test) and the engine replies with a hit test of the page: the move is the system's, with snap, double click to maximize and Wayland included.
- Click-through (`click_through`, 0.3): where the page is fully transparent clicks go to what is behind. It costs a copy of every frame on the CPU: on Windows the window becomes a layered window updated from that copy (`UpdateLayeredWindow`, which the system hit-tests by alpha) instead of the swap chain; on X11 the frame's alpha becomes the window's input shape (XShape via SDL). On Wayland no effect: SDL does not set the input region.
- With "Show window contents while dragging" off, Windows moves every window, including these, as a rectangle: it is the user's setting, respected.
- Application menu, tray and dialogs belong to the Zig framework, which reaches the native window with `zg_window_native_handle`.

## Accessibility

WebCore builds the accessibility tree, but the bridge to the screen reader is specific to each OS, and in its own port it must be written. Done (window mode): **UI Automation** on Windows, a provider written from scratch (names, roles, states, Invoke, Value, Toggle, Text, events); **AT-SPI** on Linux, its own bridge on libdbus loaded at runtime, without GLib (Accessible, Application, Component, Action, Value, Text, events). Remaining are NSAccessibility with the macOS port and accessibility in texture mode, where the window belongs to the host.
