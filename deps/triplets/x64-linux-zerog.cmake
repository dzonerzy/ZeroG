# Static libraries for the single-binary build, compiled with the same zig clang,
# glibc baseline and libc++ as WebKit, so objects can be linked together.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../toolchains/linux-zig.cmake)

if (PORT STREQUAL "sdl3")
    # SDL loads X11 at run time and only needs the libraries to find their sonames.
    # CMake takes the multiarch directory from the compiler, which zig does not
    # report.
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu")
endif ()
