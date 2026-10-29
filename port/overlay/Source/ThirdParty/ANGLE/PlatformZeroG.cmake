# ANGLE for the ZeroG port, linked statically into the engine.
if (WIN32)
    # The Win port's backend set (D3D11 with HLSL, plus the D3D9 sources ANGLE needs
    # to compile), as static libraries instead of DLLs.
    include(${CMAKE_CURRENT_LIST_DIR}/PlatformWin.cmake)
    set(GLESv2_LIBRARY_TYPE STATIC)
    set(EGL_LIBRARY_TYPE STATIC)
    # KHRONOS_STATIC and an empty ANGLE_EXPORT (OptionsZeroG.cmake, for ANGLE and its
    # users) keep the EGL/GLES entry points from being dllimport/dllexport.
else ()
    # Linux and macOS build ANGLE only for WebGL in the standard profile (ZeroG.md,
    # "Backend GPU per piattaforma"); its backends are not wired yet.
    message(FATAL_ERROR "ANGLE is not configured for ${CMAKE_SYSTEM_NAME} in the ZeroG port yet")
endif ()
