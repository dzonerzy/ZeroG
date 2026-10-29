# Toolchain for dependencies built by vcpkg on Windows: the pinned clang-cl from
# toolchain.json (third_party/toolchains/llvm-<version>), the same compiler as
# WebKit. vcpkg rebuilds the environment from vcvars, so the path is absolute.
get_filename_component(_zerog_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(READ "${_zerog_root}/toolchain.json" _zerog_manifest)
string(JSON _zerog_llvm GET "${_zerog_manifest}" llvm version)
set(_zerog_llvm_bin "${_zerog_root}/third_party/toolchains/llvm-${_zerog_llvm}/bin")
if (NOT EXISTS "${_zerog_llvm_bin}/clang-cl.exe")
    message(FATAL_ERROR "pinned LLVM ${_zerog_llvm} missing: run `python tools/toolchain.py fetch`")
endif ()

# Keep vcpkg's own Windows toolchain (CRT from VCPKG_CRT_LINKAGE, /utf-8, release
# flags): a chainloaded toolchain replaces it. It sets flags only, never compilers.
if (DEFINED ENV{ZEROG_VCPKG_ROOT})
    set(_zerog_vcpkg "$ENV{ZEROG_VCPKG_ROOT}")
else ()
    set(_zerog_vcpkg "${_zerog_root}/third_party/vcpkg")
endif ()
include("${_zerog_vcpkg}/scripts/toolchains/windows.cmake")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER "${_zerog_llvm_bin}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${_zerog_llvm_bin}/clang-cl.exe")
set(CMAKE_LINKER "${_zerog_llvm_bin}/lld-link.exe" CACHE FILEPATH "")
set(CMAKE_AR "${_zerog_llvm_bin}/llvm-lib.exe" CACHE FILEPATH "")
# Resource scripts (.rc, e.g. FreeType's version info): the Windows SDK's rc.exe.
# CMake would pick llvm-rc next to clang-cl, whose driver preprocesses with the C
# compiler and passes it every RC flag, including vcpkg's -c65001, which clang-cl
# rejects. rc.exe comes with the SDK the build already requires.
set(CMAKE_RC_COMPILER rc.exe CACHE FILEPATH "")
# Ninja's .rc dependency scanning (cmcldeps) preprocesses with the C compiler, so
# vcpkg's rc.exe-only /c65001 (UTF-8 code page for resource text) still reaches
# clang-cl. Drop it: the version resources of these libraries are ASCII.
set(CMAKE_RC_FLAGS "/DWIN32" CACHE STRING "" FORCE)
