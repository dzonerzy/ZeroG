# ZeroG port: one single-process, statically linked port for Linux, Windows and macOS.
# WTF, bmalloc, JavaScriptCore, PAL and WebCore, with Skia, curl over BoringSSL and
# RunLoopGeneric everywhere. The embedding layer and the zg_* API build on top.

find_package(Threads REQUIRED)

# Build profile (ZeroG.md, "Profili di build"), passed by tools/build.py.
set(ZEROG_PROFILE "minimal" CACHE STRING "minimal, standard, dev or debug")
if (ZEROG_PROFILE STREQUAL "minimal")
    set(_zerog_full_web OFF)
else ()
    set(_zerog_full_web ON)
endif ()
if (ZEROG_PROFILE STREQUAL "dev" OR ZEROG_PROFILE STREQUAL "debug")
    set(_zerog_inspector ON)
else ()
    set(_zerog_inspector OFF)
endif ()

if (MSVC)
    include(OptionsMSVC)
    # Single binary: static CRT. OptionsMSVC hard-sets MultiThreadedDLL, so this
    # must come after it; every dependency in the sysroot is built /MT as well.
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    # OptionsMSVC gives every build full debug information (/Zi, /DEBUG). Each
    # program linking the static engine (every test) then writes all its types
    # into a PDB of its own, about 1.3 GB apiece. Outside the debug profile, line
    # tables only (clang-cl takes the last of /Zi and this): stacks keep their
    # functions and lines, the PDBs a fraction of the size.
    if (NOT ZEROG_PROFILE STREQUAL "debug")
        MSVC_ADD_COMPILE_OPTIONS(-gline-tables-only)
    endif ()
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

option(ZEROG_MIMALLOC_SECURE "mimalloc hardened mode: guard pages, randomized allocation, double-free and free-list corruption detection" OFF)
# No export decorations anywhere (patch 0003 does the same for WTF/bmalloc): the
# JSC C API is internal to the engine; only zg_* is public (port/api/exports.txt).
add_definitions(-DJS_NO_EXPORT)

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 1) # include/zerog.h
set(PROJECT_VERSION_MICRO 0)
set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_MICRO})

