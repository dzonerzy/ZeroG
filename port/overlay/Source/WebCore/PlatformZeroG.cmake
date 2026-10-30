# WebCore for the ZeroG port: Skia (CPU and Ganesh GL over EGL), curl over
# BoringSSL, WebCore's image decoders, FreeType + Skia's custom font manager.
#
# Platform pieces come from three places:
# - ZeroG's own sources (platform/**/zerog), the platform layer of ZeroG.md;
# - upstream generic implementations, including the PlayStation port's files that
#   have no SDK dependency (marked below), as a first version to be replaced by
#   ZeroG's platform layer;
# - per-OS upstream implementations without PLATFORM(WIN) guards (Windows).

include(platform/Curl.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)
include(platform/Skia.cmake)
include(platform/Adwaita.cmake) # theme drawn by WebCore, the same on every OS

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    ${WEBCORE_DIR}/platform
    ${WEBCORE_DIR}/platform/generic
    ${WEBCORE_DIR}/platform/graphics/egl
    ${WEBCORE_DIR}/platform/graphics/opengl
    ${WEBCORE_DIR}/platform/graphics/zerog
    ${WEBCORE_DIR}/platform/mediacapabilities
    ${WEBCORE_DIR}/platform/network/zerog
    ${WEBCORE_DIR}/platform/video-codecs
    ${WEBCORE_DIR}/platform/zerog
)

list(APPEND WebCore_SOURCES
    # ZeroG
    accessibility/zerog/AXObjectCacheZeroG.cpp

    editing/zerog/EditorZeroG.cpp

    platform/graphics/zerog/FontManagerZeroG.cpp
    platform/graphics/zerog/GraphicsContextGLZeroG.cpp
    platform/graphics/zerog/GraphicsLayerZeroG.cpp
    platform/graphics/zerog/PlatformDisplayZeroG.cpp
    platform/zerog/DragDataZeroG.cpp
    platform/zerog/PasteboardZeroG.cpp
    platform/zerog/PlatformKeyboardEventZeroG.cpp
    platform/zerog/UserAgentZeroG.cpp

    # Upstream, generic
    editing/libwpe/EditorLibWPE.cpp # only uses the generic Pasteboard API

    page/glib/DragControllerGLib.cpp # no GLib in it: drag image sizes, the drop operation

    platform/ScrollAnimationKinetic.cpp
    platform/ScrollAnimationSmooth.cpp

    platform/generic/KeyedDecoderGeneric.cpp
    platform/generic/KeyedEncoderGeneric.cpp

    platform/graphics/PlatformDisplay.cpp

    platform/skia/DragImageSkia.cpp # drag images (ENABLE_DRAG_SUPPORT)

    platform/graphics/egl/BitmapTexture.cpp
    platform/graphics/egl/BitmapTexturePool.cpp
    platform/graphics/egl/GLContext.cpp
    platform/graphics/egl/GLContextWrapper.cpp
    platform/graphics/egl/GLDisplay.cpp
    platform/graphics/egl/GLFence.cpp
    platform/graphics/egl/GLFenceEGL.cpp
    platform/graphics/egl/GLFenceGL.cpp

    platform/text/Hyphenation.cpp
    platform/text/LocaleICU.cpp

    # Upstream PlayStation port, no SDK dependency (first version)
    accessibility/playstation/AccessibilityObjectPlayStation.cpp

    page/playstation/QuirkTablePlayStation.cpp

    platform/graphics/playstation/SystemFontDatabasePlayStation.cpp

    platform/network/playstation/NetworkStateNotifierPlayStation.cpp

    platform/playstation/MIMETypeRegistryPlayStation.cpp
    platform/playstation/PlatformScreenPlayStation.cpp
)

if (WIN32)
    list(APPEND WebCore_SOURCES
        platform/network/win/CurlSSLHandleWin.cpp

        platform/win/LoggingWin.cpp
        platform/win/SharedMemoryWin.cpp
    )
    list(APPEND WebCore_LIBRARIES Crypt32)
else ()
    list(APPEND WebCore_SOURCES
        platform/network/zerog/CurlSSLHandleZeroG.cpp

        platform/unix/LoggingUnix.cpp
        platform/unix/SharedMemoryUnix.cpp
    )
endif ()

list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/graphics/zerog/FontManagerZeroG.h
    platform/graphics/zerog/PlatformDisplayZeroG.h
    platform/zerog/PasteboardItemsZeroG.h
)
