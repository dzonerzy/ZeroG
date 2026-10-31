# Licenses and LGPL compliance

Static linking is allowed also for proprietary applications, but with a precise obligation: the end user must be able to **relink the application with a modified version of the LGPL parts**. The engine must make this obligation easy to meet, otherwise nobody will use it in commercial products.

This section describes the technical design, it is not legal advice. Before the first release a review must be done by a lawyer experienced in open source licenses.

## Component licenses

| Component | License |
| --- | --- |
| WebCore, JavaScriptCore | [LGPL](https://en.wikipedia.org/wiki/WebKit) (v2 "or later", about 200 files in JSC) |
| WTF | Mixed: mostly BSD 2-Clause, but about 100 files are LGPL v2 "or later", including central ones (`DateMath`, `dtoa`, `FastMalloc.h`, `Forward.h`). For the purposes of the relink obligation it must be treated as LGPL |
| bmalloc, rest of WebKit | BSD 2-Clause |
| Skia, ANGLE | BSD 3-Clause |
| HarfBuzz, nghttp2, libpsl, libxml2, brotli, woff2 | MIT |
| FreeType | FTL (chosen instead of GPLv2, requires a note in the credits) |
| ICU | Unicode License |
| curl | curl license (MIT style) |
| BoringSSL | Permissive licenses, to be verified on the revision used |
| zlib | zlib license |
| libpng | libpng license |
| libjpeg-turbo | BSD and IJG license |
| libwebp | BSD 3-Clause |
| SQLite | Public domain |
| SDL3 (window mode, media audio) | zlib license |
| Media (standard, dev, debug): libwebm, libvpx, libopus, libogg, opusfile, libvorbis, libyuv | BSD 3-Clause (libvpx also ISC) |
| Media: dav1d | BSD 2-Clause |
| Media: dr_libs (dr_mp3, dr_flac, dr_wav) | Public domain (Unlicense) or MIT-0 |
| Web Audio (standard, dev, debug): pffft | FFTPACK license (BSD style) |
| AVIF (standard, dev, debug): libavif | BSD 2-Clause (parts Apache 2.0) |
| Engine code | To be decided: proposal BSD 2-Clause, consistent with WebKit |

The embedding layer inherits the license of the code it may derive from: BSD if it takes parts of WebKitLegacy, LGPL if it takes parts of Ultralight's WebCore. It must be tracked file by file from the start ([`docs/PROVENANCE.md`](../PROVENANCE.md)). In any case it links to WTF, JSC and WebCore, so the relink obligation covers the whole engine archive: the relink kit remains the right tool, with the same design.

## What the engine does for compliance

- **Relink kit**: a `zig build relink-kit` target that produces an archive with the application's object files, the link script and the instructions. With this kit and the engine's source, anyone can relink the app with a modified WebKit.
- **Dynamic profile**: alternatively, the application can link the engine as a dynamic library, and the obligation reduces to distributing the replaceable library.
- **Automatic notices**: the build generates the third-party license file with all the components actually included, to be attached to the distribution.
- **Corresponding source**: every engine release publishes the exact source of WebKit and of the patches used, with tags aligned to the binary releases.

## Open source applications

For applications with a license compatible with the LGPL (GPL, LGPL, MPL and similar) the obligations are already met by publishing the source, and the relink kit is not needed.
