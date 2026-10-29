# ZeroG's embedding layer, built inside WebKit's CMake (port/overlay/Source/
# PlatformZeroG.cmake) so it uses WebCore's private headers and configuration.

set(ZeroGEmbedding_LIBRARY_TYPE STATIC)

set(ZeroGEmbedding_SOURCES
    Embedding.cpp
    PlatformStrategiesZeroG.cpp
)

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
