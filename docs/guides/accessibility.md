# Accessibility

In window mode the page is exposed to screen readers and other assistive
technology, through the platform's own interface:

- **Windows**: UI Automation (Narrator, NVDA, JAWS).
- **Linux**: AT-SPI over D-Bus (Orca). libdbus is loaded at run time; nothing
  happens without a session bus or with `NO_AT_BRIDGE=1`.

Controls have their names, roles, states and bounds. Buttons and links can be
pressed, text fields read (by character, word and line) and set without moving
the focus, check boxes toggled, ranges read and set; focus, value, name, state
and structure changes are announced.

Nothing to do in the application: semantic HTML and ARIA in your frontend are
what assistive technology sees.

Texture-mode views have no bridge yet: an overlay's UI is not visible to screen
readers.
