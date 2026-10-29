# Static libraries and static CRT (/MT) for the single-binary build, compiled with
# the pinned clang-cl (toolchain.json) like WebKit; OptionsZeroG.cmake re-sets
# CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded to match.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../toolchains/windows-clang-cl.cmake)
# A chainloaded toolchain turns the vcvars environment off by default; the SDK
# (rc.exe, headers, import libraries) is still needed.
set(VCPKG_LOAD_VCVARS_ENV ON)
if (PORT STREQUAL "harfbuzz")
    set(VCPKG_MESON_NATIVE_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/meson-cxx17.ini")
endif ()
