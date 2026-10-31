# Game overlays

An overlay draws a web UI on top of a game: a DLL loaded into the game hooks its
present call, creates a ZeroG runtime and a view on the game's device, and draws
each frame's texture before the game presents. This page covers the engine's
side for every graphics API Windows games use; hooking is your code.

Read [Texture mode](texture-mode.md) first: the loop and input are the same
for every API.

## Before creating the runtime

The runtime is created once, on the thread that will run the engine (often the
game's render thread, from the present hook). Two settings matter for games:

```c
zg_runtime_config config = { sizeof config };
config.adapter_luid = game_adapter_luid;  /* the game's GPU: its textures must open there */
config.gc_threads = 1;                    /* leave the game its cores */
config.jit_threads = 1;
zg_runtime *rt = zg_runtime_create(&config);
```

Each frame, in the present hook: `zg_runtime_update`, `zg_view_render`,
acquire, draw the texture as a full-screen (or positioned) quad with
premultiplied-alpha blending (`ONE`, `INV_SRC_ALPHA`), release, then call the
game's present.

## Direct3D 11

```c
surface.kind = ZG_SURFACE_D3D11;
surface.device = game_device;   /* ID3D11Device* */
```

`frame.texture` is an `ID3D11Texture2D*` on the game's device, already
acquired: create a shader resource view for it (cache it by texture pointer:
there are two or three) and draw. Release when your draw is submitted.

## Direct3D 12

```c
surface.kind = ZG_SURFACE_D3D12;
surface.device = game_queue;    /* ID3D12CommandQueue* the overlay draws on */
```

- `frame.texture` is an `ID3D12Resource*` (`DXGI_FORMAT_R8G8B8A8_UNORM`) opened
  on the queue's device, in state `COMMON`. Create a shader resource view
  once per texture.
- `zg_surface_acquire` queues a `Wait` on your queue for the frame: execute the
  command list that samples it on that same queue, after acquiring.
- `zg_surface_release` queues a `Signal` after it: release after
  `ExecuteCommandLists`. Leave the resource in `COMMON` (or let a read state
  decay to it).
- To sample on another queue, wait yourself for `frame.fence` (an
  `ID3D12Fence*`) at `frame.fence_value` there.

## Vulkan

```c
surface.kind = ZG_SURFACE_VULKAN;
surface.device = NULL;
```

Frames are Win32 handles (`zg_surface_win32`) your Vulkan device imports. The
engine renders on Direct3D 11, so your physical device must be the same GPU:
`VkPhysicalDeviceIDProperties.deviceLUID` equals `zg_win32_frame.adapter_luid`.
Enable `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32` and
timeline semaphores.

```c
zg_win32_frame h = { sizeof h };
zg_surface_win32(surface, &frame, &h);
```

- **Once**, import `h.ready_fence_handle` and `h.release_fence_handle` into two
  timeline semaphores (`vkImportSemaphoreWin32HandleKHR`,
  `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`). They stay the same for
  the surface's life.
- **Once per `h.buffer_id`**, import the texture: a `VkImage` with
  `VkExternalMemoryImageCreateInfo` (`D3D11_TEXTURE_BIT`),
  `VK_FORMAT_R8G8B8A8_UNORM`, optimal tiling, and its memory as a dedicated
  allocation from `h.texture_handle` (`VkImportMemoryWin32HandleInfoKHR`). A
  buffer keeps its handle and ID until the view is resized.
- **Each frame**, submit with a wait on the ready semaphore at
  `h.ready_value`, take the image from `VK_QUEUE_FAMILY_EXTERNAL` in
  `VK_IMAGE_LAYOUT_GENERAL`, sample it, give it back to the external family,
  and signal the release semaphore at `h.release_value`. Then
  `zg_surface_release`.
- Signal `release_value` **always**, even for a frame you did not draw: the
  engine's GPU waits for it before drawing into that texture again.

The handles stay the engine's: importing them into Vulkan does not take them.
The test
[`tests/render/texture_vulkan_win.cpp`](https://github.com/dzonerzy/ZeroG/blob/main/tests/render/texture_vulkan_win.cpp)
is a complete working example.

## Direct3D 9

```c
surface.kind = ZG_SURFACE_D3D9;
surface.device = game_device;   /* IDirect3DDevice9* */
```

`frame.texture` is an `IDirect3DTexture9*` (`D3DFMT_A8R8G8B8`,
`D3DPOOL_DEFAULT`) on the game's device. How it gets there depends on the
device:

- **An `IDirect3DDevice9Ex` on the engine's adapter** opens the engine's
  texture itself. The engine publishes a frame once its drawing is finished,
  and a released frame goes back to the engine once an event query issued at
  release says the game's GPU is done with it.
- **A plain `IDirect3DDevice9`** (or a 9Ex device on another adapter) cannot
  open shared textures, so acquire copies the pixels that changed since the
  previous frame into a texture of the game's (through a system-memory texture
  and `UpdateTexture`). Only damaged pixels cross the bus; for a UI that
  changes a little each frame that is cheap.

Direct3D 9 devices are single-threaded: acquire, release and
`zg_surface_device_lost` run on the device's thread, which for an overlay is
the game's render thread anyway.

Before `IDirect3DDevice9::Reset` (a lost device, a resolution change), call
`zg_surface_device_lost(surface)`: the engine releases its objects on the
device, as Reset requires, and recreates them at the next acquire.

Create your own D3D9 device with `D3DCREATE_FPU_PRESERVE` if you make one:
otherwise Direct3D 9 lowers the thread's floating-point precision, which
JavaScript notices. Games' devices are what they are.

## OpenGL games

Old OpenGL games on Windows can import the Win32 handles with
`GL_EXT_memory_object_win32` and `GL_EXT_semaphore_win32`, the same way as
Vulkan (`ZG_SURFACE_VULKAN`). On Linux use `ZG_SURFACE_GL` with the game's
`EGLDisplay`, or `ZG_SURFACE_VULKAN` for dma-bufs.

## Input

Hand the game's mouse and keyboard to the view when the overlay has focus (and
only then): see [Texture mode](texture-mode.md#input).
