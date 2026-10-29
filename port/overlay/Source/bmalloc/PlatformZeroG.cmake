# ZeroG stage 1: same sources as the upstream JSCOnly port, which already uses
# RunLoopGeneric and no GLib. Diverges once the platform layer needs its own files.
include(${CMAKE_CURRENT_LIST_DIR}/PlatformJSCOnly.cmake)
