# Measurements

The numbers ZeroG tracks (the [measurable goals](../design/measurable-goals.md)),
with how to reproduce each.

## Binary size

The example app without symbols, `python tools/size.py`. See
[Profiles and binary size](../guides/profiles-and-size.md).

| Profile | Windows | Linux |
| --- | --- | --- |
| minimal | 93.1 MB | 90.2 MB |
| standard | 123.0 MB | 124.1 MB |
| dev | 125.5 MB | 126.6 MB |

## Memory at rest

`python tools/memory.py`. See [Memory](../guides/memory.md) for the scenarios
and what they mean.

| Scenario (Windows, minimal) | Private bytes | Working set |
| --- | --- | --- |
| Runtime and context, no view | 29.8 MB | 26.5 MB |
| Empty CPU view | 31.7 MB | 33.4 MB |
| Empty GPU view (D3D11) | 111.5 MB | 75.9 MB |
| Empty window | 111.3 MB | 81.8 MB |
| Heavier page, CPU | 56.4 MB | 59.2 MB |

| Scenario (Linux, minimal, Docker with llvmpipe) | Private | RSS | PSS |
| --- | --- | --- | --- |
| Runtime and context, no view | 5.1 MB | 25.9 MB | 15.0 MB |
| Empty CPU view | 38.4 MB | 40.4 MB | 39.0 MB |
| Empty GPU view (dma-buf, llvmpipe) | 154.5 MB | 156.9 MB | 155.3 MB |
| Heavier page, CPU | 68.1 MB | 70.9 MB | 69.1 MB |

## Binding latency

`ZeroGBindingBench`, a C program on the public API: a page times its own
loops of calls, each warmed up first. AMD Ryzen 9 9950X3D, Windows 11 natively
and Linux in the Docker builder on WSL 2, minimal profile.

| Case | What is timed | Windows x64 | Linux x86_64 |
| --- | --- | --- | --- |
| `js` | an empty JavaScript function, for comparison | 0.005 µs | 0.005 µs |
| `sync` | `App.noop()`: an empty synchronous binding | 0.03 µs | 0.025 µs |
| `async` | `await App.later()`, its Promise resolved during the call | 0.55 µs | 0.50 µs |
| `thread` | `await App.thread()`, its Promise resolved by a worker thread | 10.5 µs | 54.8 µs |
| `os` | the same two thread switches, without the engine | 6.2 µs | 47.8 µs |

The engine's share of a Promise resolved from another thread is the difference
between `thread` and `os`: the rest is the operating system switching threads.
