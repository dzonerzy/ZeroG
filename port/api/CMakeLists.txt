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

# The example of ZeroG.md: a window, the frontend in app://, a binding.
file(GLOB_RECURSE _zerog_hello_web "${ZEROG_SOURCE_DIR}/examples/hello/web/*")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hello_assets.c
    COMMAND ${Python_EXECUTABLE} ${ZEROG_SOURCE_DIR}/tools/assets.py ${ZEROG_SOURCE_DIR}/examples/hello/web
        -o ${CMAKE_CURRENT_BINARY_DIR}/hello_assets.c --c-symbol hello_assets
    DEPENDS ${ZEROG_SOURCE_DIR}/tools/assets.py ${_zerog_hello_web}
    VERBATIM)
set(ZeroGHello_SOURCES ${ZEROG_SOURCE_DIR}/examples/hello/main.c ${CMAKE_CURRENT_BINARY_DIR}/hello_assets.c)
set(ZeroGHello_LIBRARIES ZeroGAPI)
set(ZeroGHello_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGHello_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGHello)
WEBKIT_EXECUTABLE(ZeroGHello)

# Value conversions, every type, both ways.
set(ZeroGValuesTest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/values_test.c)
set(ZeroGValuesTest_LIBRARIES ZeroGAPI)
set(ZeroGValuesTest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGValuesTest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGValuesTest)
WEBKIT_EXECUTABLE(ZeroGValuesTest)

# Page scripts, binary values, references and classes (API 0.2).
set(ZeroGBindingsTest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/bindings_test.c)
set(ZeroGBindingsTest_LIBRARIES ZeroGAPI)
set(ZeroGBindingsTest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGBindingsTest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGBindingsTest)
WEBKIT_EXECUTABLE(ZeroGBindingsTest)

# localStorage, sessionStorage and IndexedDB in the data directory.
set(ZeroGStorageTest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/storage_test.c)
set(ZeroGStorageTest_LIBRARIES ZeroGAPI)
set(ZeroGStorageTest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGStorageTest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGStorageTest)
WEBKIT_EXECUTABLE(ZeroGStorageTest)

# The clipboard, the file chooser and drag and drop.
set(ZeroGHostTest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/host_test.c)
set(ZeroGHostTest_LIBRARIES ZeroGAPI)
set(ZeroGHostTest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGHostTest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGHostTest)
WEBKIT_EXECUTABLE(ZeroGHostTest)

# Creating and destroying contexts, views and windows with work in flight.
set(ZeroGLifecycleTest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/lifecycle_test.c)
set(ZeroGLifecycleTest_LIBRARIES ZeroGAPI)
set(ZeroGLifecycleTest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGLifecycleTest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGLifecycleTest)
WEBKIT_EXECUTABLE(ZeroGLifecycleTest)

# Fuzz targets (tests/fuzz). In the AddressSanitizer build on Windows they are
# libFuzzer programs, with coverage of ZeroG's own code (the embedding layer and
# the API, where the parsers are); elsewhere they replay inputs from files, so the
# corpus runs as a regression test on every OS.
set(ZEROG_LIBFUZZER OFF)
if (MSVC AND ENABLE_SANITIZERS MATCHES "address")
    find_library(ZEROG_LIBFUZZER_LIBRARY clang_rt.fuzzer_no_main-x86_64 PATHS ${_zerog_clang_rt} NO_DEFAULT_PATH)
    if (ZEROG_LIBFUZZER_LIBRARY)
        set(ZEROG_LIBFUZZER ON)
        target_compile_options(ZeroGEmbedding PRIVATE -fsanitize=fuzzer-no-link)
        target_compile_options(ZeroGAPI PRIVATE -fsanitize=fuzzer-no-link)
        # The instrumented code calls the coverage hooks in every program that links
        # it: libFuzzer without its main has them, and does nothing unless driven
        # (tests/fuzz/fuzz.h calls LLVMFuzzerRunDriver).
        set_property(TARGET ZeroGEmbedding APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${ZEROG_LIBFUZZER_LIBRARY})
    endif ()
endif ()
foreach (_zerog_fuzz IN ITEMS Assets Requests Values)
    string(TOLOWER ${_zerog_fuzz} _zerog_fuzz_file)
    set(ZeroGFuzz${_zerog_fuzz}_SOURCES ${ZEROG_SOURCE_DIR}/tests/fuzz/fuzz_${_zerog_fuzz_file}.c)
    set(ZeroGFuzz${_zerog_fuzz}_LIBRARIES ZeroGAPI)
    set(ZeroGFuzz${_zerog_fuzz}_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
    set(ZeroGFuzz${_zerog_fuzz}_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
    WEBKIT_EXECUTABLE_DECLARE(ZeroGFuzz${_zerog_fuzz})
    WEBKIT_EXECUTABLE(ZeroGFuzz${_zerog_fuzz})
    if (ZEROG_LIBFUZZER)
        target_compile_definitions(ZeroGFuzz${_zerog_fuzz} PRIVATE ZEROG_LIBFUZZER)
        target_compile_options(ZeroGFuzz${_zerog_fuzz} PRIVATE -fsanitize=fuzzer-no-link)
    endif ()
endforeach ()

# Binding latency, through the public API.
set(ZeroGBindingBench_SOURCES ${ZEROG_SOURCE_DIR}/tests/bench/bindings.c)
set(ZeroGBindingBench_LIBRARIES ZeroGAPI)
set(ZeroGBindingBench_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGBindingBench_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGBindingBench)
WEBKIT_EXECUTABLE(ZeroGBindingBench)
if (NOT WIN32)
    target_link_libraries(ZeroGBindingBench PRIVATE Threads::Threads)
endif ()

# A C program using only include/zerog.h, with a frontend packed by
# tools/assets.py and compiled in, as an application does.
file(GLOB_RECURSE _zerog_test_assets "${ZEROG_SOURCE_DIR}/tests/api/assets/*")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_assets.c
    COMMAND ${Python_EXECUTABLE} ${ZEROG_SOURCE_DIR}/tools/assets.py ${ZEROG_SOURCE_DIR}/tests/api/assets
        -o ${CMAKE_CURRENT_BINARY_DIR}/test_assets.c --c-symbol test_assets
    DEPENDS ${ZEROG_SOURCE_DIR}/tools/assets.py ${_zerog_test_assets}
    VERBATIM)
set(ZeroGAPITest_SOURCES ${ZEROG_SOURCE_DIR}/tests/api/api_test.c ${CMAKE_CURRENT_BINARY_DIR}/test_assets.c)
set(ZeroGAPITest_LIBRARIES ZeroGAPI)
set(ZeroGAPITest_FRAMEWORKS ${ZeroGAPI_FRAMEWORKS})
set(ZeroGAPITest_PRIVATE_INCLUDE_DIRECTORIES ${ZEROG_SOURCE_DIR}/include)
WEBKIT_EXECUTABLE_DECLARE(ZeroGAPITest)
WEBKIT_EXECUTABLE(ZeroGAPITest)
# Ahem (public domain), from the WebKit checkout's Skia.
target_compile_definitions(ZeroGAPITest PRIVATE ZEROG_TEST_FONT="${CMAKE_SOURCE_DIR}/Source/ThirdParty/skia/resources/fonts/ahem.ttf")
if (NOT WIN32)
    target_link_libraries(ZeroGAPITest PRIVATE Threads::Threads)
endif ()
