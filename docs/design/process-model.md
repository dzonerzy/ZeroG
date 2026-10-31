# Process model

The engine is **single-process**: WebCore, JavaScriptCore and the application share the same process and the same main thread. It is the choice that makes static linking simple (no subprocess to launch, no IPC) and allows direct JS bindings.

## Threads

| Thread | Role | Owner |
| --- | --- | --- |
| Main | DOM, layout, JavaScript, events, API callbacks | The application: it is the thread that creates the runtime |
| Network | curl requests in parallel (curl multi) | Engine |
| JSC | Concurrent garbage collector, JIT compilation | JavaScriptCore |
| Decoding | Decoding of images and fonts | Engine, worker pool |
| Painting (optional) | Skia rasterization of tiles in parallel | Engine |

The main constraint is WebCore's: everything that touches the DOM and JavaScript happens on a single thread. For this reason the public API is usable only from the thread that created the runtime.

## Integration with the application loop

The application drives the engine, as in Ultralight:

- **Window mode**: `zg_runtime_run()` owns the loop and handles events, timers and rendering.
- **Game mode or own loop**: the application calls `zg_runtime_update()` at every tick (timers, network responses, pending tasks) and `zg_view_render()` when it wants a frame.
- **Wakeup**: `zg_runtime_wakeup_handle()` returns a handle to wait on when the application is idle, signaled when the engine has work to do (network response, expiring timer).

## Consequences to accept

- **No crash isolation**: a crash in WebCore or JSC terminates the application.
- **No sandbox for web content**: WebCore's code runs with the application's privileges (see [Security](security.md)).
- **Shared memory**: JSC's heap and WebCore's caches count toward the application's memory. The engine exposes configurable limits for caches and heap.

* **A single JavaScript VM**: all views share JSC's heap and garbage collector. Memory limits can only be applied to the whole runtime, not to the single view.
