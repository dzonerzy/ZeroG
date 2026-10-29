# WTF for the ZeroG port: the sources of the upstream JSCOnly port, which already
# uses RunLoopGeneric and no GLib, plus what WebCore needs on top.
include(${CMAKE_CURRENT_LIST_DIR}/PlatformJSCOnly.cmake)

if (NOT WIN32)
    # Used by WebCore on Unix (shared memory, GL fences), exported by every Unix
    # port that builds WebCore.
    list(APPEND WTF_PUBLIC_HEADERS
        unix/UnixFileDescriptor.h
    )
endif ()
