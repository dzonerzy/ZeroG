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
| `port/overlay/Source/bmalloc/mimalloc/PlatformZeroG.cmake` | Original | BSD-2-Clause |
| `tests/link/jsc_c_api.c` | Original | BSD-2-Clause |
| `tests/link/icu_converters.c` | Original | BSD-2-Clause |
| `tools/icu_data.py`, `deps/icu-data.json` | Original | BSD-2-Clause |
| `deps/triplets/*`, `deps/toolchains/*`, `tools/docker/*` | Original | BSD-2-Clause |
| `deps/ports/libpsl/*` | Derived from the vcpkg port (MIT) | MIT |
| `toolchain.json`, `tools/toolchain.py` | Original | BSD-2-Clause |
| `patches/0003-no-export-decorations-for-zerog.patch` | Original, modifies `ExportMacros.h` (BSD), `BExport.h` (BSD) | BSD-2-Clause |
| `tools/bundle.py`, `port/api/exports.txt`, `tests/link/host_conflicts.c` | Original | BSD-2-Clause |
| `deps/ports/openssl/*` | Derived from the vcpkg `boringssl` port (MIT) | MIT |
| `tools/dev/*` | Original | BSD-2-Clause |
| `patches/0004-skia-freetype-custom-font-manager-for-zerog.patch` | Original, modifies Skia's `CMakeLists.txt` in WebKit (BSD) | BSD-2-Clause |
| `patches/0005-webcore-zerog-font-manager.patch` | Original, modifies WebCore font cache files (LGPL/BSD headers kept) | Same as the modified files |
| `patches/0006-cmake-directory-links-as-junctions-on-windows.patch` | Original, modifies `WebKitMacros.cmake` and Skia's `CMakeLists.txt` (BSD) | BSD-2-Clause; upstream candidate |
| `patches/0007-jsc-include-ccallhelpers-in-inlinecachecompiler.patch` | Original, one include in JSC (LGPL/BSD header kept) | Same as the modified file; upstream candidate |
| `patches/0008-wtf-msvc-abi-switchon-on-every-windows-port.patch` | Original, guard change in `StdLibExtras.h` (BSD) | BSD-2-Clause; upstream candidate |
| `patches/0009-webcore-boringssl-compatibility.patch` | Original, modifies WebCore curl/crypto files | Same as the modified files; upstream candidate |
| `patches/0010-webcore-zerog-and-windows-guards.patch` | Original, guard changes in WebCore files | Same as the modified files |
| `patches/0011-hardware-acceleration-setting-for-zerog.patch` | Original, one condition in `UnifiedWebPreferences.yaml` (BSD) | BSD-2-Clause |
| `port/overlay/Source/WebCore/PlatformZeroG.cmake`, `port/overlay/Source/WebCore/PAL/pal/PlatformZeroG.cmake`, `port/overlay/Source/ThirdParty/ANGLE/PlatformZeroG.cmake` | Original (PAL and source lists modeled on the PlayStation port) | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/graphics/zerog/FontManagerZeroG.*`, `port/overlay/Source/WebCore/platform/zerog/UserAgentZeroG.cpp`, `port/overlay/Source/WebCore/platform/network/zerog/CurlSSLHandleZeroG.cpp`, `port/overlay/Source/WebCore/accessibility/zerog/AXObjectCacheZeroG.cpp` | Original | BSD-2-Clause |

Origins: **Original** (written for ZeroG), **WebKitLegacy** (derived from
`Source/WebKitLegacy`, BSD), **Ultralight WebCore** (derived from
github.com/ultralight-ux/WebCore, LGPL), **Upstream** (copied from another
WebKit port, license of the source file).
