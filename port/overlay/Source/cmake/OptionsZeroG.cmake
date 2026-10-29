# ZeroG port: one single-process, statically linked port for Linux, Windows and macOS.
# Stage 1 builds WTF, bmalloc and JavaScriptCore only (parity with JSCOnly);
# WebCore, PAL and the ZeroG layers are switched on in later stages.

find_package(Threads REQUIRED)

if (MSVC)
    include(OptionsMSVC)
    # Single binary: static CRT. OptionsMSVC hard-sets MultiThreadedDLL, so this
    # must come after it; every dependency in the sysroot is built /MT as well.
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
else ()
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif ()

# Linux builds use zig's clang (tools/docker): glibc baseline from the zig target,
# libc++ and compiler-rt always static. ZEROG_ZIG=OFF is a plain clang/libstdc++
# build, which needs the runtime made static explicitly.
option(ZEROG_ZIG "Linux toolchain is zig (glibc baseline, static libc++)" OFF)
if (ZEROG_ZIG AND WTF_CPU_X86_64)
    # zig hands clang the full baseline feature list, including -evex512, which
    # target("avx512...") attributes do not re-enable: simdutf's runtime-dispatched
    # AVX-512 kernels then fail to compile. -mevex512 only permits 512-bit vectors
    # in functions that opt into AVX-512; baseline code generation is unchanged.
    add_compile_options(-mevex512)
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ZEROG_ZIG)
    # Executables built in the port (jsc, tests) carry the C++ runtime: only glibc
    # stays dynamic (ZeroG.md, "Cosa resta dinamico a runtime").
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static-libstdc++ -static-libgcc")
    # WTF/JSC link the bare name "atomic" when ATOMICS_REQUIRE_LIBATOMIC; an
    # imported target of that name takes precedence and points at the static library.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libatomic.a
        OUTPUT_VARIABLE _zerog_libatomic OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (IS_ABSOLUTE "${_zerog_libatomic}" AND EXISTS "${_zerog_libatomic}")
        add_library(atomic STATIC IMPORTED GLOBAL)
        set_target_properties(atomic PROPERTIES IMPORTED_LOCATION "${_zerog_libatomic}")
    endif ()
endif ()

add_definitions(-DBUILDING_ZEROG__)
# No export decorations anywhere (patch 0003 does the same for WTF/bmalloc): the
# JSC C API is internal to the engine; only zg_* is public (port/api/exports.txt).
add_definitions(-DJS_NO_EXPORT)

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_MICRO 0)
set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_MICRO})

WEBKIT_OPTION_BEGIN()
# Allocator (ZeroG.md, "Allocatore di memoria"): no libpas/Gigacage reservations.
# Upstream rejects system malloc on Windows, which uses the vendored mimalloc.
if (WIN32)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SYSTEM_MALLOC PRIVATE OFF)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_MIMALLOC PRIVATE ON)
else ()
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SYSTEM_MALLOC PRIVATE ON)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_MIMALLOC PRIVATE OFF)
endif ()
# Only the dev profile gets the inspector (ZeroG.md, "Funzioni da spegnere").
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_SYSTEM_UNIFDEF "Whether to use a system-provided unifdef" PRIVATE OFF)
WEBKIT_OPTION_END()

set(ENABLE_WEBCORE OFF)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT OFF)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_WEBGL OFF)
set(ENABLE_WEBGPU OFF)
set(ENABLE_API_TESTS OFF)

# Everything static; tools/build.py bundle collects the objects into one archive.
set(bmalloc_LIBRARY_TYPE STATIC)
set(WTF_LIBRARY_TYPE STATIC)
set(JavaScriptCore_LIBRARY_TYPE STATIC)

# WTF's RunLoopGeneric everywhere: no GLib, no RunLoopWin, no CFRunLoop.
set(EVENT_LOOP_TYPE "Generic")
set(LOWERCASE_EVENT_LOOP_TYPE "generic")
SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)

if (WIN32)
    # Windows 10 1809 minimum, as upstream JSCOnly.
    add_definitions(-D_WINDOWS -DNTDDI_VERSION=0x0A000006 -D_WIN32_WINNT=0x0A00)
    add_definitions(-DNOMINMAX -DUNICODE -D_UNICODE -DNOCRYPT)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
    add_definitions(-D_SILENCE_CXX23_DENORM_DEPRECATION_WARNING)
    add_definitions(-D_WINSOCKAPI_=)

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif ()

# ICU data comes from the filtered library built by tools/icu_data.py for the
# selected profile (deps/icu-data.json), not from ICU's stock icudt.
# The variable name carries the profile, so switching profiles re-runs the search
# instead of reusing a cached path.
set(ZEROG_ICU_DATA "minimal" CACHE STRING "ICU data profile from deps/icu-data.json")
find_library(ZEROG_ICU_DATA_LIBRARY_${ZEROG_ICU_DATA} NAMES icudt-${ZEROG_ICU_DATA} REQUIRED)
set(ICU_DATA_LIBRARY_RELEASE "${ZEROG_ICU_DATA_LIBRARY_${ZEROG_ICU_DATA}}" CACHE FILEPATH "" FORCE)

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)

# Static ICU: declare i18n -> uc -> data so single-pass linkers (GNU ld) see the
# libraries in dependency order; FindICU lists data first.
set_property(TARGET ICU::i18n APPEND PROPERTY INTERFACE_LINK_LIBRARIES ICU::uc)
set_property(TARGET ICU::uc APPEND PROPERTY INTERFACE_LINK_LIBRARIES ICU::data)
if (APPLE)
    add_definitions(-DU_DISABLE_RENAMING=1)
endif ()
