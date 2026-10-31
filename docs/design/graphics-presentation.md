# Graphics and presentation

With a single process the problem of frame transport disappears: **Skia renders directly onto a GPU context of the application or one created by the engine in the same process**. No DMA-BUF, textures shared between processes or IOSurface are needed.

## Modes

- **Window mode**: the engine creates a native window and a swapchain, and presents on its own. It is the desktop framework case.
- **Texture mode**: the application provides its own GPU device (or accepts the engine's) and receives a texture with the rendered view. It is the case of game UIs and overlays.
- **CPU mode**: Skia rasterizes into an in-memory pixel buffer. It serves environments without a GPU (VMs, CI, servers) and rendering tests.

## GPU backend per platform

| Platform | Skia backend | Notes |
| --- | --- | --- |
| Linux | OpenGL (EGL) | Drivers loaded from the system with `dlopen`. For Vulkan hosts the engine renders with GL into dma-buf buffers that the host imports (`ZG_SURFACE_VULKAN`, `zg_surface_dmabuf`); GBM and the DRM device are loaded at runtime only for this |
| Windows | OpenGL via ANGLE on D3D11 | The most proven Skia path on Windows; direct texture sharing with D3D11 games; WebGL included |
| macOS | Metal | Skia's Metal backend is mature |

## Compositing

WebCore separates the parts of the page into layers (CSS transforms, animations, video, WebGL canvases) and expects a compositor that merges them. It is the most demanding part of the platform layer.

- **CPU views**: accelerated compositing disabled. WebCore draws everything into a single Skia surface, updating only the modified regions.
- **GPU views** (texture, window, Vulkan; decided and done): the upstream Skia compositor of the GTK and WPE ports (coordinated graphics, without TextureMapper), driven by the engine's own compositor thread (`graphics/Compositor`), the single-process equivalent of WebKit2's `LayerTreeHost` and `ThreadedCompositor`. Tiles are painted on the workers, CSS transform and opacity animations produce frames without updating the page rendering, at the pace of the application's frames. WebGL, 2D canvases (drawn on the GPU) and videos (frames as YUV planes converted by the GPU) are their own layers.

## Frame cycle

1. The application calls `zg_view_render()`, or the engine's loop does it in window mode.
2. The engine runs pending tasks, updates style and layout, and redraws with Skia only the modified regions.
3. The result is in the swapchain (window), in the application's texture (texture) or in the pixel buffer (CPU).
4. The completed presentation advances `requestAnimationFrame`, so animations follow vsync. Who sets the pace depends on the mode:
   - **window**: the engine, with its own vsync source (DXGI swapchain with waitable object, CVDisplayLink, EGL swap timing);
   - **texture and CPU**: the application, which calls `zg_view_render(view, timestamp)` once per frame of its own loop. The engine does not estimate the pace on its own, so the UI stays aligned with the game's frame.

   In the port, the rendering update is driven by the embedding layer (its own `DisplayRefreshMonitor`), because with `RunLoopGeneric` the run loop observers are not executed.

## DPI and size

- The view has logical dimensions and a scale factor; the surface has physical dimensions (logical × scale).
- A change of monitor or scale must be propagated to the engine, which reruns layout and rendering.
- In texture mode it is the application that decides dimensions and scale, for example the resolution of a game's UI texture.

## Separate render thread

Many games render on a thread other than the main one, while the engine draws on the thread that owns the runtime. In texture mode an explicit handoff between the two threads is therefore needed:

1. The engine renders into one of its textures, double or triple buffered, on its own thread.
2. When done it signals a GPU fence (D3D11 fence, `VkSemaphore`, `GLsync` or Metal event) and publishes the texture as "ready".
3. The app's render thread acquires the most recent texture, waits for the fence on the GPU (without blocking the CPU) and composes it.
4. On release, the texture returns to the engine for the next frame.

If app and engine use the same GPU device the fence is enough. With different devices, on Windows the texture is shared with an NT handle (`IDXGIResource1::CreateSharedHandle`). The API exposes this cycle as `zg_surface_acquire` / `zg_surface_release`, usable from the render thread.

On Windows the engine always draws with ANGLE into D3D11 textures on ANGLE's device; what changes is the way the game receives them (done for all games' graphics APIs):

| Game API | How it receives the frames | Ordering between the two devices |
| --- | --- | --- |
| D3D11 | The texture opened on its device (NT handle) | Keyed mutex |
| D3D12 | The texture opened on the device of its command queue | Two shared D3D fences: acquire queues a wait for the frame on the game's queue and release queues a signal; the engine waits for that signal on the GPU before redrawing the texture |
| Vulkan (and any API that opens NT handles) | The NT handles of the texture and of the two fences (`zg_surface_win32`), which the game imports (D3D11 external memory, D3D12 timeline semaphores) | The game waits for the ready value and signals the release one |
| Direct3D 9Ex on the same adapter | The texture itself (legacy shared handle, BGRA format) | The engine publishes the frame once drawing is finished; an event query at release tells when the game has finished reading it |
| Direct3D 9 (or 9Ex on another adapter) | A copy of the pixels changed since the last frame, into a game texture (`UpdateTexture`) | The copy is ordered by the D3D9 device itself |

Neither side blocks a CPU thread waiting for the other, except D3D9, where the engine waits for the end of its own drawing on its thread. `zg_runtime_config.adapter_luid` creates ANGLE's device on the game's adapter (a laptop's discrete GPU, for example); a device on another adapter is rejected with the reason, except D3D9 which copies.
