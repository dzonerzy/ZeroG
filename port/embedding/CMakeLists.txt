# ZeroG's embedding layer, built inside WebKit's CMake (port/overlay/Source/
# PlatformZeroG.cmake) so it uses WebCore's private headers and configuration.

set(ZeroGEmbedding_LIBRARY_TYPE STATIC)

set(ZeroGEmbedding_SOURCES
    ChromeClientZeroG.cpp
    EditorClientZeroG.cpp
    Embedding.cpp
    FrameLoaderClientZeroG.cpp
    KeyboardSDL.cpp
    Loader.cpp
    LoaderCurl.cpp
    MemoryFS.cpp
    PlatformStrategiesZeroG.cpp
    Surface.cpp
    SurfaceGL.cpp
    View.cpp
    Window.cpp
)

# Window mode: SDL3, static; on Linux it loads X11 or Wayland at run time.
find_package(SDL3 CONFIG REQUIRED)
set(ZeroGEmbedding_LIBRARIES SDL3::SDL3-static ZLIB::ZLIB)

if (WIN32)
    list(APPEND ZeroGEmbedding_SOURCES SurfaceD3D11.cpp)
    list(APPEND ZeroGEmbedding_LIBRARIES d3d11 dxgi)
endif ()

set(ZeroGEmbedding_FRAMEWORKS
    JavaScriptCore
    PAL
    WTF
    WebCore
    bmalloc
)

set(ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

WEBKIT_LIBRARY_DECLARE(ZeroGEmbedding)
WEBKIT_LIBRARY(ZeroGEmbedding)

# WebCore's headers include those of its dependencies (ICU, Skia, HarfBuzz, curl...),
# and WebKit's static frameworks only pass on their own header directories, so
# compile with WebCore's include path after our own.
function(ZEROG_USE_WEBCORE_INCLUDES _target)
    target_include_directories(${_target} SYSTEM PRIVATE $<TARGET_PROPERTY:WebCore,INCLUDE_DIRECTORIES>)
    # Skia's public definitions change its ABI (SK_TRIVIAL_ABI for sk_sp, pixel
    # order); code that calls Skia directly must be built with them.
    target_link_libraries(${_target} PRIVATE Skia)
endfunction()
ZEROG_USE_WEBCORE_INCLUDES(ZeroGEmbedding)

# Phase 1 gate: render HTML to a PNG and check its pixels.
set(ZeroGFirstRender_SOURCES
    ${ZEROG_SOURCE_DIR}/tests/render/first_render.cpp
)

set(ZeroGFirstRender_LIBRARIES
    ZeroGEmbedding
)

set(ZeroGFirstRender_FRAMEWORKS ${ZeroGEmbedding_FRAMEWORKS})
set(ZeroGFirstRender_PRIVATE_INCLUDE_DIRECTORIES ${ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES})

WEBKIT_EXECUTABLE_DECLARE(ZeroGFirstRender)
WEBKIT_EXECUTABLE(ZeroGFirstRender)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGFirstRender)

# Frame loop: requestAnimationFrame on the application's clock, dirty regions.
set(ZeroGFrameLoop_SOURCES ${ZEROG_SOURCE_DIR}/tests/render/frame_loop.cpp)
set(ZeroGFrameLoop_LIBRARIES ZeroGEmbedding)
set(ZeroGFrameLoop_FRAMEWORKS ${ZeroGEmbedding_FRAMEWORKS})
set(ZeroGFrameLoop_PRIVATE_INCLUDE_DIRECTORIES ${ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES})
WEBKIT_EXECUTABLE_DECLARE(ZeroGFrameLoop)
WEBKIT_EXECUTABLE(ZeroGFrameLoop)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGFrameLoop)

# Texture mode: frames shared with the application's GPU device, read on its own
# render thread.
if (WIN32)
    set(ZeroGTexture_SOURCES ${ZEROG_SOURCE_DIR}/tests/render/texture_d3d11.cpp)
    set(ZeroGTexture_LIBRARIES ZeroGEmbedding d3d11)
else ()
    set(ZeroGTexture_SOURCES ${ZEROG_SOURCE_DIR}/tests/render/texture_gl.cpp)
    set(ZeroGTexture_LIBRARIES ZeroGEmbedding)
endif ()
set(ZeroGTexture_FRAMEWORKS ${ZeroGEmbedding_FRAMEWORKS})
set(ZeroGTexture_PRIVATE_INCLUDE_DIRECTORIES ${ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES})
WEBKIT_EXECUTABLE_DECLARE(ZeroGTexture)
WEBKIT_EXECUTABLE(ZeroGTexture)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGTexture)

# Input: mouse, keyboard, text, wheel.
set(ZeroGInput_SOURCES ${ZEROG_SOURCE_DIR}/tests/render/input.cpp)
set(ZeroGInput_LIBRARIES ZeroGEmbedding)
set(ZeroGInput_FRAMEWORKS ${ZeroGEmbedding_FRAMEWORKS})
set(ZeroGInput_PRIVATE_INCLUDE_DIRECTORIES ${ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES})
WEBKIT_EXECUTABLE_DECLARE(ZeroGInput)
WEBKIT_EXECUTABLE(ZeroGInput)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGInput)

# The resource loader: app:// from memory, data:, http over curl.
set(ZeroGLoader_SOURCES ${ZEROG_SOURCE_DIR}/tests/render/loader.cpp)
set(ZeroGLoader_LIBRARIES ZeroGEmbedding ZLIB::ZLIB)
if (WIN32)
    list(APPEND ZeroGLoader_LIBRARIES ws2_32)
endif ()
set(ZeroGLoader_FRAMEWORKS ${ZeroGEmbedding_FRAMEWORKS})
set(ZeroGLoader_PRIVATE_INCLUDE_DIRECTORIES ${ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES})
WEBKIT_EXECUTABLE_DECLARE(ZeroGLoader)
WEBKIT_EXECUTABLE(ZeroGLoader)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGLoader)

# Window mode: a native window presented at vsync.
set(ZeroGWindow_SOURCES ${ZEROG_SOURCE_DIR}/tests/render/window.cpp)
set(ZeroGWindow_LIBRARIES ZeroGEmbedding SDL3::SDL3-static)
set(ZeroGWindow_FRAMEWORKS ${ZeroGEmbedding_FRAMEWORKS})
set(ZeroGWindow_PRIVATE_INCLUDE_DIRECTORIES ${ZeroGEmbedding_PRIVATE_INCLUDE_DIRECTORIES})
WEBKIT_EXECUTABLE_DECLARE(ZeroGWindow)
WEBKIT_EXECUTABLE(ZeroGWindow)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGWindow)
