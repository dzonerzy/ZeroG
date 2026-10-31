# Profiles and binary size

## Profiles

The engine is built in one of four profiles (`tools/build.py --profile`). The
API is the same in all of them; features a profile leaves out fail with
`ZG_ERROR_UNSUPPORTED` or are absent from pages (`window.caches`,
`navigator.serviceWorker`, `MediaSource`...).

| | minimal | standard | dev | debug |
| --- | --- | --- | --- | --- |
| HTML, CSS, JavaScript, SVG, canvas, storage, networking | Yes | Yes | Yes | Yes |
| JIT tiers | interpreter, Baseline, DFG | + FTL | + FTL | + FTL |
| WebAssembly, WebGL | No | Yes | Yes | Yes |
| `<audio>`, `<video>`, MSE, Web Audio, AVIF | No | Yes | Yes | Yes |
| Service workers, Cache API | No | Yes | Yes | Yes |
| ICU data (Intl locales) | 17 languages | all | all | all |
| Web Inspector, `dev_server_url` | No | No | Yes | Yes |
| Build | release | release | release | debug, assertions, full symbols |

`minimal` is for application UI, where the smallest binary matters;
`standard` for the full web platform; `dev` for developing an application
(the same engine as standard plus the inspector); `debug` for working on the
engine itself.

## Binary size

What the engine adds to an application, measured on the example app
(`ZeroGHello`, which links the whole engine) without symbols:

| Profile | Windows | Linux |
| --- | --- | --- |
| minimal | 93.1 MB | 90.2 MB |
| standard | 123.0 MB | 124.1 MB |
| dev | 125.5 MB | 126.6 MB |

Measure your own build with:

```sh
python tools/size.py --profile minimal --top 20
```

which also breaks the size down by library on Windows (from the linker's map).
In minimal: WebCore 39 MB, JavaScriptCore 19 MB, ICU data 11 MB, ANGLE 4 MB,
Skia 4 MB, everything else under 1.6 MB per library. In standard the full ICU
data is 27 MB of the difference.

### How the build keeps it down

- Every function and datum in a section of its own, in the engine and every
  dependency, so the application's link drops what nothing reaches; safe
  identical code folding on Windows.
- ICU data filtered: only the profile's languages, and none of the data of
  ICU services the engine never calls.
- No JIT disassembler or sampling profiler outside dev and debug.
- No unused SDL subsystems, and no SDL dynamic API.

### Shipping smaller

The executable compresses well: minimal is 26.5 MB as `.xz`. Compress your
installer or download rather than the executable: executable packers such as
UPX get a similar size but cost every launch (startup goes from about 0.25 s to
1.5 s) and memory (the code becomes private memory instead of pages shared with
the file: minimal's working set doubles), and antivirus software distrusts
them.
