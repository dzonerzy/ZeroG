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

add_definitions(-DBUILDING_ZEROG__)

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

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)
if (APPLE)
    add_definitions(-DU_DISABLE_RENAMING=1)
endif ()
