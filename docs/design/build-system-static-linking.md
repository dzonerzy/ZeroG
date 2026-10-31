# Build system and static linking

"Single binary" has a precise boundary: everything is linked statically **except libc and the GPU driver loaders**. Drivers must be loaded from the system with `dlopen` of `libEGL` or `libvulkan` on Linux, and the same goes for system DLLs on Windows and frameworks on macOS. It is the same compromise games make.

Without GLib, libsoup and GStreamer the modules loaded at runtime also disappear, which were the main obstacle to static linking.

## Structure

- **WebKit** (WTF, JavaScriptCore, WebCore) is built with WebKit's CMake and the project's `PORT`, with clang on all platforms (clang-cl on Windows), producing static archives.
- **Dependencies** C and C++ built statically in a sysroot per target, from a reproducible pipeline.
- **Platform layer, embedding layer and C API** are built in the same CMake as the port.
- **zig build** orchestrates everything: it launches CMake as an external step, collects the archives, generates the asset archive and does the final link of the application.
- **C++ runtime** static, so as not to depend on the version installed on the system.

## Media

WebCore needs a playback backend for `<video>`, `<audio>` and Web Audio, and in the upstream ports it is GStreamer or AVFoundation. Its own port must provide one.

- **Minimal profile**: no media backend, hence no playback. `ENABLE_VIDEO` stays on without a backend: no upstream port builds with video off and WebCore compiles the media code without guards, so turning it off would mean a growing series of patches. The `<video>` and `<audio>` elements exist and report that they cannot play. `ENABLE_WEB_AUDIO` and `ENABLE_MEDIA_SOURCE` are off (combination already used by the PlayStation port).
- **Standard, dev and debug profiles** (done): in the standard, dev and debug profiles its own engine ([`port/embedding/media`](https://github.com/dzonerzy/ZeroG/blob/main/port/embedding/media)) on separate libraries, all royalty-free and with permissive licenses, instead of FFmpeg: libwebm for WebM, libvpx (VP8, VP9), dav1d (AV1), libopus, libogg/opusfile/libvorbis for Ogg, dr_libs for MP3, FLAC and WAV, libyuv for frame conversion. No patented codec in the binary: H.264 and AAC in MP4 go through the operating system where available (Media Foundation on Windows, with the system's decoders; on Linux they are not played). MP4 files with VP9, AV1 and Opus play everywhere with the engine's decoders (the moov sample tables, or the moofs of a fragmented file). The native controls are WebKit's modern media controls with the Adwaita icons compiled into the engine (patch 0033). Media Source Extensions for WebM and fragmented MP4: WebCore's MediaSource and SourceBuffer hold the frames, the port parses the byte stream (WebM with libwebm's callback parser, MP4 with its own box parser) and plays the queued frames with the same decoders; H.264 and AAC from MSE go through Media Foundation's decoders on Windows (patch 0031). In composited views the video is its own layer: frames go to the compositor without repainting the page (patch 0032), as YUV planes that the GPU converts in a shader (no conversion on the CPU). On Windows H.264 is decoded on the GPU (DXVA, with a D3D11 device shared by Media Foundation's decoders) where the GPU allows it. In GPU views 2D canvases also draw on the GPU (Ganesh) as their own layers. Audio goes out with SDL3 (WASAPI; ALSA, PulseAudio, PipeWire loaded at runtime). Web Audio uses the same engine: the destination is an SDL3 stream, `decodeAudioData` goes through the media decoders, the FFT is pffft (patch 0030), WebKit's HRTF responses are compiled into the engine. The minimal profile stays without the engine and without Web Audio.

## Build profiles

- **minimal**: no media, no WebGL, reduced ICU data. The goal is the smallest possible binary for application UI.
- **standard**: adds WebGL, WebAssembly, the FTL JIT, the media engine, Web Audio and Media Source Extensions.
- **dev**: engine in release with the Web Inspector enabled. It is the profile of `zerog dev`, for developing the application.
- **debug**: full symbols and assertions enabled (WebKit Debug build). For working on the engine itself; slower and much larger.

## Times and cache

A full WebKit build takes hours. The CI uses an artifact cache per revision and configuration; changes to the project's layers alone recompile only those and the final link.
