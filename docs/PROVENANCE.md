# Provenance of port files

The embedding layer inherits the license of any code it is derived from
(ZeroG.md, "Licenze dei componenti"). Every file under `port/` and every patch
under `patches/` is listed here when it is created.

| File | Origin | License |
| --- | --- | --- |
| `include/zerog.h` | Original | BSD-2-Clause |
| `tools/webkit.py` | Original | BSD-2-Clause |
| `tools/build.py` | Original | BSD-2-Clause |
| `patches/0001-cmake-forwarding-headers-instead-of-symlinks-on-windows.patch` | Original, modifies `Source/cmake/WebKitMacros.cmake` (BSD) | BSD-2-Clause; upstream candidate |
| `patches/0002-add-zerog-port-and-platform-macro.patch` | Original, modifies `WebKitCommon.cmake`, `PlatformLegacy.h`, `BPlatform.h` (BSD) | BSD-2-Clause |
| `port/overlay/Source/cmake/OptionsZeroG.cmake` | Derived from upstream `OptionsJSCOnly.cmake` (BSD) | BSD-2-Clause |
| `port/overlay/Source/{WTF/wtf,bmalloc,JavaScriptCore}/PlatformZeroG.cmake` | Original (includes upstream `PlatformJSCOnly.cmake`) | BSD-2-Clause |
| `tests/link/jsc_c_api.c` | Original | BSD-2-Clause |
| `tests/link/icu_converters.c` | Original | BSD-2-Clause |
| `tools/icu_data.py`, `deps/icu-data.json` | Original | BSD-2-Clause |
| `deps/triplets/*`, `deps/toolchains/*`, `tools/docker/*` | Original | BSD-2-Clause |
| `toolchain.json`, `tools/toolchain.py` | Original | BSD-2-Clause |
| `patches/0003-no-export-decorations-for-zerog.patch` | Original, modifies `ExportMacros.h` (BSD), `BExport.h` (BSD) | BSD-2-Clause |
| `tools/bundle.py`, `port/api/exports.txt`, `tests/link/host_conflicts.c` | Original | BSD-2-Clause |

Origins: **Original** (written for ZeroG), **WebKitLegacy** (derived from
`Source/WebKitLegacy`, BSD), **Ultralight WebCore** (derived from
github.com/ultralight-ux/WebCore, LGPL), **Upstream** (copied from another
WebKit port, license of the source file).
