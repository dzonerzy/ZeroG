#!/usr/bin/env python3
"""THIRD_PARTY_NOTICES.txt: the license texts of everything the engine bundle
contains, which their licenses require to be distributed with the binaries.

Each component of the bundle (tools/build.py bundle) is listed below with the
profiles that contain it and where its license text is: WebKit's tree for
WebKit and the code it vendors, the vcpkg sysroot's copyright files for the
dependencies. The output covers every profile; each section says which ones
contain the component.

    python tools/notices.py [-o THIRD_PARTY_NOTICES.txt]

Needs the WebKit checkout (tools/webkit.py fetch) and the sysroot
(tools/build.py deps).
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEBKIT = ROOT / "third_party" / "webkit" / "Source"
IS_WINDOWS = sys.platform == "win32"
SHARE = ROOT / "third_party" / "sysroot" / ("x64-windows-zerog" if IS_WINDOWS else "x64-linux-zerog") / "share"

ALL = "all profiles"
FULL = "standard, dev and debug profiles"
INSPECTOR = "dev and debug profiles"

# (component, what it does in ZeroG, profiles, license files)
COMPONENTS = [
    ("WebKit: WebCore, JavaScriptCore, WTF, PAL, bmalloc", "the web engine", ALL,
     [WEBKIT / "WebCore" / "LICENSE-LGPL-2", WEBKIT / "WebCore" / "LICENSE-LGPL-2.1",
      WEBKIT / "WebCore" / "LICENSE-APPLE", WEBKIT / "JavaScriptCore" / "COPYING.LIB"]),
    ("double-conversion (WTF/dtoa)", "number formatting and parsing in WTF", ALL,
     [WEBKIT / "WTF" / "wtf" / "dtoa" / "LICENSE"]),
    ("fast_float (WTF)", "number parsing in WTF", ALL,
     [WEBKIT / "WTF" / "wtf" / "fast_float" / "LICENSE"]),
    ("SIMDe (WTF)", "portable SIMD in WTF", ALL,
     [WEBKIT / "WTF" / "LICENSE-simde.txt"]),
    ("simdutf (WTF)", "Unicode validation and transcoding in WTF", ALL,
     [WEBKIT / "WTF" / "wtf" / "simdutf" / "LICENSE-simdutf.txt"]),
    ("zmij (WTF)", "floating-point formatting in WTF", ALL,
     [WEBKIT / "WTF" / "LICENSE-zmij.txt"]),
    ("mimalloc", "the engine's memory allocator", ALL,
     [WEBKIT / "bmalloc" / "mimalloc" / "mimalloc" / "LICENSE"]),
    ("Zydis and Zycore", "JavaScriptCore's disassembler of generated code", INSPECTOR,
     [WEBKIT / "JavaScriptCore" / "disassembler" / "zydis" / "LICENSE-zydis.txt",
      WEBKIT / "JavaScriptCore" / "disassembler" / "zydis" / "LICENSE-zycore.txt"]),
    ("ANGLE", "OpenGL ES over Direct3D 11 on Windows; WebGL", ALL,
     [WEBKIT / "ThirdParty" / "ANGLE" / "LICENSE"]),
    ("Skia", "2D graphics", ALL,
     [WEBKIT / "ThirdParty" / "skia" / "LICENSE"]),
    ("LLVM runtime libraries (compiler-rt builtins; libc++, libc++abi and libunwind on Linux)",
     "compiler support routines and, on Linux, the C++ runtime", ALL,
     [WEBKIT / "WTF" / "LICENSE-LLVM.txt"]),
    ("ICU", "Unicode, locales and Intl", ALL, [SHARE / "icu" / "copyright"]),
    ("curl", "networking", ALL, [SHARE / "curl" / "copyright"]),
    ("BoringSSL", "TLS and cryptography", ALL, [SHARE / "openssl" / "copyright"]),
    ("nghttp2", "HTTP/2", ALL, [SHARE / "nghttp2" / "copyright"]),
    ("libpsl", "the public suffix list", ALL, [SHARE / "libpsl" / "copyright"]),
    ("zlib", "compression", ALL, [SHARE / "zlib" / "copyright"]),
    ("Zstandard", "compression", ALL, [SHARE / "zstd" / "copyright"]),
    ("Brotli", "compression, WOFF2 fonts", ALL, [SHARE / "brotli" / "copyright"]),
    ("WOFF2", "web fonts", ALL, [SHARE / "woff2" / "copyright"]),
    ("FreeType", "font rendering", ALL, [SHARE / "freetype" / "copyright"]),
    ("HarfBuzz", "text shaping", ALL, [SHARE / "harfbuzz" / "copyright"]),
    ("libpng", "PNG images", ALL, [SHARE / "libpng" / "copyright"]),
    ("libjpeg-turbo", "JPEG images", ALL, [SHARE / "libjpeg-turbo" / "copyright"]),
    ("libwebp", "WebP images", ALL, [SHARE / "libwebp" / "copyright"]),
    ("libxml2", "XML and XHTML documents", ALL, [SHARE / "libxml2" / "copyright"]),
    ("SQLite", "cookies, storage and caches", ALL, [SHARE / "sqlite3" / "copyright"]),
    ("SDL", "windows, input and sound", ALL, [SHARE / "sdl3" / "copyright"]),
    ("libavif", "AVIF images", FULL, [SHARE / "libavif" / "copyright"]),
    ("dav1d", "AV1 video and AVIF images", FULL, [SHARE / "dav1d" / "copyright"]),
    ("libvpx", "VP8 and VP9 video", FULL, [SHARE / "libvpx" / "copyright"]),
    ("libwebm", "WebM containers", FULL, [SHARE / "libwebm" / "copyright"]),
    ("libogg", "Ogg containers", FULL, [SHARE / "libogg" / "copyright"]),
    ("Opus", "Opus audio", FULL, [SHARE / "opus" / "copyright"]),
    ("opusfile", "Ogg Opus files", FULL, [SHARE / "opusfile" / "copyright"]),
    ("libvorbis", "Vorbis audio", FULL, [SHARE / "libvorbis" / "copyright"]),
    ("dr_libs", "MP3, FLAC and WAV audio", FULL, [SHARE / "drlibs" / "copyright"]),
    ("libyuv", "video frame conversion", FULL, [SHARE / "libyuv" / "copyright"]),
    ("PFFFT", "Web Audio's FFT", FULL, [SHARE / "pffft" / "copyright"]),
]

RULE = "=" * 78


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-o", "--output", default=str(ROOT / "THIRD_PARTY_NOTICES.txt"))
    args = parser.parse_args()

    missing = [str(f) for _, _, _, files in COMPONENTS for f in files if not f.exists()]
    if missing:
        sys.exit("license files not found (fetch WebKit and build the sysroot first):\n  " + "\n  ".join(missing))

    out = [
        "Third-party notices for the ZeroG engine",
        "",
        "Applications that link ZeroG's engine bundle (zerog-engine.lib or",
        "libzerog-engine.a) contain the components below. Their licenses require these",
        "notices to be distributed with the application. ZeroG's own code is under the",
        "BSD 2-Clause license (LICENSE).",
        "",
        "WebCore and JavaScriptCore, and parts of WTF, are under the GNU Lesser General",
        "Public License: an application that links them statically must let its users",
        "relink it with a modified version of them. ZeroG's source, at the version an",
        "application uses, is at https://github.com/dzonerzy/ZeroG; see the guide",
        "\"Licensing and relinking\" (https://dzonerzy.github.io/ZeroG/guides/licensing/)",
        "and tools/relink_kit.py.",
        "",
        "Components:",
        "",
    ]
    for name, what, profiles, _ in COMPONENTS:
        out.append(f"  - {name}: {what} ({profiles})")
    for name, what, profiles, files in COMPONENTS:
        out += ["", RULE, f"{name}", f"{what}; in the {profiles}" if profiles != ALL else f"{what}; in all profiles", RULE, ""]
        for file in files:
            text = file.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").strip()
            out += [text, ""]
    Path(args.output).write_text("\n".join(out).rstrip() + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.output}: {len(COMPONENTS)} components")


if __name__ == "__main__":
    main()
