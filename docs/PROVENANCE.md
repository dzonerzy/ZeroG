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
| `port/overlay/Source/PlatformZeroG.cmake`, `port/embedding/CMakeLists.txt`, `port/embedding/config.h`, `port/embedding/Embedding.*`, `port/embedding/PlatformStrategiesZeroG.h`, `tests/render/first_render.cpp` | Original (page setup follows `Source/WebCore/testing/TestPage.cpp`) | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/graphics/zerog/GraphicsLayerZeroG.cpp`, `port/overlay/Source/WebCore/platform/zerog/PasteboardZeroG.cpp`, `port/overlay/Source/WebCore/platform/zerog/PlatformKeyboardEventZeroG.cpp` | Original (keyboard disambiguation follows the GTK and WPE ports) | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/graphics/zerog/PlatformDisplayZeroG.*` | Original (display selection follows `PlatformDisplayWin.cpp` and `PlatformDisplaySurfaceless.cpp`) | BSD-2-Clause |
| `patches/0014-skia-gl-context-for-zerog.patch` | Original, one condition in `PlatformDisplaySkia.cpp` | Same as the modified file |
| `port/gl/*` | Original; the generated entry points are derived from the Khronos EGL/GLES headers (Apache-2.0 / MIT) | BSD-2-Clause |
| `patches/0012-webcore-generic-timer-keyboard-and-editor-for-zerog.patch` | Original, guard changes in WebCore files | Same as the modified files |
| `patches/0013-wtf-keep-main-thread-data-at-exit-on-windows.patch` | Original, modifies `ThreadingWin.cpp` (BSD) | BSD-2-Clause; upstream candidate |
| `port/embedding/View.*`, `port/embedding/Surface*`, `port/embedding/Window.*`, `tests/render/frame_loop.cpp`, `tests/render/texture_*.cpp`, `tests/render/window.cpp`, `tests/render/run_linux.sh` | Original | BSD-2-Clause |
| `port/embedding/KeyboardSDL.*`, `tests/render/input.cpp` | Original | BSD-2-Clause |
| `port/embedding/EditorClientZeroG.*` | Upstream, derived from `EmptyEditorClient` (`Source/WebCore/loader/EmptyClients.cpp`) and the key bindings of `Source/WebKit/WebProcess/WebPage/win/WebPageWin.cpp` | BSD-2-Clause (Apple and Sony headers kept) |
| `port/embedding/ChromeClientZeroG.*` | Upstream, derived from `EmptyChromeClient` in `Source/WebCore/loader/EmptyClients.{h,cpp}` | BSD-2-Clause (Apple header kept) |
| `port/embedding/FrameLoaderClientZeroG.*` | Upstream, derived from `EmptyFrameLoaderClient` in `Source/WebCore/loader/EmptyClients.{h,cpp}` | BSD-2-Clause (Apple, Eric Seidel, Nokia and Samsung headers kept) |
| `port/api/*`, `tests/api/*` | Original | BSD-2-Clause |
| `port/embedding/Loader.*` | Upstream, derived from WebKit's `WebLoaderStrategy`, `WebURLSchemeTaskProxy` (`Source/WebKit/WebProcess`), `WebErrors` (`Source/WebKit/Shared`) and `NetworkStorageSessionCurl` (`Source/WebKit/NetworkProcess/curl`) | BSD-2-Clause (Apple, Igalia and Sony headers kept) |
| `port/embedding/LoaderBlob.cpp` | Original; the load follows `Source/WebKit/NetworkProcess/NetworkDataTaskBlob.cpp` over WebCore's `BlobResourceHandleBase` and `BlobRegistryImpl` | BSD-2-Clause |
| `port/embedding/LoaderCurl.cpp` | Upstream, derived from `Source/WebKit/NetworkProcess/curl/NetworkDataTaskCurl.cpp` | BSD-2-Clause (Sony header kept) |
| `port/embedding/MemoryFS.*`, `port/embedding/Fonts.cpp`, `tools/assets.py`, `tests/render/loader.cpp` | Original | BSD-2-Clause |
| `patches/0015-webcore-embedder-frame-time.patch` | Original, modifies `Page.{h,cpp}` and `LocalDOMWindow.{h,cpp}` | Same as the modified files; upstream candidate |
| `patches/0016-wtf-generic-run-loop-next-wake-up-time.patch` | Original, modifies `RunLoop.h` and `RunLoopGeneric.cpp` (BSD) | BSD-2-Clause; upstream candidate |
| `patches/0017-webcore-threadable-loader-redirected-after-restart.patch` | Original, modifies `DocumentThreadableLoader.{h,cpp}` | Same as the modified files; upstream candidate |
| `port/embedding/WebSocketChannelZeroG.*` | Upstream, derived from `Source/WebKit/WebProcess/Network/WebSocketChannel.cpp` (Apple) and `Source/WebKit/NetworkProcess/curl/WebSocketTaskCurl.cpp` (Sony) | BSD-2-Clause (Apple and Sony headers kept) |
| `port/embedding/PlatformStrategiesZeroG.cpp` | Upstream, derived from `Tools/TestWebKitAPI/Tests/WebCore/TestPlatformStrategies.cpp` (Apple) | BSD-2-Clause (Apple header kept) |

Origins: **Original** (written for ZeroG), **WebKitLegacy** (derived from
`Source/WebKitLegacy`, BSD), **Ultralight WebCore** (derived from
github.com/ultralight-ux/WebCore, LGPL), **Upstream** (copied from another
WebKit port, license of the source file).
