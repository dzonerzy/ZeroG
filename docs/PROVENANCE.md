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
| `port/overlay/Source/WebCore/PlatformZeroG.cmake`, `port/overlay/Source/WebCore/PAL/pal/PlatformZeroG.cmake`, `port/overlay/Source/ThirdParty/ANGLE/PlatformZeroG.cmake` | Original (PAL and source lists modeled on the PlayStation port; ANGLE's Linux sources as in ANGLE's `PlatformWPE.cmake`) | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/graphics/zerog/FontManagerZeroG.*`, `port/overlay/Source/WebCore/platform/zerog/UserAgentZeroG.cpp`, `port/overlay/Source/WebCore/platform/network/zerog/CurlSSLHandleZeroG.cpp`, `port/overlay/Source/WebCore/accessibility/zerog/AXObjectCacheZeroG.cpp` | Original | BSD-2-Clause |
| `port/overlay/Source/PlatformZeroG.cmake`, `port/embedding/CMakeLists.txt`, `port/embedding/config.h`, `port/embedding/Embedding.*`, `port/embedding/PlatformStrategiesZeroG.h`, `tests/render/first_render.cpp` | Original (page setup follows `Source/WebCore/testing/TestPage.cpp`) | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/zerog/PasteboardZeroG.cpp`, `port/overlay/Source/WebCore/platform/zerog/PlatformKeyboardEventZeroG.cpp` | Original (keyboard disambiguation follows the GTK and WPE ports) | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/graphics/zerog/PlatformDisplayZeroG.*` | Original (display selection follows `PlatformDisplayWin.cpp` and `PlatformDisplaySurfaceless.cpp`) | BSD-2-Clause |
| `patches/0014-skia-gl-context-for-zerog.patch` | Original, one condition in `PlatformDisplaySkia.cpp` | Same as the modified file |
| `port/gl/*` | Original; the generated entry points are derived from the Khronos EGL/GLES headers (Apache-2.0 / MIT) | BSD-2-Clause |
| `patches/0012-webcore-generic-timer-keyboard-and-editor-for-zerog.patch` | Original, guard changes in WebCore files | Same as the modified files |
| `patches/0013-wtf-keep-main-thread-data-at-exit-on-windows.patch` | Original, modifies `ThreadingWin.cpp` (BSD) | BSD-2-Clause; upstream candidate |
| `port/embedding/View.*`, `port/embedding/graphics/Surface*`, `port/embedding/window/Window.*`, `tests/render/frame_loop.cpp`, `tests/render/texture_*.cpp`, `tests/render/window.cpp`, `tests/render/run_linux.sh` | Original | BSD-2-Clause |
| `port/embedding/window/KeyboardSDL.*`, `tests/render/input.cpp` | Original | BSD-2-Clause |
| `port/embedding/clients/EditorClientZeroG.*` | Upstream, derived from `EmptyEditorClient` (`Source/WebCore/loader/EmptyClients.cpp`) and the key bindings of `Source/WebKit/WebProcess/WebPage/win/WebPageWin.cpp` | BSD-2-Clause (Apple and Sony headers kept) |
| `port/embedding/clients/ChromeClientZeroG.*` | Upstream, derived from `EmptyChromeClient` in `Source/WebCore/loader/EmptyClients.{h,cpp}` | BSD-2-Clause (Apple header kept) |
| `port/embedding/clients/FrameLoaderClientZeroG.*` | Upstream, derived from `EmptyFrameLoaderClient` in `Source/WebCore/loader/EmptyClients.{h,cpp}` | BSD-2-Clause (Apple, Eric Seidel, Nokia and Samsung headers kept) |
| `port/api/*`, `tests/api/*` | Original | BSD-2-Clause |
| `port/embedding/loader/Loader.*` | Upstream, derived from WebKit's `WebLoaderStrategy`, `WebURLSchemeTaskProxy` (`Source/WebKit/WebProcess`), `WebErrors` (`Source/WebKit/Shared`) and `NetworkStorageSessionCurl` (`Source/WebKit/NetworkProcess/curl`) | BSD-2-Clause (Apple, Igalia and Sony headers kept) |
| `port/embedding/loader/LoaderBlob.cpp` | Original; the load follows `Source/WebKit/NetworkProcess/NetworkDataTaskBlob.cpp` over WebCore's `BlobResourceHandleBase` and `BlobRegistryImpl` | BSD-2-Clause |
| `port/embedding/loader/LoaderCurl.cpp` | Upstream, derived from `Source/WebKit/NetworkProcess/curl/NetworkDataTaskCurl.cpp` | BSD-2-Clause (Sony header kept) |
| `port/embedding/loader/MemoryFS.*`, `port/embedding/loader/Fonts.cpp`, `port/embedding/loader/Download.*`, `tools/assets.py`, `tests/render/loader.cpp` | Original | BSD-2-Clause |
| `patches/0015-webcore-embedder-frame-time.patch` | Original, modifies `Page.{h,cpp}` and `LocalDOMWindow.{h,cpp}` | Same as the modified files; upstream candidate |
| `patches/0016-wtf-generic-run-loop-next-wake-up-time.patch` | Original, modifies `RunLoop.h` and `RunLoopGeneric.cpp` (BSD) | BSD-2-Clause; upstream candidate |
| `patches/0017-webcore-threadable-loader-redirected-after-restart.patch` | Original, modifies `DocumentThreadableLoader.{h,cpp}` | Same as the modified files; upstream candidate |
| `patches/0018-webcore-console-message-call-stack-to-chrome-client.patch` | Original, modifies `ConsoleMessage.h`, `ChromeClient.{h,cpp}` and `FrameConsoleClient.cpp` | Same as the modified files; upstream candidate |
| `patches/0019-jsc-precommit-windows-stack-pages-once.patch` | Original, modifies `VM.cpp` | Same as the modified file; upstream candidate |
| `examples/hello/*`, `tests/bench/*` | Original | BSD-2-Clause |
| `tests/fuzz/*` (targets, seeds and corpus) | Original | BSD-2-Clause |
| `port/embedding/storage/{StorageAreaImpl,StorageAreaSync,StorageNamespaceImpl,StorageSyncManager,StorageThread,WebStorageNamespaceProvider,InProcessIDBServer}.*` | WebKitLegacy, copied from `Source/WebKitLegacy/Storage` (namespace kept). Changes: `config.h` and explicit includes, local storage in memory for ephemeral sessions | BSD-2-Clause (Apple headers kept) |
| `port/embedding/storage/StorageTracker.h`, `DatabaseProviderZeroG.*` | Original: an inactive stand-in for WebKitLegacy's tracker; IndexedDB per session | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/zerog/{PasteboardItemsZeroG.h,PasteboardZeroG.cpp,DragDataZeroG.cpp}`, `port/overlay/Source/WebCore/editing/zerog/EditorZeroG.cpp` | Original; the pasteboard and drag data follow the GLib port's (`platform/glib/PasteboardGLib.cpp`, `DragDataGLib.cpp`) | BSD-2-Clause |
| `port/embedding/clients/DragClientZeroG.*`, `port/embedding/window/ClipboardSDL.*`, `tests/api/host_test.c` | Original; the in-page drag loop follows WebKit2's `WebPage::performDragControllerAction` and `dragEnded` | BSD-2-Clause |
| `patches/0020-webcore-pasteboard-items-for-zerog.patch`, `patches/0021-webcore-drag-and-drop-for-zerog.patch` | Original, modify `Pasteboard.{h,cpp}`, `DataTransfer.cpp` and `DragImage.cpp` | Same as the modified files |
| `port/embedding/loader/HTTPCache.*` | Original; freshness and validation through WebCore's `CacheValidation` | BSD-2-Clause |
| `port/embedding/loader/WebSocketChannelZeroG.*` | Upstream, derived from `Source/WebKit/WebProcess/Network/WebSocketChannel.cpp` (Apple) and `Source/WebKit/NetworkProcess/curl/WebSocketTaskCurl.cpp` (Sony) | BSD-2-Clause (Apple and Sony headers kept) |
| `port/embedding/PlatformStrategiesZeroG.cpp` | Upstream, derived from `Tools/TestWebKitAPI/Tests/WebCore/TestPlatformStrategies.cpp` (Apple) | BSD-2-Clause (Apple header kept) |
| `patches/0022-webcore-curl-trusted-certificates.patch` | Original, modifies `CurlSSLHandle.{h,cpp}` and `CurlContext.cpp` | Same as the modified files; upstream candidate |
| `port/embedding/graphics/CoordinatedSceneState.*` | WebKit, copied from `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics` (namespace kept, unchanged) | BSD-2-Clause (Igalia header kept) |
| `port/embedding/graphics/Compositor.*` | Upstream, derived from WebKit's `LayerTreeHost` and `ThreadedCompositor` (`Source/WebKit/WebProcess/WebPage/CoordinatedGraphics`): one process, no IPC, the view's surface for `AcceleratedSurface` | BSD-2-Clause (Igalia, Apple, Nokia and Company 100 headers kept) |
| `tests/render/compositing.cpp`, `port/overlay/Source/ThirdParty/ANGLE/PlatformZeroG.cmake` | Original | BSD-2-Clause |
| `patches/0026-webcore-skia-executor-per-context.patch` | Original, modifies `PlatformDisplaySkia.cpp` (Skia's executor owned by its GL context, not joined by a thread's TLS destructors) | Same as the modified file; upstream candidate |
| `patches/0025-webcore-coordinated-scrolling-only-with-async-scrolling.patch` | Original, a condition in `platform/CoordinatedGraphics.cmake` (BSD) | BSD-2-Clause; upstream candidate |
| `patches/0024-webcore-coordinated-skia-without-epoxy.patch` | Original, includes in `CoordinatedPlatformLayerBufferSkiaImage.cpp` and `SkiaBackingStore.cpp` (GLES headers without libepoxy) | Same as the modified file; upstream candidate |
| `patches/0023-jsc-remote-inspector-socket-use-after-free.patch` | Original, modifies `RemoteInspectorSocketEndpoint.cpp` (a closed connection was used after its map freed it) | Same as the modified file; upstream candidate |
| `port/embedding/inspector/{LegacyWebPageInspectorController,LegacyWebPageDebuggable}.*` | WebKitLegacy, copied from `Source/WebKitLegacy/WebCoreSupport`. Changes: `config.h`, the debuggable guarded by `ENABLE(REMOTE_INSPECTOR)` | BSD-2-Clause (Apple headers kept) |
| `port/overlay/Source/WebCore/accessibility/zerog/AccessibilityClientZeroG.h`, `port/embedding/accessibility/AccessibilityUIA.cpp`, `port/embedding/accessibility/AccessibilityAtspi.cpp`, `port/embedding/accessibility/DBus.*`, `tests/render/accessibility_*.cpp` | Original; AT-SPI's role and state values are at-spi2-core's protocol constants, D-Bus's types libdbus's public ABI | BSD-2-Clause |
| `port/embedding/media/*`, `tests/render/media.cpp` | Original; the engine implements WebCore's `MediaPlayerPrivateInterface`, as upstream's players do; the codecs are libraries from the sysroot (deps/vcpkg.json) | BSD-2-Clause |
| `tests/media/*` | Original: colours and a 440 Hz tone, made with FFmpeg for the test | BSD-2-Clause |
| `deps/ports/libwebm/*` | vcpkg's libwebm port, with libwebm's callback parser built into the library (feature `parser`, `parser-objects-in-library.patch`) | MIT (vcpkg); the patch BSD-3-Clause, as libwebm |
| `deps/ports/sdl3/*` | vcpkg's SDL3 port, with SDL's sound backends (ALSA, PulseAudio, PipeWire) loaded at run time (feature `system-audio`) | MIT (vcpkg) |
| `patches/0029-webcore-media-engine-for-zerog.patch` | Original, modifies `MediaPlayer.h` and `MediaPlayerEnums.h` (the ZeroG engine's type and identifier) | Same as the modified files; upstream candidate |
| `port/overlay/Source/WebCore/platform/audio/zerog/FFTFrameZeroG.cpp` | Original; the packed spectrum layout follows `FFTFrame.h` and the Cocoa port's `FFTFrameMac.cpp` (vDSP's convention), the transform is pffft from the sysroot | BSD-2-Clause |
| `port/embedding/media/MediaFoundationFile.cpp` | Original; Media Foundation's interfaces and constants are the Windows SDK's public API | BSD-2-Clause |
| `port/embedding/media/{MediaSourceZeroG,WebMStream}.*`, `port/embedding/media/Decoders.h` | Original; the media source and source buffer follow WebCore's mock implementation (`platform/mock/mediasource`), the WebM byte stream is parsed with libwebm's `webm_parser` | BSD-2-Clause |
| `patches/0031-webcore-media-source-for-zerog.patch` | Original, modifies `MediaPlayer.h` (the ZeroG media source's platform type) | Same as the modified file; upstream candidate |
| `patches/0030-webcore-fft-frame-for-zerog.patch` | Original, modifies `FFTFrame.h` (pffft's setup and buffers in a ZeroG `FFTFrame`) | Same as the modified file; upstream candidate |
| `port/embedding/media/WebAudioZeroG.cpp`, `port/embedding/media/EmbedFile.cmake`, `tests/render/webaudio.cpp` | Original; the destination follows the Cocoa port's `AudioDestinationCocoa` over `AudioDestinationResampler` | BSD-2-Clause |
| HRTF responses (built in from `Source/WebCore/platform/audio/resources/Composite.wav`) | WebKit's resource, unchanged, compiled into the engine as a C array | Same as WebKit's resource (BSD) |
| `port/embedding/graphics/SurfaceDMABuf.cpp`, `tests/render/texture_dmabuf.cpp` | Original; DRM's ioctls and structures (`drm.h`, `drm_mode.h`), GBM's functions and Vulkan's types and values are those projects' public ABI, declared by hand | BSD-2-Clause |
| `port/embedding/accessibility/Accessibility.*` | Original; the name and description follow upstream's `accessibility/atspi/AccessibilityObjectAtspi.cpp` (the first text alternative that is not help or summary text) | BSD-2-Clause |
| `port/embedding/inspector/InspectorZeroG.*`, `tests/render/inspector.cpp` | Original; the local frontend follows WebKitLegacy's `mac/WebCoreSupport/WebInspectorClient.mm` | BSD-2-Clause |
| `port/overlay/Source/WebCore/platform/graphics/zerog/GraphicsContextGLZeroG.cpp` | Upstream, derived from `platform/graphics/coordinated/GraphicsContextGLCoordinatedEpoxy.cpp` (the engine's GL entry points instead of libepoxy) | BSD-2-Clause (Igalia header kept) |
| `patches/0028-webcore-skia-compositing-layer-paint-time.patch` | Original, modifies `SkiaCompositingLayer.{h,cpp}` (animations applied at the embedder's frame time, now by default) | Same as the modified files; upstream candidate |
| `patches/0027-webcore-angle-webgl-for-zerog-on-windows.patch` | Original, modifies `PlatformDisplayANGLE.cpp`, `GraphicsContextGLEGL.cpp` (ANGLE is the EGL on Windows, as for the Win port; context virtualization only where ANGLE has it) and `GraphicsContextGLANGLE.cpp` | Same as the modified files; upstream candidate |

Origins: **Original** (written for ZeroG), **WebKitLegacy** (derived from
`Source/WebKitLegacy`, BSD), **Ultralight WebCore** (derived from
github.com/ultralight-ux/WebCore, LGPL), **Upstream** (copied from another
WebKit port, license of the source file).
