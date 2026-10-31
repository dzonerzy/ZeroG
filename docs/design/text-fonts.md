# Text and fonts

To have identical text rendering everywhere, the engine uses the same stack on all platforms and allows **embedding fonts in the binary**. System fonts remain available as an option.

## Stack

| Component | Role | License |
| --- | --- | --- |
| FreeType | Glyph rasterization | FTL or GPLv2, at choice |
| HarfBuzz | Text shaping | MIT |
| Skia font manager | Embedded fonts or fonts read from directories and fallback, instead of Fontconfig | BSD 3-Clause (part of Skia) |
| ICU | Unicode, segmentation, bidi, collation | Unicode License |

On Windows and macOS it must be decided whether to use the same FreeType stack for visual consistency, or Skia's native font manager (DirectWrite, CoreText) for integration with the system. The proposed default is FreeType everywhere, with native as an option.

## Embedded fonts

- A minimal default set included in the binary: a sans, a serif, a monospace and a broad fallback font for non-Latin scripts.
- API to register fonts from the application (`zg_context_register_font` from memory), so the frontend's assets do not depend on the system.
- `@font-face` keeps working normally, also with the custom asset scheme (see [Networking, storage and resources](networking-storage-resources.md)).

## ICU and binary size

The full ICU data weighs tens of MB and dominates the size of a static binary. The build system uses the ICU data filter to include only the necessary languages and features, with a "minimal" and a "full" profile selectable at compile time. All profiles lack the data of the ICU services that the engine never calls (transliteration, character names, anti-spoofing checks, StringPrep, NFKC_Casefold normalizers): 1.8 MB less.

## Binary size

[`tools/size.py`](https://github.com/dzonerzy/ZeroG/blob/main/tools/size.py) measures the example app (`ZeroGHello`, which links the whole engine) as an application would ship it, without symbols: size, sections and, on Windows, how much each library contributes (from the linker map). Outside the debug profile the build reduces the binary without removing features:

- every function and every datum in its own section (`/Gw` on Windows, `-ffunction-sections -fdata-sections` on Linux, also in the vcpkg dependencies), so an application's link discards what it does not reach (`/OPT:REF`, `--gc-sections`);
- on Windows safe identical code folding (`/OPT:SAFEICF`): it folds only the functions whose address nobody takes, so comparisons between function pointers stay true (WebKit turns it off entirely for this reason). zig does not pass `--icf` to the linker, on Linux no folding;
- in the minimal and standard profiles no JIT disassembler (Zydis) nor sampling profiler, which serve only to work on the engine and stay where the Web Inspector is;
- SDL3 without the subsystems that ZeroG does not use (GPU API, camera, joystick, haptic, HIDAPI, power, sensors) and without its dynamic API, which with the `SDL_DYNAMIC_API` variable would allow replacing SDL at runtime and, with its table of all functions, would prevent the linker from discarding any of them.

Measurements of `ZeroGHello` without symbols:

| Profile | Windows before | Windows | Linux |
| --- | --- | --- | --- |
| minimal | 104.4 MB | 93.1 MB | 90.2 MB |
| standard | — | 123.0 MB | 124.1 MB |
| dev | 137.8 MB | 125.5 MB | 126.6 MB |

In minimal on Windows: WebCore 39.4 MB, JavaScriptCore 18.7, ICU data 11.4, ANGLE 4.0, Skia 3.6, the rest under 1.6 MB per library. In standard the full ICU data weighs 26.5 MB. Compressed for download (xz) minimal weighs 26.5 MB. UPX 5.3 (`--best --lzma`) brings the executable itself to 25.8 MB (standard 32.7, dev 34.4) and it runs, but every launch pays for it: startup goes from about 0.25 s to 1.4–1.7 s, and the decompressed code is private memory instead of pages shared with the file (minimal's working set at rest from 90 to 162 MB, private bytes from 112 to 204 MB). Compressing the download instead costs nothing at run time. (UPX 4.1 produced binaries that crashed at startup.)

Still possible, at a cost: compiling for size (`/O1`, `-Os`) instead of speed, removing the DFG JIT from minimal (3-4 MB), reducing the ICU data languages in standard. WebCore in minimal already has turned off all the features that can be turned off from CMake; the rest is the engine itself or would require upstream patches for a few MB.
