# Memory

## At rest

Measured with `tools/memory.py`: each scenario in its own process, after its
page has loaded, rendered and settled and the JavaScript heap has been
collected. Windows, minimal profile, NVIDIA GeForce RTX 4080; median of three
runs.

| Scenario | Private bytes | Working set |
| --- | --- | --- |
| A runtime and a context, no view | 29.8 MB | 26.5 MB |
| An empty view on a CPU surface | 31.7 MB | 33.4 MB |
| An empty view on a GPU surface (D3D11) | 111.5 MB | 75.9 MB |
| An empty window | 111.3 MB | 81.8 MB |
| A heavier page (2000 styled elements, script state), CPU | 56.4 MB | 59.2 MB |

Linux, minimal profile, in the Docker builder (Mesa's llvmpipe, a software
renderer, stands in for the GPU); median of three runs:

| Scenario | Private | RSS | PSS |
| --- | --- | --- | --- |
| A runtime and a context, no view | 5.1 MB | 25.9 MB | 15.0 MB |
| An empty view on a CPU surface | 38.4 MB | 40.4 MB | 39.0 MB |
| An empty view on a GPU surface (dma-buf, llvmpipe) | 154.5 MB | 156.9 MB | 155.3 MB |
| A heavier page (2000 styled elements, script state), CPU | 68.1 MB | 70.9 MB | 69.1 MB |

The standard and dev profiles are within 3 MB of these.

What the numbers say:

- The engine itself, with a page, is about 30 MB. A view costs little; a
  page's DOM, style and layout cost what they cost in any browser.
- **The GPU is most of a GPU view's memory.** The step from 32 to 111 MB on
  Windows is the Direct3D 11 device and the graphics driver's own allocations
  in the process (shader caches, command buffers, heaps), which every Direct3D
  application pays once per device. On Linux under llvmpipe the driver is a
  whole software renderer, hence the larger step there. A game overlay shares the game's process,
  where that cost is the game's anyway; the engine's own share stays small.
- Windows and texture views are the same: the window adds a swap chain.

Measure your own build, per profile:

```sh
python tools/memory.py --profile minimal --profile standard
```

## Comparing numbers

Memory numbers are only comparable when they count the same thing. Engines that
ship as shared libraries often quote their heap, or a process's private memory
without the library code, which is shared with the files; numbers for a
CPU-rendered view leave out the GPU driver, which is the largest single item
above. The working set includes the pages of the executable's code that were
touched, which a statically linked engine counts as its own.

## Limits

- `zg_runtime_config.js_heap_limit` is a soft limit for the JavaScript heap:
  near it the garbage collector works harder.
- `zg_runtime_config.cache_limit` caps WebCore's memory cache of decoded
  resources.
- `zg_runtime_memory_stats` reports the JavaScript heap (live at the last
  collection, and reserved), the resource cache, the engine's surfaces and the
  files it holds; `zg_runtime_collect_garbage` collects now.
