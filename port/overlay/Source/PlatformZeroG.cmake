# Included at the end of Source/CMakeLists.txt, after every WebKit target exists.
# ZeroG's own layers (ZeroG.md, "Architettura") build in the same CMake, so they
# see WebCore's private headers, its configuration and its link targets.
if (NOT ZEROG_SOURCE_DIR)
    message(FATAL_ERROR "ZEROG_SOURCE_DIR is not set (tools/build.py sets it)")
endif ()

# Skia's path-ops debug helpers (SkPathOpsDebug.cpp) call SkPath::dump, which WebKit's
# Skia build leaves out. An archive link never pulls those helpers in, but the
# engine bundle links every object into one, so the definition has to be there.
if (TARGET Skia)
    target_sources(Skia PRIVATE "${CMAKE_SOURCE_DIR}/Source/ThirdParty/skia/src/core/SkPathDump.cpp")
endif ()

if (ENABLE_WEBCORE)
    add_subdirectory("${ZEROG_SOURCE_DIR}/port/embedding" "${CMAKE_BINARY_DIR}/ZeroG/embedding")
    add_subdirectory("${ZEROG_SOURCE_DIR}/port/api" "${CMAKE_BINARY_DIR}/ZeroG/api")
endif ()
