# The Linux GL loader (GLLoader.cpp): EGL and GLES entry points that resolve the
# system driver at runtime. Included from OptionsZeroG.cmake; EGL_INCLUDE_DIRS is
# the Khronos headers of the sysroot.

set(_zerog_gl_headers EGL/egl.h EGL/eglext.h GLES3/gl32.h GLES2/gl2ext.h)
set(_zerog_gl_entry_points "${CMAKE_BINARY_DIR}/ZeroG/gl/GLEntryPoints.cpp")
list(TRANSFORM _zerog_gl_headers PREPEND "${EGL_INCLUDE_DIRS}/" OUTPUT_VARIABLE _zerog_gl_header_paths)

add_custom_command(
    OUTPUT "${_zerog_gl_entry_points}"
    COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_loader.py"
        "${_zerog_gl_entry_points}" "${EGL_INCLUDE_DIRS}" ${_zerog_gl_headers}
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/generate_loader.py" ${_zerog_gl_header_paths}
    VERBATIM
)

add_library(ZeroGGL STATIC
    "${CMAKE_CURRENT_LIST_DIR}/GLLoader.cpp"
    "${_zerog_gl_entry_points}"
)
target_include_directories(ZeroGGL PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EGL_INCLUDE_DIRS}")
target_compile_definitions(ZeroGGL PRIVATE
    EGL_EGL_PROTOTYPES=1 EGL_EGLEXT_PROTOTYPES=1 GL_GLES_PROTOTYPES=1 GL_GLEXT_PROTOTYPES=1)
target_link_libraries(ZeroGGL PRIVATE ${CMAKE_DL_LIBS})
