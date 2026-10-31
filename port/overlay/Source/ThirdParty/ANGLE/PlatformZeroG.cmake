# ANGLE for the ZeroG port, linked statically into the engine.
if (WIN32)
    # The Win port's backend set (D3D11 with HLSL, plus the D3D9 sources ANGLE needs
    # to compile), as static libraries instead of DLLs.
    include(${CMAKE_CURRENT_LIST_DIR}/PlatformWin.cmake)
    set(GLESv2_LIBRARY_TYPE STATIC)
    set(EGL_LIBRARY_TYPE STATIC)
    # KHRONOS_STATIC and an empty ANGLE_EXPORT (OptionsZeroG.cmake, for ANGLE and its
    # users) keep the EGL/GLES entry points from being dllimport/dllexport.
    # Contexts in one share group are used from several threads (the compositor's,
    # the engine thread's, window presentation). ANGLE locks them, as its own
    # build does outside Chromium (BUILD.gn, angle_enable_share_context_lock),
    # and every call checks which context made the previous one: the D3D11
    # backend keeps one device context and one state cache for all GL contexts,
    # so calls alternating between threads must re-apply their context's state
    # (angle_force_context_check_every_call; it replaces the per-context mutex).
    list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_SHARE_CONTEXT_LOCK=1 ANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL=1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Only for WebGL (docs/design/graphics-presentation.md, "GPU backend per platform"): the GL backend over
    # the system's EGL, loaded at run time, as PlatformWPE.cmake builds it.
    list(APPEND ANGLE_DEFINITIONS ANGLE_PLATFORM_LINUX EGL_NO_PLATFORM_SPECIFIC_TYPES)
    include(linux.cmake)

    list(APPEND ANGLE_SOURCES
        ${gl_backend_sources}

        ${angle_system_utils_sources_linux}
        ${angle_system_utils_sources_posix}

        ${angle_dma_buf_sources}

        ${libangle_gl_egl_dl_sources}
        ${libangle_gl_egl_sources}
        ${libangle_gl_sources}

        ${libangle_gpu_info_util_sources}
        ${libangle_gpu_info_util_linux_sources}
    )

    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_OPENGL
        USE_SYSTEM_EGL
    )

    list(APPEND ANGLEGLESv2_LIBRARIES
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )

    # WebCore calls ANGLE through its prefixed entry points (GL_*, EGL_*). The
    # unprefixed gl* ones would be a second definition of the system GL's, which
    # the engine itself draws with (port/gl, ZeroGGL).
    set(libglesv2_sources "")
else ()
    message(FATAL_ERROR "ANGLE is not configured for ${CMAKE_SYSTEM_NAME} in the ZeroG port yet")
endif ()
