# ZeroG

ZeroG is an open source web engine for applications: WebKit's WebCore and
JavaScriptCore in a single process, behind a small, stable C API
([`include/zerog.h`](https://github.com/dzonerzy/ZeroG/blob/main/include/zerog.h)),
linked statically into your program. One executable, no helper processes, no
runtime to install.

- **Desktop apps** with a web frontend and native code: ZeroG opens the
  window, serves your frontend from inside your binary under `app://`, and
  lets the page call your C functions directly, synchronously or with
  Promises.
- **Game and app overlays**: in texture mode the engine renders into GPU
  textures your renderer composites, on Direct3D 9, 11 and 12, Vulkan and
  OpenGL, without ever blocking your render thread.
- **Headless rendering**: CPU views give you pixels, for tests, thumbnails or
  anything else.

The same pages render the same way on Windows and Linux: one port, the same
Skia renderer, the same fonts and text stack everywhere.

## Where to start

<div class="grid cards" markdown>

- [**Getting started**](getting-started/index.md): build the engine and run
  your first app.
- [**Guides**](guides/index.md): window mode, texture mode, bindings,
  assets, storage, and the rest.
- [**API reference**](reference/index.md): every type and function of
  `zerog.h`.
- [**Design**](design/index.md): why ZeroG is built the way it is.

</div>

## Status

The C API is at version 0.3, frozen: additions only within 0.x
([changes](api-changes.md)). Windows and Linux are supported; macOS is
planned.

| | Windows | Linux |
| --- | --- | --- |
| Window mode | Yes (Win32, transparent, frameless, click-through) | Yes (X11, Wayland) |
| Texture mode | D3D9, D3D11, D3D12, Vulkan | OpenGL (EGL), Vulkan (dma-buf) |
| CPU mode | Yes | Yes |
| Accessibility | UI Automation | AT-SPI |
