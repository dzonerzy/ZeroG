# Overview

The goal is a web engine based on WebKit, open source, with a small and stable C API, distributable as a **single executable** on Linux, Windows and macOS. In practice: an "open source Ultralight", with offscreen rendering and presentation controlled by the application.

The project is called **ZeroG**, and the public API uses the `zg_` prefix. The name of the Zig framework built on top of it is still to be decided (proposal: Capsule).

The name does not contain "WebKit", which is a [registered trademark of Apple](https://en.wikipedia.org/wiki/WebKit). Before publication a trademark search must also be done in EUIPO: in the USA "ZERO-G" is already registered in class 9, for products other than software, and the npm package `zero-g` is taken.

## Goals

- Complete web engine (HTML, CSS, JavaScript via JavaScriptCore), compatible with modern frontends (React, Vue, Svelte, Vite).
- Identical behavior on all platforms: same engine, same version, no dependency on the system webview.
- Distribution as a single binary, with static linking where the license allows it.
- Offscreen rendering: the engine produces frames, the application decides where to present them (window, game texture, overlay).
- Stable C API, usable from any language; Zig as the first client.
- Single-process and native JS bindings: engine and application in the same process, with host functions exposed directly as JavaScript objects, without IPC.

## Non-goals

- It is not a browser: no tabs, extensions, sync, navigation UI.
- It is not meant to browse the open web safely like a modern browser; the main use case is application content (see [Security](security.md)).
- No mobile support in the first phase.
- No fork of WebCore and JavaScriptCore: the engine adds its own port and embedding layer, and changes to upstream code remain minimal patches.

## Target use cases

- Desktop frameworks in the style of Wails or Tauri that want a single engine instead of the system webviews.
- Game UIs and overlays rendered into a texture.
- Desktop tools that must work on Linux without depending on the distro's WebKitGTK version.
- Kiosk and embedded applications that use WPE today but want a simpler integration.
