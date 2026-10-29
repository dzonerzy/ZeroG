# Toolchain for dependencies built by vcpkg on Linux: the builder image's clang.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
