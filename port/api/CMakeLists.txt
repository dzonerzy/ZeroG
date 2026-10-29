# The C API (include/zerog.h) over the embedding layer.

set(ZeroGAPI_LIBRARY_TYPE STATIC)

set(ZeroGAPI_SOURCES
    API.cpp
    Bindings.cpp
)

set(ZeroGAPI_LIBRARIES
    ZeroGEmbedding
)

set(ZeroGAPI_FRAMEWORKS
    JavaScriptCore
    PAL
    WTF
    WebCore
    bmalloc
)

set(ZeroGAPI_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ZEROG_SOURCE_DIR}/include
    ${ZEROG_SOURCE_DIR}/port/embedding
)

WEBKIT_LIBRARY_DECLARE(ZeroGAPI)
WEBKIT_LIBRARY(ZeroGAPI)
ZEROG_USE_WEBCORE_INCLUDES(ZeroGAPI)

file(READ "${ZEROG_SOURCE_DIR}/WEBKIT_REVISION" _zerog_webkit_revision)
string(STRIP "${_zerog_webkit_revision}" _zerog_webkit_revision)
target_compile_definitions(ZeroGAPI PRIVATE
    ZEROG_WEBKIT_REVISION="${_zerog_webkit_revision}"
    ZEROG_PROFILE_NAME="${ZEROG_PROFILE}"
)
# The dev server origin is trusted only by the dev and debug profiles
# (ZeroG.md, "Origini fidate").
if (ZEROG_PROFILE STREQUAL "dev" OR ZEROG_PROFILE STREQUAL "debug")
    target_compile_definitions(ZeroGAPI PRIVATE ZEROG_DEV_PROFILE)
endif ()

# A C program using only include/zerog.h.
set(ZeroGAPITest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/api_test.c)
set(ZeroGAPITest_LIBRARIES ZeroGAPI)
set(ZeroGAPITest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGAPITest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGAPITest)
WEBKIT_EXECUTABLE(ZeroGAPITest)
if (NOT WIN32)
    target_link_libraries(ZeroGAPITest PRIVATE Threads::Threads)
endif ()
