# Texture mode

In texture mode the engine renders into GPU textures and your application
composites them: a game's UI, an overlay, a panel in a 3D scene. Your loop
drives the engine; the engine never draws to the screen.

## The loop

```c
zg_surface_config surface = { sizeof surface };
surface.kind = ZG_SURFACE_D3D11;     /* or D3D12, D3D9, VULKAN, GL */
surface.device = my_d3d11_device;    /* what the kind takes: see below */

zg_view_config config = { sizeof config };
config.width = 1280;                 /* logical pixels */
config.height = 720;
config.scale = 1.5f;                 /* physical pixels = logical × scale */
config.transparent = 1;              /* an overlay: no white background */
config.surface = &surface;
zg_view *view = zg_view_create(ctx, &config);

/* Every frame of your loop, on the runtime thread: */
zg_runtime_update(rt);
zg_view_render(view, now_ms);

/* On your render thread (any thread): */
zg_frame frame;
if (zg_surface_acquire(zg_view_surface(view), &frame) == ZG_OK) {
    composite(frame.texture, frame.width, frame.height);
    zg_surface_release(zg_view_surface(view), &frame);
}
```

- `zg_view_render` advances `requestAnimationFrame`, CSS animations and
  layout to your frame time. A GPU view composites on the engine's compositor
  thread, so the frame reaches `zg_surface_acquire` shortly after the call
  returns. Animations move with your frames and pause with your clock;
  `zg_view_set_frame_rate` caps them (a game at 144 fps can keep its UI at
  30).
- `zg_surface_acquire` gives the newest frame, or `ZG_ERROR_NOT_FOUND` if
  there is none newer than the last. The engine never waits for you: frames you
  do not take are replaced by newer ones.
- Frames come with the region that changed since the previous one you took
  (`dirty_x`, `dirty_y`, `dirty_w`, `dirty_h`), and rows run top to bottom in
  every kind.
- Frames are premultiplied alpha.

## Input

Your window gets the input; hand it to the view in its logical pixels:
`zg_view_mouse_event`, `zg_view_scroll_event`, `zg_view_key_event` (DOM `key`
and `code` strings, then `ZG_KEY_CHAR` with the text), the IME calls for
composition, `zg_view_drag` for drops, and `zg_view_set_focus`.
`cursor_changed` and `ime_caret_rect` tell you what to show.

## Surface kinds

| Kind | Platform | `device` | A frame is |
| --- | --- | --- | --- |
| `ZG_SURFACE_CPU` | all | (no surface config) | BGRA pixels in memory |
| `ZG_SURFACE_D3D11` | Windows | your `ID3D11Device`, or NULL for one the engine creates (`zg_surface_device`) | an `ID3D11Texture2D` opened on it, already acquired through its keyed mutex |
| `ZG_SURFACE_D3D12` | Windows | your `ID3D12CommandQueue` | an `ID3D12Resource`; acquire queues the wait on your queue, release the signal |
| `ZG_SURFACE_D3D9` | Windows | your `IDirect3DDevice9` (or 9Ex) | an `IDirect3DTexture9` |
| `ZG_SURFACE_VULKAN` | Windows | NULL | NT handles of a texture and two timeline fences (`zg_surface_win32`) |
| `ZG_SURFACE_VULKAN` | Linux | NULL | a dma-buf with its sync_file (`zg_surface_dmabuf`) |
| `ZG_SURFACE_GL` | Linux | your `EGLDisplay` | an `EGLImage` and an `EGLSync` |

[Game overlays](game-overlays.md) walks through each Windows kind.

## Synchronization

Neither side blocks a CPU thread waiting for the other's GPU:

- **D3D11**: a keyed mutex on each shared texture, taken in acquire and given
  back in release.
- **D3D12**: two shared fences. Acquire queues `Wait` for the frame on your
  queue; release queues `Signal`; the engine's GPU waits for that signal before
  drawing into the texture again.
- **Vulkan (Windows)**: the same fences as handles. You wait for `ready_value`
  and signal `release_value`.
- **Vulkan (Linux)**: the dma-buf's sync_file to wait on, and
  `zg_surface_release_with_fence` to hand back yours.
- **GL (Linux)**: an `EGLSync` to `eglWaitSync` on; release with the context
  that used the frame current.
- **D3D9**: see [Game overlays](game-overlays.md#direct3d-9).

## The GPU adapter

On Windows the engine renders with ANGLE over Direct3D 11 on one adapter, and
devices on another adapter cannot open its textures. On a laptop with two GPUs
the game may run on the discrete one: create the runtime with
`zg_runtime_config.adapter_luid` set to the game's adapter
(`DXGI_ADAPTER_DESC.AdapterLuid`, `ID3D12Device::GetAdapterLuid`,
`IDirect3D9Ex::GetAdapterLUID`). A device on another adapter is refused with
`ZG_ERROR_UNSUPPORTED` and the reason, except plain Direct3D 9, which copies.

## Threads

`zg_view_render` and everything else run on the runtime thread;
`zg_surface_acquire` and `zg_surface_release` on any thread, typically your
render thread. A Direct3D 9 surface takes them on its device's thread.
