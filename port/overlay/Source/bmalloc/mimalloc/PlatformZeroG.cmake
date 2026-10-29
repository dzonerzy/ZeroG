# Included by Source/bmalloc/mimalloc/CMakeLists.txt before mimalloc's own
# CMakeLists, so mimalloc's options can be set here without patching upstream.
set(MI_SECURE ${ZEROG_MIMALLOC_SECURE} CACHE BOOL "mimalloc hardened mode (ZEROG_MIMALLOC_SECURE)" FORCE)

if (ZEROG_ZIG)
    # zig turns -Wdate-time into an error (reproducible builds); mimalloc's verbose
    # banner prints __DATE__/__TIME__ (src/options.c). Scoped to this directory.
    add_compile_options(-Wno-date-time)
endif ()
