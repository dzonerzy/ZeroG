# Included at the end of Source/CMakeLists.txt, after every WebKit target exists.
# ZeroG's own layers (ZeroG.md, "Architettura") build in the same CMake, so they
# see WebCore's private headers, its configuration and its link targets.
if (NOT ZEROG_SOURCE_DIR)
    message(FATAL_ERROR "ZEROG_SOURCE_DIR is not set (tools/build.py sets it)")
endif ()

if (ENABLE_WEBCORE)
    add_subdirectory("${ZEROG_SOURCE_DIR}/port/embedding" "${CMAKE_BINARY_DIR}/ZeroG/embedding")
endif ()
