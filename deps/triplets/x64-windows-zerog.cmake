# Static libraries for the single-binary build.
# CRT stays dynamic for now: upstream OptionsMSVC.cmake forces
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL. Moving to /MT (ZeroG.md,
# "Cosa resta dinamico a runtime") needs a patch there first.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