# AddressSanitizer builds: -DENABLE_SANITIZERS=address, tools/build.py --asan.
# mimalloc stays (system malloc is refused on Windows, BPlatform.h) and reports its
# blocks to the sanitizer (Source/bmalloc/mimalloc/PlatformZeroG.cmake).
if (ENABLE_SANITIZERS AND MSVC)
    # The runtime is the asan_dynamic DLL with the thunk for the CRT: the static-CRT
    # one here (/MT). WebKitCompilerFlags.cmake links the /MD thunk instead, which
    # does not link with /MT; lld-link is run directly, so nothing else names them.
    get_directory_property(_zerog_link_options LINK_OPTIONS)
    list(REMOVE_ITEM _zerog_link_options "${CLANG_ASAN_LIBRARY}" "${CLANG_ASAN_RT_LIBRARY}")
    set_directory_properties(PROPERTIES LINK_OPTIONS "${_zerog_link_options}")
    get_filename_component(_zerog_llvm "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _zerog_clang_rt "${_zerog_llvm}/../lib/clang/*/lib/windows")
    add_link_options("/LIBPATH:${_zerog_clang_rt}"
        clang_rt.asan_dynamic-x86_64.lib clang_rt.asan_static_runtime_thunk-x86_64.lib)
    # The sysroot's libraries are not instrumented; the STL's container annotations
    # must then be off everywhere (/failifmismatch on annotate_string/vector).
    add_definitions(-D_DISABLE_STRING_ANNOTATION -D_DISABLE_VECTOR_ANNOTATION)
    file(COPY "${_zerog_clang_rt}/clang_rt.asan_dynamic-x86_64.dll" DESTINATION "${CMAKE_BINARY_DIR}/bin")
endif ()

WEBKIT_OPTION_BEGIN()
# Allocator (ZeroG.md, "Allocatore di memoria"): the vendored mimalloc on every OS,
# so allocation behaves the same everywhere; no libpas/Gigacage. It never replaces
# the host's malloc (MI_OVERRIDE is off upstream) and its symbols are private to
# the bundle. ZEROG_MIMALLOC_SECURE turns on mimalloc's hardened mode
# (Source/bmalloc/mimalloc/PlatformZeroG.cmake).
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SYSTEM_MALLOC PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_MIMALLOC PRIVATE ON)
WEBKIT_OPTION_DEFINE(USE_SYSTEM_UNIFDEF "Whether to use a system-provided unifdef" PRIVATE OFF)

WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA_ENCODERS PRIVATE ON) # toDataURL, toBlob
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_WOFF2 PRIVATE ON)

# Functions per profile (ZeroG.md, "Funzioni da spegnere").
# WebGL over ANGLE, composited as layers (GraphicsContextGLZeroG.cpp): ANGLE is
# the GL on Windows; on Linux its GL backend runs over the system's EGL
# (ThirdParty/ANGLE/PlatformZeroG.cmake), as GTK and WPE build it.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBGL PRIVATE ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBASSEMBLY PRIVATE ${_zerog_full_web})
# The WebAssembly JIT tiers default to the FTL setting, not to ENABLE_WEBASSEMBLY:
# left on without WebAssembly they pull Wasm types into B3 and fail to compile.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBASSEMBLY_BBQJIT PRIVATE ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBASSEMBLY_OMGJIT PRIVATE ${_zerog_full_web})
# FTL, JSC's top tier, follows WebAssembly: its B3 backend contains WebAssembly GC
# nodes that only build with WebAssembly. Minimal keeps the interpreter, Baseline
# and DFG (smaller, less JIT attack surface); other profiles have both.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FTL_JIT PRIVATE ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE ${_zerog_inspector})
# Media (ZeroG.md, "Media"): ENABLE_VIDEO stays on in every profile (no upstream
# port builds without it and WebCore compiles media code unguarded). The standard,
# dev and debug profiles have ZeroG's media engine (port/embedding/media: WebM,
# Ogg, MP3, FLAC, WAV; MP4 on Windows), Web Audio and Media Source Extensions
# (WebM) on it; in minimal <video>/<audio> exist and report that they cannot
# play, and Web Audio and MSE are off (as PlayStation builds).
set(ZEROG_MEDIA ${_zerog_full_web})
# The Cache API (port/embedding/storage/CacheStorageZeroG.cpp), off in minimal.
set(ZEROG_FULL_WEB ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUDIO PRIVATE ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_SOURCE PRIVATE ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_XSLT PRIVATE OFF)
# AVIF in the full profiles (libavif over dav1d, the AV1 decoder media uses).
# JPEG XL is left out everywhere (ZeroG.md), and so is color management.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_AVIF PRIVATE ${_zerog_full_web})
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_JPEGXL PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_LCMS PRIVATE OFF)
# Browser features an application engine does not provide.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GEOLOCATION PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_NOTIFICATIONS PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_USER_MESSAGE_HANDLERS PRIVATE OFF) # WebKit2 API
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_IMAGE_DIFF PRIVATE OFF)
# Drag and drop, and selecting text by dragging the mouse (it needs the same code).
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DRAG_SUPPORT PRIVATE ON)
WEBKIT_OPTION_END()

set(ENABLE_WEBCORE ON)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT OFF)
# The Web Inspector's frontend (dev and debug): built into a resource directory,
# which port/embedding packs with tools/assets.py and compiles in.
set(ENABLE_WEBINSPECTORUI ${_zerog_inspector})
set(ENABLE_WEBGPU OFF)
set(ENABLE_API_TESTS OFF)

# Everything static; tools/build.py bundle collects the objects into one archive.
set(bmalloc_LIBRARY_TYPE STATIC)
set(WTF_LIBRARY_TYPE STATIC)
set(JavaScriptCore_LIBRARY_TYPE STATIC)
set(PAL_LIBRARY_TYPE STATIC)
set(WebCore_LIBRARY_TYPE STATIC)
set(WebCoreTestSupport_LIBRARY_TYPE STATIC)

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

# vcpkg's ICU wrapper creates ICU::uc first and sets its location afterwards, but
# WebKit's FindICU calls find_package(ICU) again from inside, and the nested run of
# the wrapper resets the outer run's fixup list in the shared scope: ICU::uc ends
# up without a location. Complete it here.
get_target_property(_zerog_icu_uc ICU::uc IMPORTED_LOCATION_RELEASE)
if (NOT _zerog_icu_uc)
    set_property(TARGET ICU::uc APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(ICU::uc PROPERTIES
        IMPORTED_LOCATION_RELEASE "${ICU_UC_LIBRARY_RELEASE}"
        IMPORTED_LOCATION "${ICU_UC_LIBRARY_RELEASE}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        INTERFACE_INCLUDE_DIRECTORIES "${ICU_INCLUDE_DIR}")
endif ()

# Static ICU: declare i18n -> uc -> data so single-pass linkers (GNU ld) see the
# libraries in dependency order; FindICU lists data first.
set_property(TARGET ICU::i18n APPEND PROPERTY INTERFACE_LINK_LIBRARIES ICU::uc)
set_property(TARGET ICU::uc APPEND PROPERTY INTERFACE_LINK_LIBRARIES ICU::data)
if (APPLE)
    add_definitions(-DU_DISABLE_RENAMING=1)
endif ()

# WebCore dependencies, all from the static sysroot (deps/vcpkg.json).
find_package(CURL 7.87.0 REQUIRED)
find_package(Freetype 2.9.0 REQUIRED)
find_package(HarfBuzz 1.4.2 REQUIRED COMPONENTS ICU)
find_package(JPEG 1.5.2 REQUIRED)
find_package(LibPSL 0.20.2 REQUIRED)
find_package(LibXml2 2.9.7 REQUIRED)
find_package(OpenSSL REQUIRED) # BoringSSL (deps/ports/openssl)
find_package(PNG 1.6.34 REQUIRED)
find_package(SQLite3 3.23.1 REQUIRED)
find_package(WebP REQUIRED COMPONENTS demux mux)
find_package(ZLIB 1.2.11 REQUIRED)
if (USE_WOFF2)
    find_package(WOFF2 1.0.2 REQUIRED COMPONENTS dec)
    find_package(Brotli REQUIRED COMPONENTS dec)
endif ()

# Upstream's find modules expect shared libraries, which carry their own
# dependencies. The static archives in the sysroot do not.
find_library(ZEROG_SHARPYUV_LIBRARY NAMES sharpyuv libsharpyuv REQUIRED)
set_property(TARGET WebP::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${ZEROG_SHARPYUV_LIBRARY})
if (USE_WOFF2)
    set_property(TARGET WOFF2::dec APPEND PROPERTY INTERFACE_LINK_LIBRARIES WOFF2::common Brotli::dec)
endif ()
if (USE_AVIF)
    find_package(AVIF 0.9.0 REQUIRED)
    find_library(ZEROG_AVIF_DAV1D_LIBRARY NAMES dav1d libdav1d REQUIRED)
    find_library(ZEROG_AVIF_YUV_LIBRARY NAMES yuv libyuv REQUIRED)
    set_property(TARGET AVIF::AVIF APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${ZEROG_AVIF_DAV1D_LIBRARY} ${ZEROG_AVIF_YUV_LIBRARY})
endif ()

SET_AND_EXPOSE_TO_BUILD(USE_CURL ON)
SET_AND_EXPOSE_TO_BUILD(USE_HARFBUZZ ON)
# Form controls and scrollbars drawn by WebCore itself (as on Win, GTK, WPE): the
# same look on every OS.
SET_AND_EXPOSE_TO_BUILD(USE_THEME_ADWAITA ON)
SET_AND_EXPOSE_TO_BUILD(USE_OPENSSL ON)
SET_AND_EXPOSE_TO_BUILD(USE_BORINGSSL ON)
SET_AND_EXPOSE_TO_BUILD(USE_INSPECTOR_SOCKET_SERVER ${ENABLE_REMOTE_INSPECTOR})
if (NOT WIN32)
    SET_AND_EXPOSE_TO_BUILD(USE_UNIX_DOMAIN_SOCKETS ON)
endif ()
# Web Audio's HRTF responses as the one concatenated resource (Composite.wav,
# compiled into the engine), as Cocoa and GStreamer ports load them.
SET_AND_EXPOSE_TO_BUILD(USE_CONCATENATED_IMPULSE_RESPONSES ${ENABLE_WEB_AUDIO})

# GPU (ZeroG.md, "Backend GPU per piattaforma"): Skia Ganesh over GLES through EGL,
# the one GPU path upstream supports. Windows uses ANGLE (GL over D3D11), built in
# tree. Linux uses the system driver, loaded at run time; only the Khronos headers
# are needed to build, so no EGL/GLES library is linked.
if (WIN32)
    SET_AND_EXPOSE_TO_BUILD(USE_ANGLE ON)
    SET_AND_EXPOSE_TO_BUILD(USE_ANGLE_EGL ON)
    # ANGLE is static (Source/ThirdParty/ANGLE/PlatformZeroG.cmake): without these the
    # Khronos headers and ANGLE's export.h mark every EGL/GLES entry point dllimport.
    add_definitions(-DKHRONOS_STATIC -DANGLE_EXPORT=)
else ()
    # ANGLE only for WebGL, over the same driver (GTK and WPE do the same).
    SET_AND_EXPOSE_TO_BUILD(USE_ANGLE ${ENABLE_WEBGL})
    find_path(EGL_INCLUDE_DIRS NAMES EGL/egl.h REQUIRED)
    # No EGL/GLES library at link time: ZeroGGL resolves the driver with dlopen.
    include("${ZEROG_SOURCE_DIR}/port/gl/GLLoader.cmake")
    set(EGL_LIBRARIES ZeroGGL)
    if (NOT TARGET OpenGL::GLES)
        add_library(OpenGL::GLES INTERFACE IMPORTED)
        set_target_properties(OpenGL::GLES PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${EGL_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES ZeroGGL)
    endif ()
endif ()
SET_AND_EXPOSE_TO_BUILD(USE_EGL ON)

# Compositing version 2 (ZeroG.md, "Compositing"): upstream's coordinated graphics
# with Skia, as GTK and WPE build it (no TextureMapper). GPU views composite their
# layers on the compositor thread (port/embedding/graphics/Compositor.*).
SET_AND_EXPOSE_TO_BUILD(USE_COORDINATED_GRAPHICS ON)
