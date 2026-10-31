# Toolchain for dependencies built by vcpkg on Linux: zig's clang through the
# builder image's wrappers, targeting the glibc baseline in ZEROG_ZIG_TARGET and
# zig's libc++, the same toolchain WebKit is built with.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER zcc)
set(CMAKE_CXX_COMPILER zc++)
set(CMAKE_AR zar CACHE FILEPATH "")
set(CMAKE_RANLIB zranlib CACHE FILEPATH "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Every function and datum in a section of its own, as the engine
# (OptionsZeroG.cmake): the link's --gc-sections then drops the unused parts of
# each library. vcpkg hands these flags to the Meson and autotools ports too.
string(APPEND CMAKE_C_FLAGS_INIT " -ffunction-sections -fdata-sections")
string(APPEND CMAKE_CXX_FLAGS_INIT " -ffunction-sections -fdata-sections")
