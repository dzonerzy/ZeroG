# Static libraries and static CRT (/MT) for the single-binary build;
# OptionsZeroG.cmake re-sets CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded to match.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
