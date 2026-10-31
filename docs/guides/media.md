# Media

`<audio>`, `<video>`, Media Source Extensions and Web Audio are in the
standard, dev and debug profiles. The minimal profile has the elements, which
report that they cannot play, and no Web Audio or `MediaSource`.

## Formats

The engine decodes with separate, royalty-free libraries; no patented codec is
in the binary.

| Container | Codecs | Where |
| --- | --- | --- |
| WebM | VP8, VP9, AV1, Opus, Vorbis | everywhere |
| Ogg | Opus, Vorbis | everywhere |
| MP3, FLAC, WAV | | everywhere |
| MP4 | VP9, AV1, Opus | everywhere |
| MP4 | H.264, AAC | Windows, through Media Foundation (not in "N" editions); GPU decoding where available |
| AVIF images | AV1 | everywhere |

`canPlayType` and `MediaSource.isTypeSupported` answer accordingly.

## Media Source Extensions

`MediaSource` and `SourceBuffer` for WebM and fragmented MP4, several buffers
per source, seeking within what is buffered: streaming players (YouTube, hls.js)
work.

## Sound

Sound goes to the system's default output device (WASAPI on Windows; ALSA,
PulseAudio or PipeWire on Linux, loaded at run time). Web Audio's
`AudioContext` plays there; `OfflineAudioContext`, `AnalyserNode`,
`ConvolverNode`, `PannerNode` with HRTF and `createMediaElementSource` work.

## Rendering

In GPU views a video is a layer of its own: frames reach the compositor as YUV
planes the GPU converts, without repainting the page. `<video controls>` shows
WebKit's native controls.
